#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

typedef struct Job
{
    int id;
    char *command;
    pid_t pid;
    struct Job *next;
    int background;
} Job;

void execute_command(char **args);
void print_jobs(void);
void handle_fg(char **args);
void handle_bg(char **args);
void sigchld_handler(int signo);
void sigtstp_handler(int signo);
void sigint_handler(int signo);

// Job *jobs[256];
int job_count = 0;
Job *jobList = NULL; // Head of linked list

char *path[256] = {"/bin"};

int main(int argc, char *argv[])
{
    char *input_line = NULL; // input that computer gets
    size_t len = 0;
    FILE *input_stream = stdin; // input from user
    int file = 0;
    signal(SIGCHLD, sigchld_handler);
    signal(SIGTSTP, sigtstp_handler); // ctrl-z stop
    signal(SIGINT, sigint_handler);   // ctrl-c terminate

    if (argc > 2)
    {
        printf("too much arguments");
        exit(-1); // failure
    }
    if (argc == 2)
    {
        input_stream = fopen(argv[1], "r");
        file = 1;
        if (input_stream == NULL)
        {
            printf("invalid filename");
            exit(-1); // failure
        }
    }

    while (1)
    {
        printf("wsh> ");
        fflush(stdout); // ensures that the prompt is displayed right away

        if (getline(&input_line, &len, input_stream) == -1) 
        {
            if (file == 1)
            {
                fclose(input_stream);
            }
            printf("successd, no more input");
            exit(0);
        }

        char *token = NULL;
        char *args[256] = {"\0"};
        int count = 0;
        char *input = strdup(input_line); // duplicate it into modifiable memory

        token = strtok(input, " \t\n"); // any occurance of " ", \t, and \n

        while (token != NULL)
        {
            args[count] = token;
            token = strtok(NULL, " \t\n");
            count++;
        }
        args[count] = NULL; // add a null terminator

        if (args[0] != NULL && strcmp(args[0], "exit") == 0) // exit
        {
            free(input_line);
            free(input);

            if (file == 1)
            {
                fclose(input_stream);
            }
            exit(0); // success
        }

        if (args[0] != NULL && ((strcmp(args[0], "cd") == 0) || strcmp(args[0], "jobs") == 0 ||
                                strcmp(args[0], "fg") == 0 || strcmp(args[0], "bg") == 0))
        { // build-in commands

            if (args[0] != NULL && strcmp(args[0], "cd") == 0) // cd
            {

                if (count != 2)
                {
                    perror("cd too many or too little arguemnts");
                    continue;
                }
                if (access(args[1], X_OK) == -1)
                {
                    perror("can not find the file in cd");
                }
                if (chdir(args[1]) != 0) // success to use cd
                {
                    perror("cd not successful");
                }
            }
            if (args[0] != NULL && strcmp(args[0], "jobs") == 0) // jobs
            {
                print_jobs();
            }
            if (strcmp(args[0], "fg") == 0) // foreground
            {
                handle_fg(args);
            }
            if (strcmp(args[0], "bg") == 0) // background
            {
                handle_bg(args);
            }
            continue; // biult-in command does not have fork
        }
        execute_command(args);
    }
}

Job *addJob(pid_t pid, char *command)
{
    Job *newJob = malloc(sizeof(Job));
    newJob->pid = pid;
    newJob->command = strdup(command);
    newJob->next = jobList;
    if (!jobList)
    {
        newJob->id = 1;
    }
    else
    {
        newJob->id = jobList->id + 1; // Adjust as needed to handle finding the smallest unused ID
    }
    jobList = newJob;
    return newJob;
}

Job *findJob(int id)
{
    Job *job = jobList;
    while (job)
    {
        if (job->id == id)
            return job;
        job = job->next;
    }
    return NULL;
}

void removeJob(int id)
{
    Job *job = jobList, *prev = NULL;
    while (job)
    {
        if (job->id == id)
        {
            if (prev)
                prev->next = job->next;
            else
                jobList = job->next;
            free(job->command);
            free(job);
            return;
        }
        prev = job;
        job = job->next;
    }
}

