#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"

int main(void)
{
    int fd;

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    printf("HOLDER_READY\n");
    fflush(stdout);
    sleep(5);

    if (close(fd) == -1) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return 1;
    }

    printf("HOLDER_CLOSED\n");
    return 0;
}
