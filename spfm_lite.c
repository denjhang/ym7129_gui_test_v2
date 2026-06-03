/**
 * spfm_lite.c
 * Minimal SPFM Light interface for YM7129 standalone test.
 * Implements only the functions required by ym7129.c:
 *   spfm_init, spfm_cleanup, spfm_write_reg, spfm_flush
 *
 * Protocol (SPFM Light, 4 bytes per command):
 *   byte0: slot (0 or 1)
 *   byte1: (port & 7) << 1
 *   byte2: register address
 *   byte3: data
 */

#include "spfm.h"
#include "ftd2xx.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
static void spfm_sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void spfm_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
#endif

#define SPFM_LITE_BUF_SIZE (1024 * 64)

static FT_HANDLE s_handle = NULL;
static uint8_t   s_buf[SPFM_LITE_BUF_SIZE];
static DWORD     s_buf_ptr = 0;

/* ------------------------------------------------------------------ */
bool spfm_flush(void) {
    if (!s_handle || s_buf_ptr == 0) return true;
    DWORD total = 0;
    while (total < s_buf_ptr) {
        DWORD chunk = s_buf_ptr - total;
        if (chunk > 4096) chunk = 4096;
        DWORD written = 0;
        FT_STATUS st = FT_Write(s_handle, s_buf + total, chunk, &written);
        if (st != FT_OK || written != chunk) {
            fprintf(stderr, "spfm_flush: FT_Write failed (st=%d written=%lu)\n",
                    (int)st, (unsigned long)written);
            s_buf_ptr = 0;
            return false;
        }
        total += written;
    }
    s_buf_ptr = 0;
    return true;
}
/* ------------------------------------------------------------------ */
void spfm_write_reg(uint8_t slot, uint8_t port, uint8_t addr, uint8_t data) {
    if (!s_handle) return;
    /* SPFM Light: 4-byte command */
    uint8_t cmd[4];
    cmd[0] = slot & 1;
    cmd[1] = (port & 7) << 1;
    cmd[2] = addr;
    cmd[3] = data;
    if (s_buf_ptr + 4 > SPFM_LITE_BUF_SIZE) spfm_flush();
    memcpy(s_buf + s_buf_ptr, cmd, 4);
    s_buf_ptr += 4;
}

/* ------------------------------------------------------------------ */
/* 0x20 mode: [slot, 0x20, addr, data] - SN76489-style with embedded address */
void spfm_write_reg_0x20(uint8_t slot, uint8_t addr, uint8_t data) {
    if (!s_handle) return;
    /* SPFM Light 0x20 mode: 4-byte command with fixed 0x20 command byte */
    uint8_t cmd[4];
    cmd[0] = slot & 1;
    cmd[1] = 0x20;  /* Fixed command byte for 0x20 mode */
    cmd[2] = addr;
    cmd[3] = data;
    if (s_buf_ptr + 4 > SPFM_LITE_BUF_SIZE) spfm_flush();
    memcpy(s_buf + s_buf_ptr, cmd, 4);
    s_buf_ptr += 4;
}

/* 可配置命令字节模式: [slot, cmd_byte, addr, data] */
void spfm_write_reg_cmd(uint8_t slot, uint8_t cmd_byte, uint8_t addr, uint8_t data) {
    if (!s_handle) return;
    /* SPFM Light 可配置命令字节模式: 4-byte command */
    uint8_t cmd[4];
    cmd[0] = slot & 1;
    cmd[1] = cmd_byte;  /* 可配置的命令字节 */
    cmd[2] = addr;
    cmd[3] = data;
    if (s_buf_ptr + 4 > SPFM_LITE_BUF_SIZE) spfm_flush();
    memcpy(s_buf + s_buf_ptr, cmd, 4);
    s_buf_ptr += 4;
}

/* ------------------------------------------------------------------ */
/* 3-byte raw mode: [slot, 0x80, addr] [slot, 0x81, data]
 * YM7129 A0 bus: 0x80 = A0=0 (address), 0x81 = A0=1 (data) */
void spfm_write_raw(const uint8_t *raw, uint32_t len) {
    if (!s_handle || len == 0) return;
    if (s_buf_ptr + len > SPFM_LITE_BUF_SIZE) spfm_flush();
    memcpy(s_buf + s_buf_ptr, raw, len);
    s_buf_ptr += len;
}

/* ------------------------------------------------------------------ */
void spfm_hw_wait(uint32_t samples) {
    for (uint32_t i = 0; i < samples; i++) {
        if (s_buf_ptr + 1 > SPFM_LITE_BUF_SIZE) spfm_flush();
        s_buf[s_buf_ptr++] = 0x80;
    }
}

