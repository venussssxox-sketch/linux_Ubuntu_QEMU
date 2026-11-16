#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"

int main(void)
{
    const char message[] = "abc";
    char buffer[sizeof(message) - 1];
    char ready = 'R';
    int ready_pipe[2];
    int fd;
    int status;
    pid_t pid;
    pid_t wait_result;
    ssize_t transferred;

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    if (pipe(ready_pipe) == -1) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        close(fd);
        return 1;
    }

    if (pid == 0) {
        close(ready_pipe[0]);

        if (write(ready_pipe[1], &ready, 1) != 1) {
            fprintf(stderr, "child readiness write failed\n");
            close(ready_pipe[1]);
            close(fd);
            return 1;
        }

        close(ready_pipe[1]);
        alarm(5);

        transferred = read(fd, buffer, sizeof(buffer));
        if (transferred == -1) {
            fprintf(stderr, "blocking read failed: %s\n",
                    strerror(errno));
            close(fd);
            return 1;
        }

        alarm(0);

        if (transferred != (ssize_t)sizeof(buffer)) {
            fprintf(stderr,
                    "blocking read length: expected=%zu actual=%zd\n",
                    sizeof(buffer), transferred);
            close(fd);
            return 1;
        }

        if (memcmp(buffer, message, sizeof(buffer)) != 0) {
            fprintf(stderr, "blocking read data mismatch\n");
            close(fd);
            return 1;
        }

        close(fd);
        printf("PASS: child read %zd bytes after wake-up: %.*s\n",
               transferred, (int)transferred, buffer);
        return 0;
    }

    close(ready_pipe[1]);

    if (read(ready_pipe[0], &ready, 1) != 1) {
        fprintf(stderr, "parent readiness read failed\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(ready_pipe[0]);
        close(fd);
        return 1;
    }

    close(ready_pipe[0]);
    usleep(200000);

    wait_result = waitpid(pid, &status, WNOHANG);
    if (wait_result == -1) {
        fprintf(stderr, "waitpid WNOHANG failed: %s\n",
                strerror(errno));
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(fd);
        return 1;
    }

    if (wait_result != 0) {
        fprintf(stderr,
                "child exited before data was written; read did not block\n");
        close(fd);
        return 1;
    }

    printf("PASS: child remained blocked while FIFO was empty\n");

    transferred = write(fd, message, sizeof(message) - 1);
    if (transferred != (ssize_t)(sizeof(message) - 1)) {
        fprintf(stderr,
                "parent write: expected=%zu actual=%zd errno=%s\n",
                sizeof(message) - 1,
                transferred,
                strerror(errno));
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(fd);
        return 1;
    }

    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "final waitpid failed: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child did not exit successfully\n");
        return 1;
    }

    printf("PASS: parent write woke the blocked child\n");
    return 0;
}
