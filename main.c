#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include "keyValStore.h"
#include "main.h"

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int transaction_in_progress = 0;
int active_client = -1;
Subscription subscriptions[MAX_SUBSCRIPTIONS];
int num_subscriptions = 0;

void* handle_client(void* arg) {
    int sockfd = *(int*) arg;
    char buf[BUF_SIZE];
    char key[BUF_SIZE];
    char response[BUF_SIZE];
    int nbytes;

    while ((nbytes = recv(sockfd, buf, BUF_SIZE, 0)) > 0) {
        buf[nbytes] = '\0';
        printf("Received message: %s\n", buf);

        // Parse the request
        char value[BUF_SIZE];
        int command = -1; // Initialize to invalid value

        // Check if the command is "BEG"
        if (strncasecmp(buf, "BEG", 3) == 0) {
            command = 0; // BEG command
        }
            // Check if the command is "END"
        else if (strncasecmp(buf, "END", 3) == 0) {
            command = 1; // END command
        }
            // Check if the command starts with "PUT"
        else if (strncasecmp(buf, "PUT", 3) == 0) {
            // Try to parse the command
            if (sscanf(buf, "PUT %s %s", key, value) == 2) {
                command = 2; // PUT command
            }
        }
            // Check if the command starts with "GET"
        else if (strncasecmp(buf, "GET", 3) == 0) {
            // Try to parse the command
            if (sscanf(buf, "GET %s", key) == 1) {
                command = 3; // GET command
            }
        }
            // Check if the command starts with "DEL"
        else if (strncasecmp(buf, "DEL", 3) == 0) {
            // Try to parse the command
            if (sscanf(buf, "DEL %s", key) == 1) {
                command = 4; // DEL command
            }
        }
            // Check if the command starts with "SUB"
        else if (strncasecmp(buf, "SUB", 3) == 0) {
            // Try to parse the command
            if (sscanf(buf, "SUB %s", key) == 1) {
                command = 5; // SUB command
            }
        }
            // Check if the command is "QUIT"
        else if (strncasecmp(buf, "QUIT", 4) == 0) {
            command = 6; // QUIT command
        }

        // Handle the command
        switch (command) {
            case 0: // BEG
                pthread_mutex_lock(&mutex);
                if (transaction_in_progress) {
                    snprintf(response, BUF_SIZE, "ERROR: Transaction already in progress by another client\n");
                } else {
                    transaction_in_progress = 1;
                    active_client = sockfd;
                    snprintf(response, BUF_SIZE, "Started transaction\n");
                }
                pthread_mutex_unlock(&mutex);
                break;

            case 1: // END
                pthread_mutex_lock(&mutex);
                if (!transaction_in_progress) {
                    snprintf(response, BUF_SIZE, "ERROR: No transaction in progress\n");
                } else if (active_client != sockfd) {
                    snprintf(response, BUF_SIZE, "ERROR: Not the active client in the transaction\n");
                } else {
                    transaction_in_progress = 0;
                    active_client = -1;
                    snprintf(response, BUF_SIZE, "Ended transaction\n");
                    pthread_cond_broadcast(&cond);
                }
                pthread_mutex_unlock(&mutex);
                break;

            case 2: // PUT
                pthread_mutex_lock(&mutex);
                while (transaction_in_progress && active_client != sockfd) {
                    pthread_cond_wait(&cond, &mutex);
                }
                pthread_mutex_unlock(&mutex);

                if (put(key, value) == 0) {
                    snprintf(response, BUF_SIZE, "PUT:%s:%s\n", key, value);
                    // Publish to subscribers
                    for (int i = 0; i < num_subscriptions; i++) {
                        if (strcmp(subscriptions[i].key, key) == 0) {
                            send(subscriptions[i].sockfd, response, strlen(response), 0);
                        }
                    }
                } else {
                    snprintf(response, BUF_SIZE, "PUT ERROR\n");
                }
                break;


            case 3: // GET
                pthread_mutex_lock(&mutex);
                while (transaction_in_progress && active_client != sockfd) {
                    pthread_cond_wait(&cond, &mutex);
                }
                pthread_mutex_unlock(&mutex);

                if (get(key, value) == 0) {
                    snprintf(response, BUF_SIZE, "GET:%s:%s\n", key, value);
                } else {
                    snprintf(response, BUF_SIZE, "GET ERROR\n");
                }
                break;

            case 4: // DEL
                pthread_mutex_lock(&mutex);
                while (transaction_in_progress && active_client != sockfd) {
                    pthread_cond_wait(&cond, &mutex);
                }
                pthread_mutex_unlock(&mutex);

                if (del(key) == 0) {
                    snprintf(response, BUF_SIZE, "DEL:%s\n", key);
                } else {
                    snprintf(response, BUF_SIZE, "DEL ERROR\n");
                }
                break;

            case 5: // SUB
                pthread_mutex_lock(&mutex);
                int result = subscribe(key, sockfd);
                pthread_mutex_unlock(&mutex);

                if (result == 0) {
                    snprintf(response, BUF_SIZE, "SUB:%s\n", key);
                } else {
                    snprintf(response, BUF_SIZE, "ERROR: Maximum number of subscriptions reached\n");
                }
                break;

            case 6: // QUIT
                printf("Received QUIT command, terminating client connection\n");
                pthread_mutex_lock(&mutex);
                if (transaction_in_progress && active_client == sockfd) {
                    transaction_in_progress = 0;
                    active_client = -1;
                    pthread_cond_broadcast(&cond);
                }
                pthread_mutex_unlock(&mutex);
                close(sockfd);
                free(arg);
                return NULL;

            default:
                snprintf(response, BUF_SIZE, "INVALID COMMAND\n");
                break;
        }

        // Send the response
        send(sockfd, response, strlen(response), 0);
        printf("Sent message: %s\n", response);
    }

    pthread_mutex_lock(&mutex);
    if (transaction_in_progress && active_client == sockfd) {
        transaction_in_progress = 0;
        active_client = -1;
        pthread_cond_broadcast(&cond);
    }
    pthread_mutex_unlock(&mutex);

    // Remove subscriptions for the client
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < num_subscriptions; i++) {
        if (subscriptions[i].sockfd == sockfd) {
            subscriptions[i] = subscriptions[--num_subscriptions];
            i--;
        }
    }
    pthread_mutex_unlock(&mutex);

    close(sockfd);
    free(arg);
    return NULL;
}

