#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"

static volatile sig_atomic_t got_signal;

static void handle_sigusr1(int signal_number)
{
    (void)signal_number;
    got_signal = 1;
}

int main(void)
{
    char buffer[1];
    char ready = 'R';
    int ready_pipe[2];
    int status;
    pid_t pid;
    pid_t wait_result;
    ssize_t transferred;
    struct sigaction action;

    if (pipe(ready_pipe) == -1) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        return 1;
    }

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return 1;
    }

    if (pid == 0) {
        int fd;

        close(ready_pipe[0]);
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_sigusr1;
        sigemptyset(&action.sa_mask);

        if (sigaction(SIGUSR1, &action, NULL) == -1) {
            fprintf(stderr, "sigaction failed: %s\n", strerror(errno));
            close(ready_pipe[1]);
            return 1;
        }

        fd = open(DEVICE_PATH, O_RDWR);
        if (fd == -1) {
            fprintf(stderr, "child open failed: %s\n", strerror(errno));
            close(ready_pipe[1]);
            return 1;
        }

        if (write(ready_pipe[1], &ready, 1) != 1) {
            fprintf(stderr, "child readiness write failed\n");
            close(ready_pipe[1]);
            close(fd);
            return 1;
        }

        close(ready_pipe[1]);
        alarm(5);
        errno = 0;
        transferred = read(fd, buffer, sizeof(buffer));
        alarm(0);

        if (transferred != -1 || errno != EINTR || !got_signal) {
            fprintf(stderr,
                    "expected read=-1 and errno=EINTR; read=%zd errno=%s signal=%d\n",
                    transferred, strerror(errno), got_signal);
            close(fd);
            return 1;
        }

        close(fd);
        printf("PASS: blocked read returned EINTR after SIGUSR1\n");
        return 0;
    }

    close(ready_pipe[1]);

    if (read(ready_pipe[0], &ready, 1) != 1) {
        fprintf(stderr, "parent readiness read failed\n");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        close(ready_pipe[0]);
        return 1;
    }

    close(ready_pipe[0]);
    usleep(200000);

    wait_result = waitpid(pid, &status, WNOHANG);
    if (wait_result == -1) {
        fprintf(stderr, "waitpid WNOHANG failed: %s\n", strerror(errno));
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    if (wait_result != 0) {
        fprintf(stderr,
                "child exited before SIGUSR1 was sent; read did not block\n");
        return 1;
    }

    printf("PASS: child remained blocked before SIGUSR1\n");

    if (kill(pid, SIGUSR1) == -1) {
        fprintf(stderr, "kill SIGUSR1 failed: %s\n", strerror(errno));
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "final waitpid failed: %s\n", strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child did not exit successfully\n");
        return 1;
    }

    printf("PASS: parent SIGUSR1 interrupted the blocked child\n");
    return 0;
}
