#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/kqueue_lab"
#define KQUEUE_FIFO_CAPACITY 4096
#define PARTIAL_TEST_SIZE (KQUEUE_FIFO_CAPACITY + 1)

int main(void)
{
		const char message[] = "abc";
		char buffer[sizeof(message) - 1];
		char partial_message[PARTIAL_TEST_SIZE];
		char partial_buffer[PARTIAL_TEST_SIZE];
		int fd;
		ssize_t written;
		ssize_t received;
		ssize_t zero_received;
		ssize_t empty_received;
		ssize_t partial_written;
		ssize_t partial_received;
		ssize_t zero_written;

		fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
		if (fd == -1) {
				fprintf(stderr, "open failed: %s\n", strerror(errno));
				return 1;
		}

		errno = 0;
		zero_written = write(fd, message, 0);
		if (zero_written != 0) {
				fprintf(stderr,
								"zero-length write: expected=0 actual=%zd errno=%s\n",
								zero_written,
								strerror(errno));
				close(fd);
				return 1;
		}

		written = write(fd, message, sizeof(message) - 1);
		if (written == -1) {
				fprintf(stderr, "write failed: %s\n", strerror(errno));
				close(fd);
				return 1;
		}

		if ((size_t)written != sizeof(message) - 1) {
				fprintf(stderr,
								"partial write: expected=%zu actual=%zd\n",
								sizeof(message) - 1,
								written);
				close(fd);
				return 1;
		}

		errno = 0;
		zero_received = read(fd, buffer, 0);
		if (zero_received != 0) {
				fprintf(stderr,
								"zero-length read: expected=0 actual=%zd errno=%s\n",
								zero_received,
								strerror(errno));
				close(fd);
				return 1;
		}

		received = read(fd, buffer, sizeof(buffer));
		if (received == -1) {
				fprintf(stderr, "read failed: %s\n", strerror(errno));
				close(fd);
				return 1;
		}

		if (received != written) {
				fprintf(stderr,
								"length mismatch: written=%zd received=%zd\n",
								written,
								received);
				close(fd);
				return 1;
		}

		if (memcmp(message, buffer, (size_t)received) != 0) {
				fprintf(stderr, "data mismatch\n");
				close(fd);
				return 1;
		}

		errno = 0;
		empty_received = read(fd, buffer, sizeof(buffer));
		if (empty_received != -1) {
				fprintf(stderr,
								"empty queue read unexpectedly returned %zd\n",
								empty_received);
				close(fd);
				return 1;
		}

		if (errno != EAGAIN) {
				fprintf(stderr,
								"empty queue read: expected EAGAIN, actual=%s\n",
								strerror(errno));
				close(fd);
				return 1;
		}

		memset(partial_message, 'P', sizeof(partial_message));

		partial_written = write(fd, partial_message, sizeof(partial_message));
		if (partial_written != KQUEUE_FIFO_CAPACITY) {
				fprintf(stderr,
								"partial write: expected=%d actual=%zd errno=%s\n",
								KQUEUE_FIFO_CAPACITY,
								partial_written,
								strerror(errno));
				close(fd);
				return 1;
		}

		partial_received = read(fd, partial_buffer, sizeof(partial_buffer));
		if (partial_received != KQUEUE_FIFO_CAPACITY) {
				fprintf(stderr,
								"partial read: expected=%d actual=%zd errno=%s\n",
								KQUEUE_FIFO_CAPACITY,
								partial_received,
								strerror(errno));
				close(fd);
				return 1;
		}

		if (memcmp(partial_message,
								partial_buffer,
								KQUEUE_FIFO_CAPACITY) != 0) {
				fprintf(stderr, "partial transfer data mismatch\n");
				close(fd);
				return 1;
		}

		errno = 0;
		empty_received = read(fd, partial_buffer, 1);
		if (empty_received != -1 || errno != EAGAIN) {
				fprintf(stderr,
								"FIFO should be empty after partial transfer: "
								"read=%zd errno=%s\n",
								empty_received,
								strerror(errno));
				close(fd);
				return 1;
		}


		if (close(fd) == -1) {
				fprintf(stderr, "close failed: %s\n", strerror(errno));
				return 1;
		}

		printf("PASS: zero-length write returned 0\n");

		printf("PASS: zero-length read returned 0\n");
		printf("PASS: wrote and read back %zd bytes: %.*s\n",
						received,
						(int)received,
						buffer);
		printf("PASS: empty queue returned EAGAIN\n");


		return 0;
}
