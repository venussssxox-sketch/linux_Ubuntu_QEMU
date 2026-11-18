#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"

int main(void)
{
    const char value = 'X';
    char received;
    const void *invalid_source;
    void *invalid_destination;
    int fd;
    long page_size;
    ssize_t transferred;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "could not determine page size\n");
        return 1;
    }

    invalid_source = (const void *)(uintptr_t)page_size;
    invalid_destination = (void *)(uintptr_t)page_size;

    fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    errno = 0;
    transferred = write(fd, invalid_source, 1);
    if (transferred != -1 || errno != EFAULT) {
        fprintf(stderr, "invalid write: result=%zd errno=%s\n",
                transferred, strerror(errno));
        close(fd);
        return 1;
    }

    transferred = write(fd, &value, 1);
    if (transferred != 1) {
        fprintf(stderr, "valid write failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    errno = 0;
    transferred = read(fd, invalid_destination, 1);
    if (transferred != -1 || errno != EFAULT) {
        fprintf(stderr, "invalid read: result=%zd errno=%s\n",
                transferred, strerror(errno));
        close(fd);
        return 1;
    }

    transferred = read(fd, &received, 1);
    if (transferred != 1 || received != value) {
        fprintf(stderr, "valid read after EFAULT failed: result=%zd value=%u\n",
                transferred, (unsigned char)received);
        close(fd);
        return 1;
    }

    close(fd);
    printf("PASS: invalid user buffers returned EFAULT without consuming data\n");
    return 0;
}
