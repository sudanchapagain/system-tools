#include "event.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

int
send_event(int fd, enum event_type event)
{
    ssize_t bytes = write(fd, &(uint64_t){event}, sizeof(uint64_t));

    if (bytes < 0)
        return -errno;
    else if (bytes != (ssize_t)sizeof(uint64_t))
        return 1;
    return 0;
}

