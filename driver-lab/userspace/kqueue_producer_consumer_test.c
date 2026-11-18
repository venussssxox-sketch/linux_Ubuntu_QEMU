#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define SINGLE_BYTES 1024
#define MULTI_BYTES_PER_WRITER 6000
#define WRITE_CHUNK 127
#define READ_CHUNK 113

static int write_all(int fd, const unsigned char *data, size_t count)
{
    size_t offset = 0;

    while (offset < count) {
        ssize_t transferred = write(fd, data + offset, count - offset);

        if (transferred == -1) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "write failed: %s\n", strerror(errno));
            return 1;
        }

        if (transferred == 0) {
            fprintf(stderr, "write unexpectedly returned 0\n");
            return 1;
        }

        offset += (size_t)transferred;
    }

    return 0;
}

static int run_sequence_writer(void)
{
    unsigned char payload[SINGLE_BYTES];
    int fd;
    size_t index;

    for (index = 0; index < sizeof(payload); index++)
        payload[index] = (unsigned char)(index % 251);

    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "sequence writer open failed: %s\n", strerror(errno));
        return 1;
    }

    if (write_all(fd, payload, sizeof(payload)) != 0) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int run_repeated_writer(unsigned char value)
{
    unsigned char payload[WRITE_CHUNK];
    int fd;
    size_t remaining = MULTI_BYTES_PER_WRITER;

    memset(payload, value, sizeof(payload));

    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "writer %c open failed: %s\n", value, strerror(errno));
        return 1;
    }

    while (remaining > 0) {
        size_t request = remaining < sizeof(payload) ? remaining : sizeof(payload);

        if (write_all(fd, payload, request) != 0) {
            close(fd);
            return 1;
        }

        remaining -= request;
    }

    close(fd);
    return 0;
}

static int wait_for_success(pid_t pid, const char *name)
{
    int status;

    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid for %s failed: %s\n", name, strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s did not exit successfully\n", name);
        return 1;
    }

    return 0;
}

int main(void)
{
    unsigned char buffer[READ_CHUNK];
    int fd;
    pid_t writer_a;
    pid_t writer_b;
    size_t received;
    size_t index;
    size_t count_a = 0;
    size_t count_b = 0;

    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "consumer open failed: %s\n", strerror(errno));
        return 1;
    }

    writer_a = fork();
    if (writer_a == -1) {
        fprintf(stderr, "fork sequence writer failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (writer_a == 0)
        return run_sequence_writer();

    received = 0;
    while (received < SINGLE_BYTES) {
        ssize_t transferred = read(fd, buffer, sizeof(buffer));

        if (transferred == -1) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "sequence reader failed: %s\n", strerror(errno));
            wait_for_success(writer_a, "sequence writer");
            close(fd);
            return 1;
        }

        for (index = 0; index < (size_t)transferred; index++) {
            unsigned char expected = (unsigned char)((received + index) % 251);

            if (buffer[index] != expected) {
                fprintf(stderr,
                        "sequence mismatch at byte %zu: expected=%u actual=%u\n",
                        received + index, expected, buffer[index]);
                wait_for_success(writer_a, "sequence writer");
                close(fd);
                return 1;
            }
        }

        received += (size_t)transferred;
    }

    if (wait_for_success(writer_a, "sequence writer") != 0) {
        close(fd);
        return 1;
    }

    printf("PASS: one producer and one consumer preserved %d-byte order\n",
           SINGLE_BYTES);

    writer_a = fork();
    if (writer_a == -1) {
        fprintf(stderr, "fork A writer failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (writer_a == 0)
        return run_repeated_writer('A');

    writer_b = fork();
    if (writer_b == -1) {
        fprintf(stderr, "fork B writer failed: %s\n", strerror(errno));
        wait_for_success(writer_a, "A writer");
        close(fd);
        return 1;
    }

    if (writer_b == 0)
        return run_repeated_writer('B');

    received = 0;
    while (received < 2 * MULTI_BYTES_PER_WRITER) {
        size_t remaining = 2 * MULTI_BYTES_PER_WRITER - received;
        size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t transferred = read(fd, buffer, request);

        if (transferred == -1) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "multi-reader failed: %s\n", strerror(errno));
            wait_for_success(writer_a, "A writer");
            wait_for_success(writer_b, "B writer");
            close(fd);
            return 1;
        }

        for (index = 0; index < (size_t)transferred; index++) {
            if (buffer[index] == 'A')
                count_a++;
            else if (buffer[index] == 'B')
                count_b++;
            else {
                fprintf(stderr, "unexpected byte in multi-writer test: %u\n",
                        buffer[index]);
                wait_for_success(writer_a, "A writer");
                wait_for_success(writer_b, "B writer");
                close(fd);
                return 1;
            }
        }

        received += (size_t)transferred;
    }

    if (wait_for_success(writer_a, "A writer") != 0 ||
        wait_for_success(writer_b, "B writer") != 0) {
        close(fd);
        return 1;
    }

    close(fd);

    if (count_a != MULTI_BYTES_PER_WRITER ||
        count_b != MULTI_BYTES_PER_WRITER) {
        fprintf(stderr, "byte totals: A=%zu B=%zu\n", count_a, count_b);
        return 1;
    }

    printf("PASS: two producers delivered %d A bytes and %d B bytes\n",
           MULTI_BYTES_PER_WRITER, MULTI_BYTES_PER_WRITER);
    return 0;
}
