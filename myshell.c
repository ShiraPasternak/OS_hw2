#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

int isBackground(int len, char** arglist);
int getPipingSymbolIndex(int len, char** arglist);
int isPiping(int len, char** arglist);
int isRedirecting(int len, char** arglist);
int executeBackgroundProcess(int len, char** arglist);
int executePipingProcess(int len, char** arglist);
int executeRedirectingProcess(int len, char** arglist);
int executeRegularCommand(int len, char** arglist);
int handleWaitExitCode(int en);
char *getFileName(int len, char** arglist);
void safeClose(int fd);
void safeDup2(int fdSrc, int fdDes);
void safeExecvp(char *command, char **arglist);
int handleForkError();

int prepare(void){ //remove zombies and handel signals
    signal(SIGCHLD, SIG_IGN); //ERAN'S TRICK
    signal(SIGINT, SIG_IGN);
    return 0;
}
int finalize(void){
    return 0;
}

int process_arglist(int count, char** arglist){
    int error;
    if (isBackground(count, arglist)) {
        error = executeBackgroundProcess(count, arglist);
    }
    else if (isPiping(count, arglist)) {
        error = executePipingProcess(count, arglist);
    }
    else if (isRedirecting(count, arglist)) {
        error = executeRedirectingProcess(count, arglist);
    }
    else /*if (isRegular())*/{
        error = executeRegularCommand(count, arglist);
    }
    if (error == 0) {
        perror("Failed to execute command");
    }
    return error;
}

int isBackground(int len, char** arglist) {
    if (len > 1 && strcmp(arglist[len-1],"&") == 0)
        return 1;
    return 0;
}

int getPipingSymbolIndex(int len, char** arglist) {
    for (int i = 1; i < len-1; ++i) {
        if (strcmp(arglist[i],"|") == 0)
            return i;
    }
    return 0;
}

int isPiping(int len, char** arglist) {
    if (getPipingSymbolIndex(len, arglist))
        return 1;
    return 0;
}

int isRedirecting(int len, char** arglist) {
    if (len > 1 && strcmp(arglist[len-2],">") == 0)
        return 1;
    return 0;
}

int executeBackgroundProcess(int len, char** arglist) {
    signal(SIGCHLD, SIG_IGN); // ERAN'S TRICK
    pid_t pid = fork();
    if (pid == -1)
        return handleForkError();
    else if (pid == 0) { //child
        arglist[len - 1] = NULL; // preparing arglist for execvp syscall
        safeExecvp(arglist[0], arglist);
    }
    return 1;
}

int executePipingProcess(int len, char** arglist) { // general build taken from http://www.cs.loyola.edu/~jglenn/702/S2005/Examples/dup2.html
    pid_t cpid[2]; // children's pid array
    int pipefd[2];
    int isWaitFailed1, isWaitFailed2, exit_code1 = -1, exit_code2 = -1; // waitpid() method variables
    if (pipe(pipefd) == -1) {
        perror("Failed on pipe syscall");
        return 0;
    }
    cpid[0] = fork(); // creating first child
    if (cpid[0] == -1) { // close all piping files when fork fails
        safeClose(pipefd[0]);
        safeClose(pipefd[1]);
        return handleForkError();
    }
    else if (cpid[0] == 0) { // first child
        signal(SIGINT, SIG_DFL); // enable kill in first child
        safeClose(pipefd[0]); // close unused file
        safeDup2(pipefd[1], STDOUT_FILENO);
        safeClose(pipefd[1]);
        arglist[getPipingSymbolIndex(len, arglist)] = NULL; // preparing arglist for execvp syscall
        safeExecvp(arglist[0], arglist);
    }
    else { // parent
        cpid[1] = fork(); // creating second child
        if (cpid[1] == -1) { // close all piping files when fork fails
            safeClose(pipefd[0]);
            safeClose(pipefd[1]);
            return handleForkError();
        }
        else if (cpid[1] == 0) { // second child
            signal(SIGINT, SIG_DFL); // enable kill in second child
            safeClose(pipefd[1]); // close unused file
            safeDup2(pipefd[0], STDIN_FILENO);
            safeClose(pipefd[0]);
            safeExecvp(arglist[getPipingSymbolIndex(len, arglist) + 1], // second part of piping command
                       &arglist[getPipingSymbolIndex(len, arglist) + 1]); // second part of piping arglist
        }
        else { // parent, waiting for both children to finish their run
            isWaitFailed1 = waitpid(cpid[0], &exit_code1, 0);
            safeClose(pipefd[1]);
            isWaitFailed2 = waitpid(cpid[1], &exit_code2, 0);
            safeClose(pipefd[0]);
            if (isWaitFailed1 == -1 || isWaitFailed2 == -1) // handle errors in waitpid's
                return handleWaitExitCode(errno) && handleWaitExitCode(errno);

        }
    }
    return 1;
}

int executeRedirectingProcess(int len, char** arglist) { // general build taken from https://stackoverflow.com/questions/5517913/redirecting-stdout-to-file-after-a-fork
    pid_t pid;
    int exit_code = -1; // waitpid() method variable
    int fd = open(getFileName(len, arglist), O_CREAT | O_TRUNC | O_WRONLY, 0777); // from Eran code files
    if (fd == -1) {
        perror("Failed to open file for redirecting");
        return 0;
    }
    pid = fork();
    if (pid == -1)
        return handleForkError();
    else if (pid == 0) { //child
        signal(SIGINT, SIG_DFL);  // enable kill in child
        safeDup2(fd, STDOUT_FILENO);
        safeClose(fd);
        arglist[len-1] = NULL; arglist[len-2] = NULL; // preparing arglist for execvp syscall
        safeExecvp(arglist[0], arglist);
    }
    else { // parent, wait for child to finish his run
        if (waitpid(pid, &exit_code, 0) == -1)
            return handleWaitExitCode(errno);
    }
    return 1;
}

int executeRegularCommand(int len, char** arglist) { // general build taken from Eran exec_fork file
    int exit_code = -1; // waitpid() method variable
    pid_t pid = fork();
    if (pid == -1)
        return handleForkError();
    else if (pid == 0) { // child
        signal(SIGINT, SIG_DFL); // enable kill in child
        safeExecvp(arglist[0], arglist);
    }
    else { // parent, wait for child to finish his run
        if (waitpid(pid, &exit_code, 0) == -1)
            return handleWaitExitCode(errno);
    }
    return 1;
}

void safeExecvp(char *command, char **arglist) { // executing execvp syscall with error handling
    if (execvp(arglist[0], arglist) == -1) {
        perror("Failed to execute execvp syscall in child process");
        exit(1);
    }
}

void safeDup2(int fdSrc, int fdDes) { // executing dup2 syscall with error handling
    if (dup2(fdSrc, fdDes) == -1) {
        perror("Failed to execute dup2 syscall in child process");
        exit(1);
    }
}

void safeClose(int fd) { // executing close syscall with error handling
    if (close(fd) == -1) {
        perror("Failed to close file");
        exit(1);
    }
}

char *getFileName(int len, char** arglist) {
    return arglist[len-1];
}

int handleForkError() {
    perror("Failed to create child process");
    return 0;
}

int handleWaitExitCode(int en) {
    if (en == ECHILD || en == EINTR) // according to assigment description of error handling - if this two errors happen we need to handle them like no error occurred
        return 1;
    return 0;
}
