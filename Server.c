#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10
#define MAX_PROCESSES 5
#define PORT 4000
#define MAX_FILES 5

sem_t post_sem;
pthread_mutex_t log_lock;
_Atomic int client_id_counter = 1;

int socket_pairs[MAX_PROCESSES][2];

void log_request(const char *method, const char *file_add, const char *status, int client_id) {
    pthread_mutex_lock(&log_lock);

    FILE *log_file = fopen("server.log", "a");
    if (log_file == NULL) {
        perror("Error opening log file");
        pthread_mutex_unlock(&log_lock);
        return;
    }

    time_t now = time(NULL);
    char *timestamp = ctime(&now);
    timestamp[strlen(timestamp) - 1] = '\0'; // Remove newline from timestamp

    fprintf(log_file, "[%s] Method: %s, Client ID: %d, File: %s.txt, Status: %s\n", timestamp, method, client_id, file_add, status);
    fclose(log_file);

    pthread_mutex_unlock(&log_lock);
}

void send_fd(int socket, int fd) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char buf[1] = {0};
    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(fd))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(fd));

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    *((int *)CMSG_DATA(cmsg)) = fd;

    if (sendmsg(socket, &msg, 0) < 0) {
        perror("Error sending file descriptor");
    }
}

int recv_fd(int socket) {
    struct msghdr msg = {0};
    struct iovec iov[1];
    char buf[1] = {0}; // Dummy data
    iov[0].iov_base = buf;
    iov[0].iov_len = sizeof(buf);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    if (recvmsg(socket, &msg, 0) < 0) {
        perror("Error receiving file descriptor");
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
        fprintf(stderr, "Invalid file descriptor received\n");
        return -1;
    }

    int fd = *((int *)CMSG_DATA(cmsg));
    return fd;
}

typedef struct {
    char file_add[256];
    int client_socket;
    int client_id;
} GetThreadArgs;

void *thread_handle_get(void *args) {
    GetThreadArgs *get_args = (GetThreadArgs *)args;
    if (strstr(get_args->file_add, "..") != NULL) {
        const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 17\r\n\r\nInvalid file path";
        send(get_args->client_socket, response, strlen(response), 0);
        log_request("GET", get_args->file_add, "400 Bad Request", get_args->client_id);
        return NULL;
    }

    char path[256];
    snprintf(path, sizeof(path), "./static/%.240s.txt", get_args->file_add);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 14\r\n\r\nFile not found";
        send(get_args->client_socket, response, strlen(response), 0);
        log_request("GET", get_args->file_add, "404 Not Found", get_args->client_id);
        return NULL;
    }

    char file_data[BUFFER_SIZE] = {0};
    size_t bytes_read = fread(file_data, 1, sizeof(file_data) - 1, file);
    fclose(file);

    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
             bytes_read);
    send(get_args->client_socket, header, strlen(header), 0);
    send(get_args->client_socket, file_data, bytes_read, 0);

    log_request("GET", get_args->file_add, "200 OK", get_args->client_id);

    close(get_args->client_socket);
    free(get_args);
    return NULL;
}

typedef struct {
    char line[BUFFER_SIZE];
    char file_add[256];
} PostThreadArgs;

void *thread_handle_post(void *args) {
    PostThreadArgs *post_args = (PostThreadArgs *)args;
    char path[256];
    snprintf(path, sizeof(path), "./static/%.240s.txt", post_args->file_add);

    pthread_mutex_t file_mutex;
    pthread_mutex_init(&file_mutex, NULL);

    pthread_mutex_lock(&file_mutex);

    FILE *file = fopen(path, "a");
    if (file == NULL) {
        perror("Error opening file for appending");
        pthread_mutex_unlock(&file_mutex);
        pthread_mutex_destroy(&file_mutex);
        free(post_args);
        return NULL;
    }

    fprintf(file, "%s\n", post_args->line);
    fclose(file);
    pthread_mutex_unlock(&file_mutex);
    pthread_mutex_destroy(&file_mutex);
    free(post_args);
    return NULL;
}

