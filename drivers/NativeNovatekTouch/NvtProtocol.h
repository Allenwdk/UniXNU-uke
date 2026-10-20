// SPDX-License-Identifier: Apache-2.0
// NT36532E host-download protocol over an abstract SPI transport. Register facts follow the study of the
// Linux nt36532e driver (no code copied); see NOTICE. No board addresses or clocks here.
#ifndef NVT_PROTOCOL_H
#define NVT_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define NVT_TRANSFER_LEN (63*1024)
#define NVT_THP_HEADER_LEN 257
#define NVT_THP_PAYLOAD_LEN 5160
#define NVT_THP_FRAME_LEN (NVT_THP_HEADER_LEN+NVT_THP_PAYLOAD_LEN)
#define NVT_THP_MATRIX_OFFSET (NVT_THP_HEADER_LEN+0x40)
#define NVT_MAX_PARTITIONS 64
#define NVT_READ_MAX 8192

#define NVT_ERR_BUS (-5)
#define NVT_ERR_PARAM (-22)
#define NVT_ERR_FW (-8)
#define NVT_ERR_CHIP (-19)
#define NVT_ERR_TIMEOUT (-110)

// Transport: full-duplex SPI mode 0, one transfer, chip select held for the whole transfer. rx may alias nothing.
struct nvt_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
};

struct nvt_reg {uint32_t addr; uint8_t mask;};
struct nvt_map {
    const char *name;
    uint32_t event_buf, boot_rdy, tx_auto_copy_en, spi_dma_tx_info;
    uint32_t ilm_des, ilm_length, dlm_des, dlm_length;
    uint32_t g_ilm_checksum, g_dlm_checksum, r_ilm_checksum, r_dlm_checksum, bld_ilm_dlm_crc;
    struct nvt_reg enb_casc;
};

struct nvt_chip {
    const struct nvt_bus *bus;
    uint8_t *scratch;                 // transmit buffer: caller supplied, at least NVT_TRANSFER_LEN + 8 bytes
    uint8_t *rxbuf;                   // receive buffer: caller supplied, at least NVT_READ_MAX + 2 bytes
    const struct nvt_map *map;
    uint32_t chip_ver_trim, swrst_sif;
    int cascade;                      // cascade variant selected by ENB_CASC_REG
    int hw_crc_len;                   // 3: 3-byte HW CRC bank (NT36532 family)
    uint8_t id[6];
    // Download diagnostics (what the last nvt_download_firmware attempt was doing when it stopped).
    unsigned dl_attempt, dl_stage;    // stage: 1 reset, 2 crc setup, 3 auto-copy enable, 4 firmware write, 5 auto-copy wait,
                                      //        6 crc enable, 7 boot ready, 8 reset-state check
    int dl_rc;                        // result of the failing stage
    uint8_t dl_wait_value;            // last value read while waiting for auto copy to finish
    uint8_t dl_reset_state[5];        // last bytes read from the reset-complete event
    unsigned dl_polls;                // polls spent in the failing wait
    // Bootloader CRC state read back after a failed download (nvt_read_bld_hw_crc of the vendor driver).
    uint8_t crc_flags;                // bit2 crc_done, bit0 ilm ok, bit1 dlm ok
    uint32_t crc_bin[2], crc_golden[2], crc_result[2];
    int crc_valid;
    // SRAM read-back after a failed download: 1 = the 32 bytes read match the firmware image. [0] ILM start, [1] ILM end, [2] DLM start.
    int sram_valid;
    uint8_t sram_ok[3];
    int32_t sram_first_bad[2];        // full read-back of ILM/DLM: first differing byte offset (-1 none)
    uint32_t sram_bad[2], sram_crc[2];// differing byte count and the CRC of the read-back words (same algorithm as the image CRC)
    unsigned reset_retries;           // reset-state polls before giving up (0 = the vendor's 10); one poll every 10 ms
};

struct nvt_fw_partition {
    char name[12];
    uint32_t bin_addr, sram_addr, size, crc;
};
struct nvt_fw {
    const uint8_t *data;
    size_t size;
    unsigned count;                   // partitions
    int cascade_2nd_header;
    struct nvt_fw_partition part[NVT_MAX_PARTITIONS];
};
// Read the bootloader's CRC flags and golden/result checksums (the vendor driver prints these when a download fails).
int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw);
// Compare small windows of the downloaded ILM/DLM in SRAM with the image (integrity check of the bulk transfers).
int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw);

// Host-side CRC used for ILM/DLM (word-wise CRC-32, polynomial 0x04c11db7, covers (size+4)/4 words).
uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail);   // bytes beyond avail read as 0

int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len);   // sets the page first
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len);
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value);
int nvt_set_page(struct nvt_chip *c, uint32_t addr);

int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *data, size_t size);
// Bootloader reset, read the trim id and select the NT36532 / NT36532E memory map.
int nvt_identify(struct nvt_chip *c);
// Full host download with HW CRC and boot-ready handshake; returns 0 once the IC reports reset state >= 0xa0.
int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw);
// Read one THP frame (NVT_THP_FRAME_LEN bytes) from the event buffer.
int nvt_read_frame(struct nvt_chip *c, uint8_t *frame);
// Extract the 40x60 int16 touch matrix and header fields from a frame.
void nvt_frame_matrix(const uint8_t *frame, int16_t *matrix);
unsigned nvt_frame_type(const uint8_t *frame);
unsigned nvt_frame_counter(const uint8_t *frame);

// After the download (mirrors the Linux boot flow): firmware info, idle-baseline update, doze delay, doze mode.
#define NVT_EVENT_HOST_CMD 0x50
#define NVT_EVENT_FWINFO 0x78
#define NVT_DOZE_DELAY_ADDR 0x113718
struct nvt_fw_info {
    uint8_t fw_ver, fw_type;
    uint8_t x_num, y_num;
    uint16_t abs_x_max, abs_y_max, pid;
    int defaulted;                    // info block was inconsistent after retries
};
int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info);
// Host command {0xbf, cmd, 0, lo, hi} at the event buffer; the firmware clears the first byte when it has taken it.
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value);
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value);
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info);
// Header integrity of a THP frame: the CRC/marker words at header+4/+8 and their complements at +12/+16.
int nvt_frame_header_valid(const uint8_t *frame);
// A frame is announced by a 32-byte window of its THP header (frame[NVT_THP_HEADER_LEN..+32)): cheap to read, it carries the
// integrity words, the counter and the type, so the full 5 KB frame is only read when it is new.
#define NVT_THP_PEEK_LEN 32
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr);   // hdr: NVT_THP_PEEK_LEN bytes
int nvt_peek_valid(const uint8_t *hdr);
unsigned nvt_peek_counter(const uint8_t *hdr);
unsigned nvt_peek_type(const uint8_t *hdr);
#endif
