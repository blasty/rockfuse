#ifndef __ROCKUSB_H__
#define __ROCKUSB_H__

#include <stdint.h>

// "USBC"
#define USBMS_CBW_SIG 0x43425355
// "USBS"
#define USBMS_CSW_SIG 0x53425355

#define CBW_READ_FLASH_ID    0x01
#define CBW_READ_LBA         0x14
#define CBW_WRITE_LBA        0x15
#define CBW_READ_FLASH_INFO  0x1a

#pragma pack(1)

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

int rockusb_init();
int rockusb_read_flash_id(uint8_t *flash_id_out);
int rockusb_read_flash_info(flash_info_t *flash_info);
int rockusb_read_lba(uint32_t offset, uint32_t count, uint8_t *buf);
int rockusb_write_lba(uint32_t offset, uint32_t count, uint8_t *buf);

#endif
