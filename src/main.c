#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <libusb.h>

#include "rockusb.h"
#include "rockfuse.h"

int main(int argc, char *argv[]) {
    int rc = 0;

    if (argc != 2) {
        printf("usage: %s <mountpath>\n", argv[0]);
        return -1;
    }

    printf("rockfuse init\n");
    if (rockfuse_init() != 0) {
        exit(-1);
    }

    printf("rockfuse main\n");
    rc = rockfuse_main(argc, argv);

    return rc;
}
