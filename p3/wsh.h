#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef WSH_H
#define WSH_H

#include <stdbool.h>

#define MAX_INPUT_SIZE 1024
#define MAX_ARG_SIZE 100

// Data structure for a job
typedef struct Job
{
    int id;
    char *command;
    pid_t pid;
    bool is_background;
    struct Job *next;
} Job;

// Function declarations

// Read a line of input from the user or batch file
char *wsh_read_line(void);

// Parse a line of input into individual arguments
char **wsh_parse_line(char *line);

// Execute a given command
void wsh_execute(char **args);

// Add a job to the jobs list
void wsh_add_job(int id, char *command, pid_t pid, bool is_background);

// Remove a job from the jobs list
void wsh_remove_job(pid_t pid);

// Find the next available job ID
int wsh_next_job_id(void);

// Print the list of jobs
void wsh_jobs(void);

// Move a process to the foreground
void wsh_fg(char **args);

// Move a process to the background
void wsh_bg(char **args);

// Built-in command handlers
void wsh_handle_exit(char **args);
void wsh_handle_cd(char **args);
// ... other built-in commands ...

#endif /* WSH_H */
