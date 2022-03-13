#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FIB_DEV "/dev/fibonacci"

int main()
{
    // cppcheck-suppress variableScope
    long long sz;
    struct timespec ts1, ts2;

    char buf[256];
    char write_buf[] = "testing writing";
    int offset = 500; /* TODO: try test something bigger than the limit */

    int fd = open(FIB_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open character device");
        exit(1);
    }

    for (int i = 0; i <= offset; i++) {
        lseek(fd, i, SEEK_SET);
        clock_gettime(CLOCK_REALTIME, &ts1);
        sz = read(fd, buf, sizeof(buf));
        buf[sz] = 0;
        clock_gettime(CLOCK_REALTIME, &ts2);
        printf("Reading from " FIB_DEV
               " at offset %d, returned the sequence "
               "%s. ",
               i, buf);
        sz = write(fd, write_buf, strlen(write_buf));
        printf("cost time in kernel: %lld ns,", sz);
        printf("cost time in userspace: %ld ns.\n", ts2.tv_nsec - ts1.tv_nsec);
    }

    close(fd);
    return 0;
}
