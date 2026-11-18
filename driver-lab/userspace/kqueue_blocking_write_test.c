#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define FIFO_CAPACITY 4096

int main(void)
{
    const char message[] = "XYZ";
    char fill[FIFO_CAPACITY];
    char drained[FIFO_CAPACITY];
    char ready = 'R';
    int ready_pipe[2];
    int fd;
    int status;
    pid_t pid;
    pid_t wait_result;
    ssize_t transferred;

    memset(fill, 'F', sizeof(fill));

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    transferred = write(fd, fill, sizeof(fill));
    if (transferred != (ssize_t)sizeof(fill)) {
        fprintf(stderr, "initial fill: expected=%zu actual=%zd errno=%s\n",
                sizeof(fill), transferred, strerror(errno));
        close(fd);
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
        transferred = write(fd, message, sizeof(message) - 1);
        if (transferred != (ssize_t)(sizeof(message) - 1)) {
            fprintf(stderr, "blocking write: expected=%zu actual=%zd errno=%s\n",
                    sizeof(message) - 1, transferred, strerror(errno));
            close(fd);
            return 1;
        }

        alarm(0);
        close(fd);
        printf("PASS: child wrote %zd bytes after wake-up: %.*s\n",
               transferred, (int)transferred, message);
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
        fprintf(stderr, "waitpid WNOHANG failed: %s\n", strerror(errno));
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(fd);
        return 1;
    }

    if (wait_result != 0) {
        fprintf(stderr,
                "child exited before space was available; write did not block\n");
        close(fd);
        return 1;
    }

    printf("PASS: child remained blocked while FIFO was full\n");

    transferred = read(fd, drained, sizeof(message) - 1);
    if (transferred != (ssize_t)(sizeof(message) - 1) ||
        memcmp(drained, fill, sizeof(message) - 1) != 0) {
        fprintf(stderr, "parent drain before wake-up failed\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(fd);
        return 1;
    }

    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "final waitpid failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child did not exit successfully\n");
        close(fd);
        return 1;
    }

    transferred = read(fd, drained, sizeof(drained));
    if (transferred != (ssize_t)sizeof(drained) ||
        memcmp(drained, fill, FIFO_CAPACITY - (sizeof(message) - 1)) != 0 ||
        memcmp(drained + FIFO_CAPACITY - (sizeof(message) - 1),
               message, sizeof(message) - 1) != 0) {
        fprintf(stderr, "remaining FIFO data did not match expected order\n");
        close(fd);
        return 1;
    }

    if (close(fd) == -1) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return 1;
    }

    printf("PASS: parent read created space and woke the child\n");
    return 0;
}
