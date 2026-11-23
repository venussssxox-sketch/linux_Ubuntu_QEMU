#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../include/kqueue_lab_ioctl.h"

/* Position in/out at the last FIFO byte: the two user copies then straddle
 * both the FIFO wrap and a PROT_NONE page, guaranteeing one copied byte. */
int main(void)
{
    char fill[4095], out[4095], pair[2] = {'A', 'B'};
    struct kqueue_info info;
    long page = sysconf(_SC_PAGESIZE);
    int fd = -1, result = 1;
    char *mapping;
    ssize_t n;
    if (page <= 0) return 1;
    mapping = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) { perror("mmap"); return 1; }
    if (mprotect(mapping + page, (size_t)page, PROT_NONE)) {
        perror("mprotect"); goto done;
    }
    fd = open("/dev/kqueue_lab", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); goto done; }
    memset(fill, 'P', sizeof(fill));
    if (write(fd, fill, sizeof(fill)) != sizeof(fill) ||
        read(fd, out, sizeof(out)) != sizeof(out)) goto done;
    mapping[page - 1] = 'W';
    errno = 0;
    n = write(fd, mapping + page - 1, 2);
    if (ioctl(fd, KQUEUE_IOC_GET_INFO, &info)) goto done;
    printf("partial fault write: returned=%zd used=%u expected=1/1\n", n, info.used);
    if (n != 1 || info.used != 1 || read(fd, out, 1) != 1 || out[0] != 'W')
        goto done;
    /* in/out are now 4096; move back to index 4095 for the read case. */
    if (write(fd, fill, sizeof(fill)) != sizeof(fill) ||
        read(fd, out, sizeof(out)) != sizeof(out) || write(fd, pair, 2) != 2)
        goto done;
    mapping[page - 1] = 0;
    errno = 0;
    n = read(fd, mapping + page - 1, 2);
    if (ioctl(fd, KQUEUE_IOC_GET_INFO, &info)) goto done;
    printf("partial fault read: returned=%zd used=%u expected=1/1\n", n, info.used);
    if (n != 1 || info.used != 1 || mapping[page - 1] != 'A' ||
        read(fd, out, 1) != 1 || out[0] != 'B') goto done;
    puts("PASS: partial user faults report transferred bytes and preserve FIFO state");
    result = 0;
done:
    if (result) fprintf(stderr, "FAIL: partial fault regression (errno=%d)\n", errno);
    if (fd >= 0 && close(fd)) result = 1;
    if (munmap(mapping, (size_t)page * 2)) result = 1;
    return result;
}
