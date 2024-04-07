// Demo proxyserver.c
//  Originally morning
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "proxyserver.h"
#include "safequeue.h"

/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000
SafePriorityQueue *job_queue;
void serve_request(HTTPRequest req);
void serve_forever(int *server_fd, int index);

// listener
void *port_list_struct(void *arg)
{
    int *index_ptr = (int *)arg;
    int index = *index_ptr;
    free(arg);

    int server_fd;
    serve_forever(&server_fd, index);

    return NULL;
}

// worker
void *worker_list_struct(void *arg)
{
    while (1)
    {
        HTTPRequest req = get_work(job_queue);

        if (req.delay > 0)
        {
            sleep(req.delay);
        }

        if (req.client_socket < 0)
        {
            continue;
        }
        serve_request(req);
    }
    return NULL;
}

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;

void send_error_response(int client_fd, status_code_t err_code, char *err_msg)
{
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}

/*
 * forward the client request to the fileserver and
 * forward the fileserver response to the client
 */
// worker
void serve_request(HTTPRequest req)
{

    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1)
    {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

    // connect to the fileserver
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0)
    {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(req.client_socket, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    printf("connected to fileserver successfully\n");

    // successfully connected to the file server

    char *buffer = (char *)malloc(RESPONSE_BUFSIZE * sizeof(char));

    int bytes_read = req.bytes_read;

    // int ret = http_send_data(fileserver_fd, buffer, bytes_read);
    int ret = http_send_data(fileserver_fd, req.read_buffer, bytes_read); // 0///////////

    printf("forwarded client request to fileserver\n");
    if (ret < 0)
    {
        printf("Failed to send request to the file server\n");
        send_error_response(req.client_socket, BAD_GATEWAY, "Bad Gateway");
    }
    else
    {
        // forward the fileserver response to the client
        while (1)
        {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            ret = http_send_data(req.client_socket, buffer, bytes_read);
            if (ret < 0)
            { // write failed, client_fd has been closed
                break;
            }
        }
        printf("forwarded fileserver response to the client\n");
    }

    // close the connection to the fileserver
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);

    // Free resources and exit
    free(buffer);
}

// int *server_fd;

/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
// listener
void serve_forever(int *server_fd, int index)
{
    // create a socket to listen
    *server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (*server_fd == -1)
    {
        perror("Failed to create a new socket");
        exit(errno);
    }
    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1)
    {
        perror("Failed to set socket options");
        exit(errno);
    }

    int proxy_port = listener_ports[index];
    // create the full address of this proxyserver
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(*server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1)
    {
        perror("Failed to bind on socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(*server_fd, 1024) == -1)
    {
        perror("Failed to listen on socket");
        exit(errno);
    }

    printf("Listening on port %d...\n", proxy_port);

    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    while (1)
    {
        client_fd = accept(*server_fd,
                           (struct sockaddr *)&client_address,
                           (socklen_t *)&client_address_length);
        if (client_fd < 0)
        {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);

        //

        HTTPRequest request;
        parse_client_request(client_fd, &request);
        request.client_socket = client_fd;
        if (strstr(request.path, "GetJob") != NULL)
        {

            if (job_queue->size == 0) // empty size
            {
                send_error_response(client_fd, QUEUE_EMPTY, "No jobs");
                close(client_fd);
            }
            else
            {
                HTTPRequest a = get_work(job_queue);

                send_error_response(client_fd, OK, a.path);
                close(client_fd);
            }
        }
        else if (strstr(request.path, "html") != NULL)
        {
            if (job_queue->size == max_queue_size) // full size
            {
                send_error_response(client_fd, QUEUE_FULL, "Full jobs");
                close(client_fd);
            }
            else // successfully add work
            {
                if (job_queue->size == job_queue->capacity)
                {
                    send_error_response(client_fd, 599, "it is full!");
                }
                else
                {
                    add_work(job_queue, request);
                }
            }
        }
        else // bad request
        {
            send_error_response(client_fd, BAD_REQUEST, "Bad request");
        }
    }

    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
    return;
}

void print_settings()
{
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_listener);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum)
{
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++)
    {
        // if (close(server_fd) < 0)
        //     perror("Failed to close server_fd (ignoring)\n");
    }
    if (listener_ports != NULL)
    {
        free(listener_ports);
    }
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage()
{
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{

    signal(SIGINT, signal_callback_handler);

    int i;
    for (i = 1; i < argc; i++)
    {
        if (strcmp("-l", argv[i]) == 0)
        {
            num_listener = atoi(argv[++i]);
            if (listener_ports != NULL)
            {
                free(listener_ports);
            }
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++)
            {
                listener_ports[j] = atoi(argv[++i]);
            }
        }
        else if (strcmp("-w", argv[i]) == 0)
        {
            num_workers = atoi(argv[++i]);
        }
        else if (strcmp("-q", argv[i]) == 0)
        {
            max_queue_size = atoi(argv[++i]);
        }
        else if (strcmp("-i", argv[i]) == 0)
        {
            fileserver_ipaddr = argv[++i];
        }
        else if (strcmp("-p", argv[i]) == 0)
        {
            fileserver_port = atoi(argv[++i]);
        }
        else
        {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }
    // create the queue
    job_queue = create_queue(max_queue_size);

    print_settings();

    pthread_t *worker_list = malloc(num_workers * sizeof(pthread_t));
    for (int i = 0; i < num_workers; i++)
    {
        if (pthread_create(&worker_list[i], NULL, worker_list_struct, NULL) != 0)
        {
            perror("Failed to create worker thread");
        }
    }

    pthread_t *port_list = malloc(num_listener * sizeof(pthread_t));

    for (int i = 0; i < num_listener; i++)
    {
        int *index = malloc(sizeof(int));
        *index = i;
        if (pthread_create(&port_list[i], NULL, port_list_struct, index) != 0) // port_list[i]
        {
            perror("Failed to create listener thread");
        }
    }

    for (int i = 0; i < num_listener; i++)
    {
        pthread_join(port_list[i], NULL);
    }

    for (int i = 0; i < num_workers; i++)
    {
        pthread_join(worker_list[i], NULL);
    }

    free(listener_ports);
    free(port_list);

    destroy_queue(job_queue);
    return EXIT_SUCCESS;
}
