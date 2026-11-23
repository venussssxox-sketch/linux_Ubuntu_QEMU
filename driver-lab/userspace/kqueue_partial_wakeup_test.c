#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define FIFO_SIZE 4096

static int wait_child(pid_t pid, const char *name)
{
    int status;

    alarm(5);
    if (waitpid(pid, &status, 0) == -1) {
        fprintf(stderr, "waitpid(%s) failed: %s\n", name, strerror(errno));
        alarm(0);
        return 1;
    }
    alarm(0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "%s failed or timed out\n", name);
        return 1;
    }
    return 0;
}

static int wait_until_blocked(pid_t pid, const char *name)
{
    int status;
    int attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == -1) {
            fprintf(stderr, "waitpid(%s, WNOHANG) failed: %s\n",
                    name, strerror(errno));
            return 1;
        }
        if (result == pid) {
            fprintf(stderr, "%s exited before the wakeup condition\n", name);
            return 1;
        }
        usleep(10000);
    }
    return 0;
}

int main(void)
{
    char fill[FIFO_SIZE];
    char drain[FIFO_SIZE];
    char *mapping = MAP_FAILED;
    long page_size;
    int fd = -1;
    int ready_pipe[2] = {-1, -1};
    pid_t child;
    ssize_t transferred;
    int result = 1;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "could not determine page size\n");
        return 1;
    }
    mapping = mmap(NULL, (size_t)page_size * 2,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        return 1;
    }
    if (mprotect(mapping + page_size, (size_t)page_size, PROT_NONE) == -1) {
        fprintf(stderr, "mprotect failed: %s\n", strerror(errno));
        goto out;
    }

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "open failed: %s\n", strerror(errno));
        goto out;
    }

    /* A reader blocks on an empty FIFO. A one-byte partial write must wake it. */
    if (pipe(ready_pipe) == -1) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        goto out;
    }
    child = fork();
    if (child == -1) {
        fprintf(stderr, "fork reader failed: %s\n", strerror(errno));
        goto out;
    }
    if (child == 0) {
        char byte;
        close(ready_pipe[0]);
        if (write(ready_pipe[1], "R", 1) != 1)
            _exit(2);
        close(ready_pipe[1]);
        if (read(fd, &byte, 1) != 1 || byte != 'W')
            _exit(3);
        _exit(0);
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read(ready_pipe[0], drain, 1) != 1 ||
        wait_until_blocked(child, "reader") != 0)
        goto out;
    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    mapping[page_size - 1] = 'W';
    transferred = write(fd, mapping + page_size - 1, 2);
    if (transferred != 1 || wait_child(child, "reader") != 0) {
        fprintf(stderr, "partial write did not wake reader: result=%zd\n",
                transferred);
        goto out;
    }
    puts("PASS: partial write woke a blocked reader");

    /* A writer blocks on a full FIFO. A one-byte partial read must wake it. */
    memset(fill, 'P', sizeof(fill));
    if (write(fd, fill, sizeof(fill)) != (ssize_t)sizeof(fill)) {
        fprintf(stderr, "could not fill FIFO\n");
        goto out;
    }
    if (pipe(ready_pipe) == -1) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        goto out;
    }
    child = fork();
    if (child == -1) {
        fprintf(stderr, "fork writer failed: %s\n", strerror(errno));
        goto out;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        if (write(ready_pipe[1], "W", 1) != 1)
            _exit(2);
        close(ready_pipe[1]);
        if (write(fd, "Z", 1) != 1)
            _exit(3);
        _exit(0);
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read(ready_pipe[0], drain, 1) != 1 ||
        wait_until_blocked(child, "writer") != 0)
        goto out;
    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    mapping[page_size - 1] = 0;
    transferred = read(fd, mapping + page_size - 1, 2);
    if (transferred != 1 || mapping[page_size - 1] != 'P' ||
        wait_child(child, "writer") != 0) {
        fprintf(stderr, "partial read did not wake writer: result=%zd\n",
                transferred);
        goto out;
    }
    if (read(fd, drain, sizeof(drain)) != (ssize_t)sizeof(drain) ||
        drain[FIFO_SIZE - 1] != 'Z') {
        fprintf(stderr, "FIFO content invalid after writer wakeup\n");
        goto out;
    }
    puts("PASS: partial read woke a blocked writer");
    result = 0;

out:
    if (ready_pipe[0] != -1)
        close(ready_pipe[0]);
    if (ready_pipe[1] != -1)
        close(ready_pipe[1]);
    if (fd != -1)
        close(fd);
    if (mapping != MAP_FAILED)
        munmap(mapping, (size_t)page_size * 2);
    return result;
}
