#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "rockusb.h"

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

static pthread_mutex_t usb_mutex;

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

#define MAX_SECTORS 128

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

    pthread_mutex_lock(&usb_mutex);

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
        while (size_left >= 0x200) {
            uint32_t num_sectors =
                ((size_left >> 9) > MAX_SECTORS) ? MAX_SECTORS : (size_left >> 9);

            if (rockusb_read_lba(
                vfile->sector_start + ((offset + buf_pos) >> 9),
                num_sectors, (uint8_t*)buf + buf_pos
            ) != 0) {
                return 0;
            }

            buf_pos += (num_sectors << 9);
            size_left -= (num_sectors << 9);
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

    pthread_mutex_unlock(&usb_mutex);

    return size;
}

static int rockfuse_write(const char *path, const char *buf, size_t size, off_t offset,
              struct fuse_file_info *fi) {
    size_t maxlen;
    (void) fi;
    vfile_entry_t* vfile = get_vfile_entry_by_path(path);

    if (vfile == NULL) {
        return -ENOENT;
    }

    maxlen = vfile->sector_count * 0x200;

    pthread_mutex_lock(&usb_mutex);

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

            memcpy(workbuf + (offset % 0x200), buf, fragsize);

            if (rockusb_write_lba(
                vfile->sector_start + (offset >> 9), 1, workbuf
            ) != 0) {
                return 0;
            }

            buf_pos += fragsize;
            size_left -= fragsize;
        }

        // write aligned main body (sector-multiples)
        while (size_left >= 0x200) {
            uint32_t num_sectors =
                ((size_left >> 9) > MAX_SECTORS) ? MAX_SECTORS : (size_left >> 9);

            if (rockusb_write_lba(
                vfile->sector_start + ((offset + buf_pos) >> 9),
                num_sectors, (uint8_t*)buf + buf_pos
            ) != 0) {
                return 0;
            }

            buf_pos += num_sectors;
            size_left -= num_sectors;
        }

        // write trailing unaligned size left bytes
        if (size_left > 0) {
            if (rockusb_read_lba(
                vfile->sector_start + ((offset + buf_pos) >> 9), 1, workbuf
            ) != 0) {
                return 0;
            }

            memcpy(workbuf, buf + buf_pos, size_left);
            if (rockusb_write_lba(
                vfile->sector_start + ((offset + buf_pos) >> 9), 1, workbuf
            ) != 0) {
                return 0;
            }
        }
    } else
        size = 0;

    pthread_mutex_unlock(&usb_mutex);

    return size;
}

int rockfuse_init() {
    if (rockusb_init() != 0) {
        fprintf(stderr, "%s: rockusb_init failed\n", __FUNCTION__);
        return -1;
    }

    uint8_t id[5];

    if (rockusb_read_flash_id(id) != 0) {
        fprintf(stderr, "%s: rockusb_read_flash_id failed\n", __FUNCTION__);
        return -1;
    }

    printf(
        "flash id: %02x %02x %02x %02x %02x ('%c%c%c%c%c')\n",
        id[0], id[1], id[2], id[3], id[4],
        id[0], id[1], id[2], id[3], id[4]
    );

    flash_info_t flash_info;

    memset(&flash_info, 0xaa, sizeof(flash_info_t));

    if (rockusb_read_flash_info(&flash_info) != 0) {
        fprintf(stderr, "%s: rockusb_read_flash_info failed\n", __FUNCTION__);
        return -1;
    }

    vfile_tab[FILE_ID_FULL].sector_count = flash_info.flash_size;
    vfile_tab[FILE_ID_ROOT].sector_count =
                    flash_info.flash_size - ROOT_START_SECTOR;

#ifdef VERBOSE
    printf("flash info:\n");
    printf("  flash size: %08x\n", flash_info.flash_size);
    printf("  page size : %08x\n", flash_info.page_size);
    printf("  block size: %04x\n", flash_info.block_size);
    printf("  mfg code  : %02x\n", flash_info.mfg_code);
    printf("\n");
#endif

    // create mutex for parallel USB I/O operations
    if (pthread_mutex_init(&usb_mutex, NULL) != 0) {
        fprintf(stderr, "%s: error creating mutex\n", __FUNCTION__);
        return -1;
    }

    return 0;
}

static struct fuse_operations rockfuse_oper = {
    .getattr    = rockfuse_getattr,
    .readdir    = rockfuse_readdir,
    .open       = rockfuse_open,
    .read       = rockfuse_read,
    .write      = rockfuse_write
};

int rockfuse_main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &rockfuse_oper, NULL);
}