int main(int argc, char* argv[]) {
    int sockfd, connfd;
    struct sockaddr_in servaddr, clientaddr;

    // Initialize the key-value store
    initKeyValStore();

    // Create the socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set up the server address
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    // Bind the socket to the server address
    if (bind(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) != 0) {
        perror("socket bind failed");
        exit(EXIT_FAILURE);
    }

    // Start listening for connections
    if (listen(sockfd, 5) != 0){
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    printf("Key-value store server listening on port %d...\n", PORT);

    while (1) {
        // Accept a new connection
        socklen_t clientaddr_len = sizeof(clientaddr);
        connfd = accept(sockfd, (struct sockaddr *) &clientaddr, &clientaddr_len);
        if (connfd == -1) {
            perror("accept failed");
            exit(EXIT_FAILURE);
        }

        // Send a welcome message to the client
        char welcome_message[BUF_SIZE];
        snprintf(welcome_message, BUF_SIZE, "Welcome to the Key-Value Server!\n\n"
                                            "This server allows you to store and retrieve key-value pairs using the following functions:\n\n"
                                            "| Function | Description                        | Usage         |\n"
                                            "|----------|------------------------------------|---------------|\n"
                                            "| BEG      | Starts a transaction               | BEG           |\n"
                                            "| END      | Ends a transaction                 | END           |\n"
                                            "| PUT      | Stores a key-value pair in database| PUT key val   |\n"
                                            "| GET      | Retrieves value for a given key    | GET key res   |\n"
                                            "| DEL      | Removes a key-value pair from db   | DEL key       |\n"
                                            "| SUB      | Subscribes to updates for a key    | SUB key       |\n\n"
                                            "To use these functions in your code, simply call them with the appropriate arguments as shown.\n"
                                            "Thank you for using our Key-Value Server!\n");
        send(connfd, welcome_message, strlen(welcome_message), 0);

        // Create a new thread to handle the connection
        int* arg = malloc(sizeof(*arg));
        if (arg == NULL) {
            perror("malloc failed");
            exit(EXIT_FAILURE);
        }
        *arg = connfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, arg) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
    }

    return 0;
}
