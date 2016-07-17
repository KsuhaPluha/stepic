#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>

int pid_restore(const char *pid_path){
    int fd = open( pid_path, O_RDONLY );
    if (fd >= 0){
        char buffer[8];
        ssize_t bytes;
        if ((bytes = read(fd, buffer, sizeof(buffer))<=0))
            perror("pid.read");

        if (close(fd) < 0)
            perror("pid.close");
        return atoi(buffer);
    }
    else {
        perror("pid.open");
    }
}
int main(int argc, char **argv){
    pid_t pid = pid_restore("http.pid");
    if (pid > 0){
        kill( pid, SIGTERM );
    }
    exit(EXIT_SUCCESS);
}