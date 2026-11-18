#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../include/kqueue_lab_ioctl.h"

static int check_bad_user_address(int fd)
{
    long page_size;
    void *bad_address;
    int ret;
    int saved_errno;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size == -1) {
        fprintf(stderr,
                "sysconf failed: %s\n",
                strerror(errno));
        return -1;
    }

    bad_address = mmap(NULL,
                       (size_t)page_size,
                       PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);
    if (bad_address == MAP_FAILED) {
        fprintf(stderr,
                "mmap failed: %s\n",
                strerror(errno));
        return -1;
    }

    errno = 0;
    ret = ioctl(fd, KQUEUE_IOC_GET_INFO, bad_address);
    saved_errno = errno;

    if (munmap(bad_address, (size_t)page_size) == -1) {
        fprintf(stderr,
                "munmap failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (ret != -1) {
        fprintf(stderr,
                "bad address: expected ret=-1, actual=%d\n",
                ret);
        return -1;
    }

    if (saved_errno != EFAULT) {
        fprintf(stderr,
                "bad address: expected errno=%d, actual=%d (%s)\n",
                EFAULT,
                saved_errno,
                strerror(saved_errno));
        return -1;
    }

    puts("bad address: EFAULT PASS");
    return 0;
}

static int check_unknown_command(int fd)
{
    unsigned long unknown_cmd;
    int ret;

    unknown_cmd = _IO(KQUEUE_IOC_MAGIC, 0x7f);

    errno = 0;
    ret = ioctl(fd, unknown_cmd, 0);

    if (ret != -1) {
        fprintf(stderr,
                "unknown command: expected ret=-1, actual=%d\n",
                ret);
        return -1;
    }

    if (errno != ENOTTY) {
        fprintf(stderr,
                "unknown command: expected errno=%d, actual=%d (%s)\n",
                ENOTTY,
                errno,
                strerror(errno));
        return -1;
    }

    puts("unknown command: ENOTTY PASS");
    return 0;
}

static int check_info(int fd,
                      unsigned int expected_used,
                      const char *stage)
{
    struct kqueue_info info;

    if (ioctl(fd, KQUEUE_IOC_GET_INFO, &info) == -1) {
        fprintf(stderr,
                "%s: ioctl failed: %s\n",
                stage,
                strerror(errno));
        return -1;
    }

    printf("%s: capacity=%u used=%u available=%u\n",
           stage,
           info.capacity,
           info.used,
           info.available);

    if (info.capacity != 4096) {
        fprintf(stderr, "%s: unexpected capacity\n", stage);
        return -1;
    }

    if (info.used != expected_used) {
        fprintf(stderr,
                "%s: expected used=%u, actual=%u\n",
                stage,
                expected_used,
                info.used);
        return -1;
    }

    if (info.used + info.available != info.capacity) {
        fprintf(stderr, "%s: inconsistent FIFO state\n", stage);
        return -1;
    }

    return 0;
}

int main(void)
{
    int fd;
    char buffer[2];
    ssize_t ret;

    fd = open("/dev/kqueue_lab", O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        return 1;
    }

    if (check_unknown_command(fd) != 0)
        goto fail;

    if (check_bad_user_address(fd) != 0)
        goto fail;

    if (check_info(fd, 0, "empty") != 0)
        goto fail;

    ret = write(fd, "abc", 3);
    if (ret != 3) {
        fprintf(stderr,
                "write failed: ret=%zd errno=%s\n",
                ret,
                strerror(errno));
        goto fail;
    }

    if (check_info(fd, 3, "after write") != 0)
        goto fail;

    if (check_info(fd, 3, "second query") != 0)
        goto fail;

    ret = read(fd, buffer, sizeof(buffer));
    if (ret != 2) {
        fprintf(stderr,
                "read failed: ret=%zd errno=%s\n",
                ret,
                strerror(errno));
        goto fail;
    }

    if (check_info(fd, 1, "after read") != 0)
        goto fail;

    close(fd);
    puts("kqueue ioctl test: PASS");
    return 0;

fail:
    close(fd);
    return 1;
}