void handle_post(char *file_add, char *data, int client_socket, int client_id) {
    if (strstr(file_add, "..") != NULL) {
        const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 17\r\n\r\nInvalid file path";
        send(client_socket, response, strlen(response), 0);
        log_request("POST", file_add, "400 Bad Request", client_id);
        return;
    }

    sem_wait(&post_sem);

    usleep(2000000);

    char *line = strtok(data, "\n");
    while (line != NULL) {
        PostThreadArgs *post_args = malloc(sizeof(PostThreadArgs));
        if (!post_args) {
            perror("Error allocating memory for POST thread arguments");
            sem_post(&post_sem);
            return;
        }

        strncpy(post_args->line, line, sizeof(post_args->line) - 1);
        post_args->line[sizeof(post_args->line) - 1] = '\0';
        strncpy(post_args->file_add, file_add, sizeof(post_args->file_add) - 1);
        post_args->file_add[sizeof(post_args->file_add) - 1] = '\0';

        pthread_t post_thread;
        if (pthread_create(&post_thread, NULL, thread_handle_post, (void *)post_args) != 0) {
            perror("Error creating thread for POST request");
            free(post_args);
            continue;
        }

        pthread_detach(post_thread);
        line = strtok(NULL, "\n");
    }

    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\nContent-Length: 27\r\n\r\nData received successfully!");
    sem_post(&post_sem);
    send(client_socket, response, strlen(response), 0);

    log_request("POST", file_add, "201 Created", client_id);
}

void worker_process(int process_id) {
    int socket = socket_pairs[process_id][0];
    while (1) {
        int client_socket = recv_fd(socket);
        if (client_socket < 0) {
            fprintf(stderr, "Error receiving file descriptor\n");
            continue;
        }

        int client_id;
        ssize_t recv__bytes = recv(socket, &client_id, sizeof(client_id), 0);
        if (recv__bytes <= 0) {
            perror("Error receiving client ID");
            close(client_socket);
            continue;
        }

        printf("Process %d is handling client ID %d\n", process_id, client_id);

        char buffer[BUFFER_SIZE] = {0};
        ssize_t recv_bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (recv_bytes <= 0) {
            perror("Error receiving data or client closed the connection");
            close(client_socket);
            continue;
        }
        buffer[recv_bytes] = '\0';

        char method[16], file_add[256];
        sscanf(buffer, "%s %s", method, file_add);
        char *body = strstr(buffer, "\r\n\r\n");
        if (body != NULL) {
            body += 4;
        } else {
            body = "";
        }

        if (strcmp(method, "GET") == 0) {
            GetThreadArgs *get_args = malloc(sizeof(GetThreadArgs));
            if (!get_args) {
                perror("Error allocating memory for GET request arguments");
                close(client_socket);
                continue;
            }

            strncpy(get_args->file_add, file_add, sizeof(get_args->file_add) - 1);
            get_args->file_add[sizeof(get_args->file_add) - 1] = '\0';
            get_args->client_socket = client_socket;
            get_args->client_id = client_id;


            pthread_t get_thread;
            if (pthread_create(&get_thread, NULL, thread_handle_get, (void *)get_args) != 0) {
                perror("Error creating thread for GET request");
                close(client_socket);
                free(get_args);
                continue;
            }

            pthread_detach(get_thread);
        } else if (strcmp(method, "POST") == 0) {
            handle_post(file_add, body, client_socket, client_id);
            close(client_socket);
        } else {
            const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 18\r\n\r\nUnsupported method";
            send(client_socket, response, strlen(response), 0);
            log_request(method, file_add, "400 Bad Request", client_id);
            close(client_socket);
        }
    }
}

int main() {
    sem_init(&post_sem, 0, 5);
    pthread_mutex_init(&log_lock, NULL);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Error creating socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("Error binding socket");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Error listening on socket");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_pairs[i]) < 0) {
            perror("Error creating socket pair");
            exit(EXIT_FAILURE);
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(socket_pairs[i][1]);
            worker_process(i);
        } else if (pid > 0) {
            close(socket_pairs[i][0]);
        } else {
            perror("Error forking process");
            exit(EXIT_FAILURE);
        }
    }

    printf("Server is running on port %d\n", PORT);
    int RB_counter = 0;
    while (1) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_socket = accept(server_socket, (struct sockaddr *)&client_address, &client_len);
        if (client_socket < 0) {
            perror("Error accepting connection");
            continue;
        }

        int client_id = atomic_fetch_add(&client_id_counter, 1); // Assign unique ID
        printf("Forwarding client ID %d to process %d\n", client_id, RB_counter);

        // Send the client ID and socket to the worker process
        send_fd(socket_pairs[RB_counter][1], client_socket);
        send(socket_pairs[RB_counter][1], &client_id, sizeof(client_id), 0);

        close(client_socket);

        RB_counter = (RB_counter + 1) % MAX_PROCESSES;
    }

    close(server_socket);
    sem_destroy(&post_sem);
    pthread_mutex_destroy(&log_lock);

    return 0;
}