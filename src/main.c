//
// rockfuse.c -- FUSE filesystem driver for mounting RockChip eMMC over USB.
// =========================================================================
//
// This uses the `rockusb` protocol which is part of the RockChip recovery ROM
// as well as part of rockchip U-boot. To use this from u-boot issue this cmd:
//   rockusb 0 mmc 0
//
// build:
//   gcc -o rockfuse rockfuse.c `pkg-config libusb-1.0 fuse --libs --cflags`
//
// usage:
//   ./rockfuse /mnt/path
//
//
// -- blasty <peter@haxx.in> // 20191107
//

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libusb.h>

// hardcoded for RK3399
#define ROCKCHIP_VID 0x2207
#define ROCKCHIP_PID 0x330c

#define PIPE_BULK_IN 0x81
#define PIPE_BULK_OUT 0x02

#define DIRECTION_OUT  0x00
#define DIRECTION_IN   0x80

// http://opensource.rock-chips.com/wiki_File:Rockchip_bootflow20181122.jpg
#define LOADER1_START_SECTOR 0x40
#define LOADER2_START_SECTOR 0x4000
#define TRUST_START_SECTOR   0x6000
#define BOOT_START_SECTOR    0x8000
#define ROOT_START_SECTOR    0x40000

#define LOADER1_SIZE (LOADER2_START_SECTOR - LOADER1_START_SECTOR)
#define LOADER2_SIZE (TRUST_START_SECTOR - LOADER2_START_SECTOR)
#define TRUST_SIZE (BOOT_START_SECTOR - TRUST_START_SECTOR)
#define BOOT_SIZE (ROOT_START_SECTOR - BOOT_START_SECTOR)

// "USBC"
#define USBMS_CBW_SIG 0x43425355
// "USBS"
#define USBMS_CSW_SIG 0x53425355

#define CBW_READ_FLASH_ID    0x01
#define CBW_READ_LBA         0x14
#define CBW_WRITE_LBA        0x15
#define CBW_READ_FLASH_INFO  0x1a

#pragma pack(1)

int rockusb_read_lba(uint32_t offset, uint32_t count, uint8_t *buf);

typedef struct {
    uint8_t opcode;
    uint8_t reserved;
    uint32_t address;
    uint8_t reserved2;
    uint16_t length;
    uint8_t reserved3[7];
} usbms_cbwcb_t;

typedef struct {
    uint32_t sig;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t length;
    usbms_cbwcb_t command;
} usbms_cbw_t;

typedef struct {
    uint32_t sig;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t status;
} usbms_csw_t;

typedef struct {
    uint32_t flash_size;
    uint16_t block_size;
    uint32_t page_size;
    uint8_t ecc_bits;
    uint8_t access_time;
    uint8_t mfg_code;
    uint8_t flash_cs;
    uint8_t reserved[501];
} flash_info_t;

typedef struct {
    char *vpath;
    uint64_t sector_start;
    uint64_t sector_count;
} vfile_entry_t;

#define FILE_ID_FULL 0
#define FILE_ID_ROOT 5

vfile_entry_t vfile_tab[]={
    { "/full.img", 0, 0 },
    { "/loader1.img", LOADER1_START_SECTOR, LOADER1_SIZE },
    { "/loader2.img", LOADER2_START_SECTOR, LOADER2_SIZE },
    { "/trust.img",   TRUST_START_SECTOR,   TRUST_SIZE },
    { "/boot.img",    BOOT_START_SECTOR,    BOOT_SIZE },
    { "/root.img",    ROOT_START_SECTOR,    0 },
    { NULL, 0, 0 }
};

vfile_entry_t *get_vfile_entry_by_path(const char *path) {
    for(int i = 0; vfile_tab[i].vpath != NULL; i++) {
        if (strcmp(vfile_tab[i].vpath, path) == 0) {
            return &vfile_tab[i];
        }
    }

    return NULL;
}

static struct libusb_device_handle *devh = NULL;

static int rockfuse_getattr(const char *path, struct stat *stbuf) {
    int res = 0;

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return res;
    }

    vfile_entry_t* vfile = get_vfile_entry_by_path(path);

    if (vfile == NULL) {
        return -ENOENT;
    }

    stbuf->st_mode = S_IFREG | 0666;
    stbuf->st_nlink = 1;
    stbuf->st_size = vfile->sector_count * 0x200;

    return res;
}

static int rockfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
             off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for(int i = 0; vfile_tab[i].vpath != NULL; i++) {
        filler(buf, vfile_tab[i].vpath + 1, NULL, 0);
    }

    return 0;
}

