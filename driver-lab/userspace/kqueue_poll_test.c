#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define FIFO_CAPACITY 4096

static int expect_poll_state(int fd, short expected, short unexpected,
                             const char *name)
{
    struct pollfd descriptor = {
        .fd = fd,
        .events = POLLIN | POLLOUT,
        .revents = 0,
    };
    int result;

    result = poll(&descriptor, 1, 0);
    if (result != 1 || (descriptor.revents & expected) != expected ||
        (descriptor.revents & unexpected) != 0) {
        fprintf(stderr,
                "%s: poll=%d expected=%#x revents=%#x\n",
                name, result, expected, descriptor.revents);
        return 1;
    }

    printf("PASS: %s revents=%#x\n", name, descriptor.revents);
    return 0;
}

int main(void)
{
    const char message[] = "abc";
    const char notification[] = "Z";
    const char write_notification[] = "W";
    char fill[FIFO_CAPACITY - (sizeof(message) - 1)];
    char full[FIFO_CAPACITY];
    char buffer[FIFO_CAPACITY];
    struct pollfd descriptor;
    int fd;
    int status;
    int result;
    pid_t pid;
    ssize_t transferred;

    memset(fill, 'F', sizeof(fill));
    memset(full, 'Q', sizeof(full));

    fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    if (expect_poll_state(fd, POLLOUT, POLLIN, "empty FIFO") != 0)
        goto fail;

    transferred = write(fd, message, sizeof(message) - 1);
    if (transferred != (ssize_t)(sizeof(message) - 1)) {
        fprintf(stderr, "write abc failed: %s\n", strerror(errno));
        goto fail;
    }

    if (expect_poll_state(fd, POLLIN | POLLOUT, 0,
                          "non-empty, non-full FIFO") != 0)
        goto fail;

    transferred = write(fd, fill, sizeof(fill));
    if (transferred != (ssize_t)sizeof(fill)) {
        fprintf(stderr, "fill FIFO failed: %s\n", strerror(errno));
        goto fail;
    }

    if (expect_poll_state(fd, POLLIN, POLLOUT, "full FIFO") != 0)
        goto fail;

    transferred = read(fd, buffer, 1);
    if (transferred != 1 || buffer[0] != 'a') {
        fprintf(stderr, "read one byte from full FIFO failed\n");
        goto fail;
    }

    if (expect_poll_state(fd, POLLIN | POLLOUT, 0,
                          "FIFO after one-byte read") != 0)
        goto fail;

    transferred = read(fd, buffer, sizeof(buffer));
    if (transferred != FIFO_CAPACITY - 1) {
        fprintf(stderr, "drain FIFO: expected=%d actual=%zd\n",
                FIFO_CAPACITY - 1, transferred);
        goto fail;
    }

    if (expect_poll_state(fd, POLLOUT, POLLIN, "drained FIFO") != 0)
        goto fail;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, 200);
    if (result != 0 || descriptor.revents != 0) {
        fprintf(stderr, "empty FIFO POLLIN timeout: result=%d revents=%#x\n",
                result, descriptor.revents);
        goto fail;
    }

    printf("PASS: empty FIFO POLLIN wait timed out\n");

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        goto fail;
    }

    if (pid == 0) {
        int child_fd;

        usleep(200000);
        child_fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
        if (child_fd == -1 ||
            write(child_fd, notification, sizeof(notification) - 1) != 1) {
            fprintf(stderr, "child write failed: %s\n", strerror(errno));
            if (child_fd != -1)
                close(child_fd);
            return 1;
        }

        close(child_fd);
        return 0;
    }

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, 2000);
    if (result != 1 || (descriptor.revents & POLLIN) == 0) {
        fprintf(stderr, "poll wait: result=%d revents=%#x errno=%s\n",
                result, descriptor.revents, strerror(errno));
        waitpid(pid, NULL, 0);
        goto fail;
    }

    transferred = read(fd, buffer, 1);
    if (transferred != 1 || buffer[0] != notification[0]) {
        fprintf(stderr, "read notification failed\n");
        waitpid(pid, NULL, 0);
        goto fail;
    }

    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child did not exit successfully\n");
        goto fail;
    }

    transferred = write(fd, full, sizeof(full));
    if (transferred != (ssize_t)sizeof(full)) {
        fprintf(stderr, "fill FIFO for POLLOUT wait failed: %s\n",
                strerror(errno));
        goto fail;
    }

    pid = fork();
    if (pid == -1) {
        fprintf(stderr, "fork for POLLOUT wait failed: %s\n", strerror(errno));
        goto fail;
    }

    if (pid == 0) {
        int child_fd;
        char consumed;

        usleep(200000);
        child_fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
        if (child_fd == -1 || read(child_fd, &consumed, 1) != 1 ||
            consumed != full[0]) {
            fprintf(stderr, "child read for POLLOUT wait failed: %s\n",
                    strerror(errno));
            if (child_fd != -1)
                close(child_fd);
            return 1;
        }

        close(child_fd);
        return 0;
    }

    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    result = poll(&descriptor, 1, 2000);
    if (result != 1 || (descriptor.revents & POLLOUT) == 0) {
        fprintf(stderr, "POLLOUT wait: result=%d revents=%#x errno=%s\n",
                result, descriptor.revents, strerror(errno));
        waitpid(pid, NULL, 0);
        goto fail;
    }

    transferred = write(fd, write_notification,
                        sizeof(write_notification) - 1);
    if (transferred != (ssize_t)(sizeof(write_notification) - 1)) {
        fprintf(stderr, "write after POLLOUT failed: %s\n", strerror(errno));
        waitpid(pid, NULL, 0);
        goto fail;
    }

    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "POLLOUT child did not exit successfully\n");
        goto fail;
    }

    transferred = read(fd, buffer, sizeof(buffer));
    if (transferred != FIFO_CAPACITY ||
        memcmp(buffer, full, FIFO_CAPACITY - 1) != 0 ||
        buffer[FIFO_CAPACITY - 1] != write_notification[0]) {
        fprintf(stderr, "FIFO order after POLLOUT wake-up is incorrect\n");
        goto fail;
    }

    close(fd);
    printf("PASS: POLLIN wait woke after another process wrote Z\n");
    printf("PASS: POLLOUT wait woke after another process read one byte\n");
    return 0;

fail:
    close(fd);
    return 1;
}
