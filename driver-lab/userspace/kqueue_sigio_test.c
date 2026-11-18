#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"

static volatile sig_atomic_t got_sigio;
static volatile sig_atomic_t timed_out;

static void handle_sigio(int signal_number)
{
    (void)signal_number;
    got_sigio = 1;
}

static void handle_alarm(int signal_number)
{
    (void)signal_number;
    timed_out = 1;
}

int main(void)
{
    char byte = '\0';
    int fd;
    int flags;
    int status;
    pid_t pid;
    sigset_t blocked;
    sigset_t old_mask;
    struct sigaction action;
    ssize_t transferred;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigio;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGIO, &action, NULL) == -1) {
        fprintf(stderr, "sigaction(SIGIO) failed: %s\n", strerror(errno));
        return 1;
    }

    action.sa_handler = handle_alarm;
    if (sigaction(SIGALRM, &action, NULL) == -1) {
        fprintf(stderr, "sigaction(SIGALRM) failed: %s\n", strerror(errno));
        return 1;
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGIO);
    sigaddset(&blocked, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &blocked, &old_mask) == -1) {
        fprintf(stderr, "sigprocmask failed: %s\n", strerror(errno));
        return 1;
    }

    fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    if (fcntl(fd, F_SETOWN, getpid()) == -1) {
        fprintf(stderr, "fcntl(F_SETOWN) failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_ASYNC) == -1) {
        fprintf(stderr, "fcntl(O_ASYNC) failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (pid == 0) {
        int writer_fd = open(DEVICE_PATH, O_WRONLY);

        if (writer_fd == -1 || write(writer_fd, "Z", 1) != 1) {
            fprintf(stderr, "child write failed: %s\n", strerror(errno));
            if (writer_fd != -1)
                close(writer_fd);
            return 1;
        }

        close(writer_fd);
        return 0;
    }

    alarm(3);
    while (!got_sigio && !timed_out)
        sigsuspend(&old_mask);
    alarm(0);

    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !got_sigio || timed_out) {
        fprintf(stderr, "SIGIO was not received before timeout\n");
        close(fd);
        return 1;
    }

    transferred = read(fd, &byte, 1);
    if (transferred != 1 || byte != 'Z') {
        fprintf(stderr, "expected to read Z, read=%zd byte=%d errno=%s\n",
                transferred, byte, strerror(errno));
        close(fd);
        return 1;
    }

    if (close(fd) == -1) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return 1;
    }

    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
        fprintf(stderr, "sigprocmask restore failed: %s\n", strerror(errno));
        return 1;
    }

    printf("PASS: SIGIO reported newly readable data and read returned Z\n");
    return 0;
}