static int rockfuse_open(const char *path, struct fuse_file_info *fi) {
    vfile_entry_t* vfile = get_vfile_entry_by_path(path);

    if (vfile == NULL) {
        return -ENOENT;
    }

    return 0;
}

uint8_t workbuf[0x200];

static int rockfuse_read(const char *path, char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi) {
    size_t maxlen;
    (void) fi;
    vfile_entry_t* vfile = get_vfile_entry_by_path(path);

    if (vfile == NULL) {
        return -ENOENT;
    }

    maxlen = vfile->sector_count * 0x200;

    if (offset < maxlen) {
        if (offset + size > maxlen)
            size = maxlen - offset;

        size_t size_left = size;
        uint32_t buf_pos = 0;

        // check for initial unaligned offset
        if (offset % 0x200 != 0) {
            if (rockusb_read_lba(
                vfile->sector_start + (offset >> 9), 1, workbuf
            ) != 0) {
                return 0;
            }
            uint32_t fragsize = 0x200 - (offset % 0x200);
            if (fragsize > size) {
                fragsize = size;
            }

            memcpy(buf + buf_pos, workbuf + (offset % 0x200), fragsize);

            buf_pos += fragsize;
            size_left -= fragsize;
        }

        // read main body (sector-multiples)
        if (size_left >= 0x200) {
            if (rockusb_read_lba(
                vfile->sector_start + ((offset + buf_pos) >> 9),
                size_left >> 9, (uint8_t*)buf + buf_pos
            ) != 0) {
                return 0;
            }

            uint32_t mainsize = (size_left >> 9) << 9;

            buf_pos += mainsize;
            size_left -= mainsize;
        }

        // read trailing unaligned size left bytes
        if (size_left > 0) {
            if (rockusb_read_lba(
                    vfile->sector_start + ((offset + buf_pos) >> 9),
                    1, workbuf
            ) != 0) {
                return 0;
            }

            memcpy(buf + buf_pos, workbuf, size_left);
        }
    } else
        size = 0;

    return size;
}

static struct fuse_operations rockfuse_oper = {
    .getattr    = rockfuse_getattr,
    .readdir    = rockfuse_readdir,
    .open        = rockfuse_open,
    .read        = rockfuse_read,
};

int rockusb_init() {
    int rc;

    rc = libusb_init(NULL);
    if (rc < 0) {
        fprintf(stderr,
            "Error initializing libusb: %s\n", libusb_error_name(rc)
        );
        return -1;
    }

    devh = libusb_open_device_with_vid_pid(NULL, ROCKCHIP_VID, ROCKCHIP_PID);
    if (!devh) {
        fprintf(stderr, "Error finding USB device\n");
        goto cleanup;
    }

    rc = libusb_claim_interface(devh, 0);
    if (rc < 0) {
        fprintf(stderr,
            "Error claiming interface: %s\n", libusb_error_name(rc)
        );
        goto cleanup;
    }

    printf("rockusb_init OK\n");

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

int main(int argc, char *argv[]) {
    int rc = 0;

    if (argc != 2) {
        printf("usage: %s <mountpath>\n", argv[0]);
        return -1;
    }

    if (rockusb_init() != 0) {
        printf("rockusb_init failed\n");
        return -1;
    }

    uint8_t id[5];

    if (rockusb_read_flash_id(id) != 0) {
        printf("rockusb_read_flash_id failed\n");
        return -1;
    }

    printf(
        "flash id: %02x %02x %02x %02x %02x ('%c%c%c%c%c')\n",
        id[0], id[1], id[2], id[3], id[4],
        id[0], id[1], id[2], id[3], id[4]
    );

    flash_info_t flash_info;

    memset(&flash_info, 0xaa, sizeof(flash_info_t));

    printf("rockusb_read_flash_info()\n");
    if (rockusb_read_flash_info(&flash_info) != 0) {
        printf("rockusb_read_flash_info failed\n");
        return -1;
    }

    vfile_tab[FILE_ID_FULL].sector_count = flash_info.flash_size;
    vfile_tab[FILE_ID_ROOT].sector_count =
                    flash_info.flash_size - ROOT_START_SECTOR;

#ifdef VERBOSE
    printf("flash size: %08x\n", flash_info.flash_size);
    printf("page size : %08x\n", flash_info.page_size);
    printf("block size: %04x\n", flash_info.block_size);
    printf("mfg code  : %02x\n", flash_info.mfg_code);
#endif

    printf("starting FUSE..\n");

    rc = fuse_main(argc, argv, &rockfuse_oper, NULL);

    return rc;
}