void execute_command(char **args)
{
    if (!args[0])
    {
        return;
    }
    pid_t pid;
    int status;
    char command[256];
    int background = 0; // refresh it to 0
    int count = 0;
    // Check if the last argument is '&' indicating background job
    while (args[count])
    {
        count++;
    }
    if (count > 0 && strcmp(args[count - 1], "&") == 0)
    {
        background = 1;         // background job
        args[count - 1] = NULL; // Remove the '&' from args
        count--;
    }

    snprintf(command, sizeof(command), "%s/%s", path[0], args[0]);

    if (access(command, X_OK) == -1)
    {
        return;
    }

    pid = fork();

    if (background == 1) // if it is background
    {
        int total_len = 0;
        for (int i = 0; args[i] != NULL; i++)
        {
            total_len += strlen(args[i]);
        }

        // concatenated is the whole thigns without &
        char *concatenated = malloc(total_len + 1 + 256); // add 256 for spaces between words

        concatenated[0] = '\0'; // Start with an empty string

        for (int i = 0; args[i] != NULL; i++)
        {
            strcat(concatenated, args[i]);
            strcat(concatenated, " "); // Separate words with a space
        }
        strcat(concatenated, " &");
        Job *newJob = addJob(pid, concatenated); // add job into the list
        free(concatenated);
        newJob->background = background;
        // setpgid(pid, 0); // set process group id so it ignores terminal signals
    }

    if (pid == 0) // child process.
    {
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (execvp(args[0], args) == -1) // can not execute
        {
            perror("not such command");
        }
        exit(-1); // never be println
    }
    else if (pid < 0) // error
    {
        perror("fork has error");
    }

    else // parent process
    {
        if (background == 0) // only foregound need to wait
        {
            do
            {
                waitpid(pid, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
        }
    }
    return;
}

void handle_fg(char **args)
{
    int id = atoi(args[1]);
    if (id == 0)
    {
        printf("can not be transformed into number");
        return;
    }
    Job *job = findJob(id);
    if (job == NULL)
    {
        printf("no such jobs fg\n");
        return;
    }
    tcsetpgrp(STDIN_FILENO, job->pid);
    kill(job->pid, SIGCONT);
    // tcsetpgrp(STDIN_FILENO, job->pid);
    waitpid(job->pid, NULL, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpid());
    removeJob(job->id); // remove it from background
    job->background = 0;
    // tcsetpgrp(STDIN_FILENO, getpid());
    // tcsetpgrp(STDIN_FILENO, getpid());
}

void handle_bg(char **args)
{
    int id = atoi(args[1]);
    if (id == 0)
    {
        printf("can not be transformed into number");
        return;
    }
    Job *job = findJob(id);
    if (job == NULL)
    {
        printf("no such jobs bg\n");
        return;
    }

    kill(job->pid, SIGCONT); // continue to run (background)
}

void print_jobs()
{
    Job *current = jobList;
    while (current)
    {
        printf("%d: %s\n", current->id, current->command);
        current = current->next;
    }
}

void sigchld_handler(int signo)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        removeJob(pid);
    }
    tcsetpgrp(STDIN_FILENO, getpid()); // Ensure shell regains control after a child process stops or exits
}

void sigtstp_handler(int signo) // ctrl-z
{

    pid_t fg_pid = tcgetpgrp(STDIN_FILENO);
    Job *job = findJob(getpid());

    if (job && job->background == 0) // if it is foreground
    {

        kill(fg_pid, SIGTSTP); // Send stop signal to foreground job
        job->background = 1;
        if (job->command && *job->command)
        {
            memmove(job->command, job->command + 1, strlen(job->command));
        }
        addJob(fg_pid, job->command); //
    }
    // Detach the terminal from the foreground job
    tcsetpgrp(STDIN_FILENO, getpid());
}

void sigint_handler(int signo) // ctrl-c
{
    pid_t fg_pid = tcgetpgrp(STDIN_FILENO);
    Job *job = findJob(fg_pid);
    if (job && job->background == 0) // if it is foreground
    {
        kill(fg_pid, SIGINT); // Send interrupt signal to foreground job
    }
}
