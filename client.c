#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 4000
#define BUFFER_SIZE 1024
#define NUM_REQUESTS 10
#define REPEAT_COUNT 1000

void *send_post_request(void *arg) {
    int request_num = *(int *)arg;
    free(arg);

    for (int i = 0; i < REPEAT_COUNT; i++) {
        int client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket < 0) {
            perror("Error creating socket");
            pthread_exit(NULL);
        }

        struct sockaddr_in server_address;
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(SERVER_PORT);
        server_address.sin_addr.s_addr = inet_addr(SERVER_IP);

        if (connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
            perror("Error connecting to server");
            close(client_socket);
            pthread_exit(NULL);
        }

        char request[BUFFER_SIZE];
        snprintf(request, sizeof(request),
                 "POST /file%d HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %d\r\n\r\n"
                 "This is test data for POST request number %d.",
                 request_num, SERVER_IP, SERVER_PORT, 42, request_num);

        if (send(client_socket, request, strlen(request), 0) < 0) {
            perror("Error sending POST request");
            close(client_socket);
            pthread_exit(NULL);
        }

        char response[BUFFER_SIZE];
        ssize_t bytes_received = recv(client_socket, response, sizeof(response) - 1, 0);
        if (bytes_received < 0) {
            perror("Error receiving response");
        } else {
            response[bytes_received] = '\0';
            printf("Response for request %d iteration %d:\n%s\n", request_num, i + 1, response);
        }

        close(client_socket);
    }

    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_REQUESTS];

    for (int i = 0; i < NUM_REQUESTS; i++) {
        int *request_num = malloc(sizeof(int));
        if (!request_num) {
            perror("Error allocating memory for request number");
            exit(EXIT_FAILURE);
        }

        *request_num = i + 1;

        if (pthread_create(&threads[i], NULL, send_post_request, request_num) != 0) {
            perror("Error creating thread");
            free(request_num);
        }

    }

    pause();

    return 0;
}
