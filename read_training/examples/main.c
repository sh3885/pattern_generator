#include "read_training.h"

#include <stdio.h>

int main(void)
{
    const char *line = "  training input line  ";
    char buffer[64];
    RtStatus status;

    status = rt_trim_line(line, buffer, sizeof(buffer), NULL);
    if (status != RT_OK) {
        fprintf(stderr, "rt_trim_line failed: %s\n", rt_status_message(status));
        return 1;
    }

    printf("%s\n", buffer);
    return 0;
}

