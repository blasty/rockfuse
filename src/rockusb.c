#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libusb.h>

#include "rockusb.h"
#include "util.h"

// hardcoded for RK3399
#define ROCKCHIP_VID 0x2207
#define ROCKCHIP_PID 0x330c

#define PIPE_BULK_IN 0x81
#define PIPE_BULK_OUT 0x02

#define DIRECTION_OUT  0x00
#define DIRECTION_IN   0x80

static struct libusb_device_handle *devh = NULL;

int rockusb_init() {
    int rc;

    rc = libusb_init(NULL);
    if (rc < 0) {
        fprintf(stderr,
            "%s: Error initializing libusb: %s\n",
            __FUNCTION__, libusb_error_name(rc)
        );
        return -1;
    }

    devh = libusb_open_device_with_vid_pid(NULL, ROCKCHIP_VID, ROCKCHIP_PID);
    if (!devh) {
        fprintf(stderr, ":%s: Error finding USB device\n", __FUNCTION__);
        goto cleanup;
    }

    rc = libusb_claim_interface(devh, 0);
    if (rc < 0) {
        fprintf(stderr,
            ":%s: Error claiming interface: %s\n",
            __FUNCTION__, libusb_error_name(rc)
        );
        goto cleanup;
    }

    return 0;

cleanup:
    if (devh) {
        libusb_close(devh);
    }

    libusb_exit(NULL);

    return -1;
}

int rockusb_read(uint8_t *buf, size_t size) {
    int rc;
    int read;

    rc = libusb_bulk_transfer(devh, PIPE_BULK_IN, buf, size, &read, 0);
    if (rc < 0) {
        fprintf(stderr,
            "%s: error issuing bulk transfer: %s\n",
            __FUNCTION__, libusb_error_name(rc)
        );
        return -1;
    }
    if (read != size) {
        fprintf(stderr,
            "%s: read != size (0x%x vs 0x%lx)\n", __FUNCTION__, read, size
        );
        return -1;
    }
    return 0;
}

int rockusb_write(uint8_t *buf, size_t size) {
    int rc;
    int wrote;

    rc = libusb_bulk_transfer(devh, PIPE_BULK_OUT, buf, size, &wrote, 0);
    if (rc < 0) {
        fprintf(stderr,
            "%s: error issuing bulk transfer: %s\n",
            __FUNCTION__, libusb_error_name(rc)
        );
        return -1;
    }
    if (wrote != size) {
        fprintf(stderr,
            "%s: wrote != size (0x%x vs 0x%lx)\n", __FUNCTION__, wrote, size
        );
        return -1;
    }
    return 0;
}


void cbw_build(usbms_cbw_t *cbw, uint8_t opcode) {
    memset(cbw, 0, sizeof(usbms_cbw_t));
    cbw->sig = USBMS_CBW_SIG;
    cbw->tag = 0;
    for(int i = 0; i < 4; i++) {
        cbw->tag <<= 8;
        cbw->tag |= (rand() & 0xff);
    }
    cbw->command.opcode = opcode;
}

int csw_check(usbms_cbw_t *cbw, usbms_csw_t *csw) {
    if (csw->sig != USBMS_CSW_SIG) {
        return -1;
    }

    if (cbw->tag != csw->tag) {
        return -2;
    }

    return 0;
}


int rockusb_read_flash_id(uint8_t *flash_id_out) {
    usbms_cbw_t cbw;
    usbms_csw_t csw;

    cbw_build(&cbw, CBW_READ_FLASH_ID);
    cbw.flags = DIRECTION_IN;
    cbw.length = 6;
    cbw.transfer_length = 5;

    if (rockusb_write((uint8_t*)&cbw, sizeof(usbms_cbw_t)) != 0) {
        printf("%s: write cbw failed\n", __FUNCTION__);
        return -1;
    }

    if(rockusb_read(flash_id_out, 5) != 0) {
        printf("%s: read response failed\n", __FUNCTION__);
        return -2;
    }

    if (rockusb_read((uint8_t*)&csw, sizeof(usbms_csw_t)) != 0) {
        printf("%s: read status failed\n", __FUNCTION__);
        return -3;
    }

    if (csw_check(&cbw, &csw) != 0) {
        printf("%s: csw check failed\n", __FUNCTION__);
        return -4;
    }

    return 0;
}

