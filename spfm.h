/* See LICENSE for licence details. */
#ifndef SPFM_H
#define SPFM_H

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include <stdint.h>
#include <stdbool.h>

#include "ftd2xx.h"
#include "error.h"

typedef FT_HANDLE SPFM_HANDLE;

typedef enum {
    SPFM_TYPE_UNKNOWN,
    SPFM_TYPE_SPFM,
    SPFM_TYPE_SPFM_LIGHT
} SPFM_TYPE;

typedef struct {
    uint8_t port;
    uint8_t addr;
    uint8_t data;
} spfm_reg_t;

#define OPNA_SLOT_NUM 0
#define OPM_SLOT_NUM  1

#define BUFSIZE 256
#define SPFM_WRITE_BUF_SIZE (1024 * 64)

#ifdef __cplusplus
extern "C" {
#endif

int spfm_init(int dev_idx);
int spfm_get_selected_device_index(void);
void spfm_init_chips(void);
void spfm_cleanup(void);
SPFM_HANDLE spfm_get_handle(void);
SPFM_TYPE spfm_get_type(void);
void spfm_reset(void);
void spfm_chip_reset(void);
void spfm_write_reg(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data);
void spfm_write_reg_0x20(uint8_t slot, uint8_t addr, uint8_t data);
void spfm_write_reg_cmd(uint8_t slot, uint8_t cmd_byte, uint8_t addr, uint8_t data);
void spfm_write_regs(uint8_t slot, const spfm_reg_t* regs, uint32_t count, uint32_t write_wait);
void spfm_write_data(uint8_t slot, uint8_t data);
void spfm_write_raw(const uint8_t *data, uint32_t len);
void spfm_hw_wait(uint32_t samples);
void spfm_wait_and_write_reg(uint32_t wait_samples, uint8_t slot, uint8_t port, uint8_t addr, uint8_t data);
bool spfm_flush(void);
void spfm_write_ym2608_ram(uint8_t slot, uint32_t address, uint32_t size, const uint8_t* data);
void spfm_write_ym2608_ram_timed(uint8_t slot, uint32_t address, uint32_t size, const uint8_t* data);
int spfm_get_dev_index(void);

#ifdef __cplusplus
}
#endif

#endif /* SPFM_H */
