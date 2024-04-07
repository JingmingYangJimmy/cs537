#include "safequeue.h"
#include <stdlib.h>
// #include <pthread.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include "proxyserver.h"

// Function to create a new priority queue
SafePriorityQueue *create_queue(int capacity)
{
    SafePriorityQueue *q = malloc(sizeof(SafePriorityQueue));
    // Initialize the queue, capacity, mutex, and condition variable
    q->requests = malloc(sizeof(HTTPRequest) * capacity);
    q->size = 0;
    q->capacity = capacity;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

// Enqueue an item
void add_work(SafePriorityQueue *q, HTTPRequest item)
{
    pthread_mutex_lock(&q->lock);

    if (q->size == q->capacity)
    {
        send_error_response(item.client_socket, 599, "it is full!");
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->lock);
        return;
    }
    // Add the item to the queue in the correct position based on its priority
    // check queue size is available

    // Find the correct position to insert the new item based on priority
    int i;
    for (i = q->size - 1; (i >= 0 && q->requests[i].priority < item.priority); i--)
    {
        q->requests[i + 1] = q->requests[i]; // Shift item
    }
    q->requests[i + 1] = item; // Insert new item
    q->size++;

    // Signal that the queue is not empty
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

// blocking func to get the job with highext priority
HTTPRequest get_work(SafePriorityQueue *q)
{

    pthread_mutex_lock(&q->lock);
    // Wait for the queue to not be empty
    while (q->size == 0)
    {

        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    // Remove the item from the front of the queue
    HTTPRequest item = q->requests[0]; // Assuming a sorted list, the first item would be dequeued

    // shift the remaining items forward;
    memmove(&q->requests[0], &q->requests[1], sizeof(HTTPRequest) * (q->size - 1));

    q->size--;
    pthread_mutex_unlock(&q->lock);

    return item;
}
// Non-blocking function to get the highest priority job
HTTPRequest get_work_nonblocking(SafePriorityQueue *q)
{
    pthread_mutex_lock(&q->lock);
    // if no elements on the queue,send an error message
    if (q->size == 0)
    {
        pthread_mutex_unlock(&q->lock);
        HTTPRequest emptyItem; // Initialize appropriately
        memset(&emptyItem, 0, sizeof(HTTPRequest));
        return emptyItem; // return error
    }
    HTTPRequest item = q->requests[0];                                              // remove and return the higest priority item;
    memmove(&q->requests[0], &q->requests[1], sizeof(HTTPRequest) * (q->size - 1)); // shift the remaining items forward
    q->size--;

    pthread_mutex_unlock(&q->lock);
    return item;
}

// Destroy the queue and free the structure
void destroy_queue(SafePriorityQueue *q)
{
    // Clean up resources and free the data structure
    free(q->requests);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    free(q);
}