int rockusb_read_flash_info(flash_info_t *flash_info) {
    usbms_cbw_t cbw;
    usbms_csw_t csw;

    cbw_build(&cbw, CBW_READ_FLASH_INFO);
    cbw.flags = DIRECTION_IN;
    cbw.length = 6;
    cbw.transfer_length = 11;

    if (rockusb_write((uint8_t*)&cbw, sizeof(usbms_cbw_t)) != 0) {
        printf("%s: write cbw failed\n", __FUNCTION__);
        return -1;
    }

    if(rockusb_read((uint8_t*)flash_info, 11) != 0) {
        printf("%s: read response failed\n", __FUNCTION__);
        return -2;
    }

    if (rockusb_read((uint8_t*)&csw, sizeof(usbms_csw_t)) != 0) {
        printf("%s: read status failed\n", __FUNCTION__);
        return -3;
    }

    if (csw_check(&cbw, &csw) != 0) {
        printf("%s: csw check failed\n", __FUNCTION__);
        return -4;
    }

    return 0;
}

int rockusb_read_lba(uint32_t offset, uint32_t count, uint8_t *buf) {
    usbms_cbw_t cbw;
    usbms_csw_t csw;

    printf("%s: offset=%08x, count=%08x, buf=%p\n", __FUNCTION__, offset, count, buf);

    cbw_build(&cbw, CBW_READ_LBA);
    cbw.flags = DIRECTION_IN;
    cbw.length = 0x0a;
    cbw.transfer_length = count * 0x200;
    cbw.command.address = __builtin_bswap32(offset);
    cbw.command.length = __builtin_bswap16(count);

    if(rockusb_write((uint8_t*)&cbw, sizeof(usbms_cbw_t)) != 0) {
        printf("%s: write cbw failed\n", __FUNCTION__);
        return -1;
    }

    if(rockusb_read(buf, count * 0x200) != 0) {
        printf("%s: read response failed\n", __FUNCTION__);
        return -2;
    }

    if (rockusb_read((uint8_t*)&csw, sizeof(usbms_csw_t)) != 0) {
        printf("%s: read status failed\n", __FUNCTION__);
        return -3;
    }

    if (csw_check(&cbw, &csw) != 0) {
        printf("%s: csw check failed\n", __FUNCTION__);
        return -4;
    }

    if (csw.status == 1) {
        printf("%s: read failed\n", __FUNCTION__);
        return -5;
    }

    return 0;
}

int rockusb_write_lba(uint32_t offset, uint32_t count, uint8_t *buf) {
    usbms_cbw_t cbw;
    usbms_csw_t csw;

    cbw_build(&cbw, CBW_WRITE_LBA);
    cbw.flags = DIRECTION_OUT;
    cbw.length = 0x0a;
    cbw.transfer_length = count * 0x200;
    cbw.command.address = __builtin_bswap32(offset);
    cbw.command.length = __builtin_bswap16(count);

    if(rockusb_write((uint8_t*)&cbw, sizeof(usbms_cbw_t)) != 0) {
        printf("%s: write cbw failed\n", __FUNCTION__);
        return -1;
    }

    if(rockusb_write(buf, count * 0x200) != 0) {
        printf("%s: write data failed\n", __FUNCTION__);
        return -2;
    }

    if (rockusb_read((uint8_t*)&csw, sizeof(usbms_csw_t)) != 0) {
        printf("%s: read status failed\n", __FUNCTION__);
        return -3;
    }

    if (csw_check(&cbw, &csw) != 0) {
        printf("%s: csw check failed\n", __FUNCTION__);
        return -4;
    }

    if (csw.status == 1) {
        printf("%s: read failed\n", __FUNCTION__);
        return -5;
    }

    return 0;
}