/* ------------------------------------------------------------------ */
void spfm_wait_and_write_reg(uint32_t wait_samples, uint8_t slot, uint8_t port, uint8_t addr, uint8_t data) {
    /* Per yasp analysis: hardware 0x80 bytes are reliable for <10 samples (~226us).
     * For longer waits, flush first then use OS sleep. */
    const uint32_t HW_WAIT_THRESHOLD = 10;
    if (wait_samples > 0) {
        if (wait_samples < HW_WAIT_THRESHOLD) {
            spfm_hw_wait(wait_samples);
        } else {
            spfm_flush();
            unsigned int us = (unsigned int)((uint64_t)wait_samples * 1000000 / 44100);
#ifdef _WIN32
            if (us >= 1000)
                Sleep(us / 1000);
            else {
                /* busy-wait for sub-ms */
                LARGE_INTEGER freq, start, now;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&start);
                double ticks = (double)us * freq.QuadPart / 1e6;
                do { QueryPerformanceCounter(&now); }
                while ((double)(now.QuadPart - start.QuadPart) < ticks);
            }
#else
            usleep(us);
#endif
        }
    }
    spfm_write_reg(slot, port, addr, data);
}

/* ------------------------------------------------------------------ */
void spfm_write_regs(uint8_t slot, const spfm_reg_t* regs, uint32_t count, uint32_t write_wait) {
    for (uint32_t i = 0; i < count; i++) {
        spfm_write_reg(slot, regs[i].port, regs[i].addr, regs[i].data);
        for (uint32_t j = 0; j < write_wait; j++) {
            if (s_buf_ptr + 1 > SPFM_LITE_BUF_SIZE) spfm_flush();
            s_buf[s_buf_ptr++] = 0x80;
        }
    }
}

void spfm_write_data(uint8_t slot, uint8_t data) {
    /* SPFM Light data-only write: slot, 0x20, data
     * Used for chips like SN76489 that don't need separate address */
    if (!s_handle) return;
    if (s_buf_ptr + 3 > SPFM_LITE_BUF_SIZE) spfm_flush();
    s_buf[s_buf_ptr++] = slot & 1;
    s_buf[s_buf_ptr++] = 0x20;  // Data-only command flag
    s_buf[s_buf_ptr++] = data;
}
/* ------------------------------------------------------------------ */
int spfm_init(int dev_idx) {
    FT_STATUS st;
    s_buf_ptr = 0;

    st = FT_Open(dev_idx, &s_handle);
    if (st != FT_OK) {
        fprintf(stderr, "spfm_init: FT_Open(%d) failed, status=%d\n", dev_idx, (int)st);
        return -1;
    }

    FT_SetBaudRate(s_handle, 1500000);
    FT_SetDataCharacteristics(s_handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetFlowControl(s_handle, FT_FLOW_NONE, 0, 0);
    FT_SetTimeouts(s_handle, 100, 100);
    FT_SetLatencyTimer(s_handle, 2);
    FT_SetUSBParameters(s_handle, 65536, 65536);
    FT_Purge(s_handle, FT_PURGE_RX | FT_PURGE_TX);

    /* Reset sequence */
    FT_SetDtr(s_handle); FT_SetRts(s_handle);
    spfm_sleep_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_sleep_ms(1);
    FT_SetRts(s_handle);
    spfm_sleep_ms(1);
    FT_ClrDtr(s_handle); FT_ClrRts(s_handle);
    spfm_sleep_ms(10);

    /* Send hardware reset command (0xFE) */
    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(100);
    FT_Purge(s_handle, FT_PURGE_RX | FT_PURGE_TX);

    printf("spfm_init: SPFM Light opened on device %d\n", dev_idx);
    return 0;
}

void spfm_cleanup(void) {
    if (!s_handle) return;
    spfm_flush();
    /* Silence: send hardware reset */
    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(50);
    FT_Close(s_handle);
    s_handle = NULL;
}

void spfm_reset(void) {
    if (!s_handle) return;
    spfm_flush();
    uint8_t reset_cmd = 0xFE;
    DWORD written = 0;
    FT_Write(s_handle, &reset_cmd, 1, &written);
    spfm_sleep_ms(50);
}

void spfm_chip_reset(void) { spfm_reset(); }

void spfm_init_chips(void) { /* not needed for ym7129 standalone */ }

SPFM_HANDLE spfm_get_handle(void) { return s_handle; }
SPFM_TYPE   spfm_get_type(void)   { return SPFM_TYPE_SPFM_LIGHT; }
int spfm_get_dev_index(void)       { return -1; }
int spfm_get_selected_device_index(void) { return -1; }

void spfm_write_ym2608_ram(uint8_t slot, uint32_t address, uint32_t size, const uint8_t* data) {
    (void)slot; (void)address; (void)size; (void)data;
}
void spfm_write_ym2608_ram_timed(uint8_t slot, uint32_t address, uint32_t size, const uint8_t* data) {
    (void)slot; (void)address; (void)size; (void)data;
}


