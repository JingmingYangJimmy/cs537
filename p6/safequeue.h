#ifndef SAFE_PQUEUE_H
#define SAFE_PQUEUE_H

#include <pthread.h>

// The data type for the items in the queue
typedef struct
{
    char real_request_body[256]; // real request body
    int client_socket;
    char request_body[256]; // parse http request
    int priority;
    int delay;
    int bytes_read;
    char read_buffer[256];
    char path[9999];

} HTTPRequest;

typedef struct
{
    HTTPRequest *requests; // Array of HTPRequest items
    int size;
    int capacity;             // Maximun capacity of the queue
    pthread_mutex_t lock;     // Mutex for synchronization
    pthread_cond_t not_empty; // Condition variable for signaling not empty

} SafePriorityQueue;

// Function to initialize the priority queue
SafePriorityQueue *create_queue(int capacity);

// Function to add an item to the priority queue
void add_work(SafePriorityQueue *q, HTTPRequest item);

// Function to remove and return the highest priority item from the queue
HTTPRequest get_work(SafePriorityQueue *q);

// Function to retur the
HTTPRequest get_work_nonblocking(SafePriorityQueue *q);

// Function to destroy the priority queue and free its resources
void destroy_queue(SafePriorityQueue *q);

#endif // SAFE_PQUEUE_H
