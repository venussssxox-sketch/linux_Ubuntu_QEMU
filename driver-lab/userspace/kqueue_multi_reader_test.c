#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define READER_COUNT 4
#define BYTES_PER_READER 4096
#define OPEN_CLOSE_ROUNDS 1000

static int reader_process(unsigned char expected)
{
    unsigned char buffer[127];
    size_t received = 0;
    int fd = open(DEVICE_PATH, O_RDONLY);

    if (fd == -1)
        return 2;
    while (received < BYTES_PER_READER) {
        size_t remaining = BYTES_PER_READER - received;
        size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t count = read(fd, buffer, request);
        size_t index;

        if (count == -1 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(fd);
            return 3;
        }
        for (index = 0; index < (size_t)count; index++) {
            if (buffer[index] != expected) {
                close(fd);
                return 4;
            }
        }
        received += (size_t)count;
    }
    return close(fd) == 0 ? 0 : 5;
}

int main(void)
{
    unsigned char payload[251];
    pid_t readers[READER_COUNT];
    size_t total = READER_COUNT * BYTES_PER_READER;
    size_t sent = 0;
    int writer = -1;
    int index;

    memset(payload, 0x5a, sizeof(payload));
    for (index = 0; index < OPEN_CLOSE_ROUNDS; index++) {
        int fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
        if (fd == -1 || close(fd) == -1) {
            fprintf(stderr, "open/close round %d failed: %s\n",
                    index, strerror(errno));
            return 1;
        }
    }
    printf("PASS: %d repeated open/close operations\n", OPEN_CLOSE_ROUNDS);

    for (index = 0; index < READER_COUNT; index++) {
        readers[index] = fork();
        if (readers[index] == -1) {
            fprintf(stderr, "fork reader %d failed: %s\n",
                    index, strerror(errno));
            return 1;
        }
        if (readers[index] == 0)
            return reader_process(0x5a);
    }

    writer = open(DEVICE_PATH, O_WRONLY);
    if (writer == -1) {
        fprintf(stderr, "writer open failed: %s\n", strerror(errno));
        return 1;
    }
    alarm(15);
    while (sent < total) {
        size_t remaining = total - sent;
        size_t request = remaining < sizeof(payload) ? remaining : sizeof(payload);
        ssize_t count = write(writer, payload, request);

        if (count == -1 && errno == EINTR)
            continue;
        if (count <= 0) {
            fprintf(stderr, "writer failed after %zu bytes: %s\n",
                    sent, strerror(errno));
            close(writer);
            return 1;
        }
        sent += (size_t)count;
    }
    close(writer);

    for (index = 0; index < READER_COUNT; index++) {
        int status;
        if (waitpid(readers[index], &status, 0) == -1 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "reader %d failed\n", index);
            return 1;
        }
    }
    alarm(0);
    printf("PASS: %d readers consumed %zu verified bytes\n",
           READER_COUNT, total);
    return 0;
}
