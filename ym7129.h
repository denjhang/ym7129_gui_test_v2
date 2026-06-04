#ifndef YM7129_H
#define YM7129_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// YM7129 (OPK) Register Map
// Analyzed by madscient. Reference: opk_register html/Sheet1.html
// ============================================================

// Test register - set to 0xFF on init
#define YM7129_REG_TEST         0x01

// Reg 0x10-0x1F: Kon / VIB / AM / WS / DM / EG-TYP / ML (wuqi verified)
// Modulator (odd offset): Bit7=unused, Bit6=VIB, Bit5=DM, Bit4=EG-TYP, Bit3-0=ML
// Carrier   (even offset+2): Bit7=KON, Bit6=AM, Bit5=WS, Bit4=EG-TYP, Bit3-0=ML
#define YM7129_REG_BASE_MLWS    0x10

// Reg 0x20-0x2F: Total Level
// Modulator (even offset): Bit7-0: TL (8bit, 0=loudest, 255=quietest)
// Carrier   (odd offset+2): Bit7=DP (force damping), Bit6-0: Volume (0=loudest)
#define YM7129_REG_BASE_TL      0x20

// Reg 0x30-0x3F: AR / DR
// Bit7-4: AR, Bit3-0: DR
#define YM7129_REG_BASE_ARDR    0x30

// Reg 0x40-0x4F: SL / SR(RR)
// Bit7-4: SL, Bit3-0: RR
#define YM7129_REG_BASE_SLRR    0x40

// Reg 0x50-0x5F: BLK / F-NUM MSB
// Bit7-5: BLK(0-7), Bit4-0: FNUM[10:6]
#define YM7129_REG_BASE_BLKFHI  0x50

// Reg 0x60-0x6F: F-NUM LSB
// Bit7-2: FNUM[5:0] (left-justified)
#define YM7129_REG_BASE_FLO     0x60

// Reg 0x70-0x77: ? / R / L / FB  (per channel, linear)
// Bit6: R, Bit5: L, Bit2-0: FB
#define YM7129_REG_BASE_LRFB    0x70

// Reg 0x80-0x83: Rhythm channel control
// Bit7: L/H, Bit6-4: Rhythm No, Bit3-0: Volume
#define YM7129_REG_BASE_RHY     0x80

// Reg 0x84: Rhythm key-on flags  Bit3:k3 Bit2:k2 Bit1:k1 Bit0:k0
#define YM7129_REG_RHY_KEYON    0x84

// ============================================================
// Channel offset table (non-linear register mapping)
// Logical ch 0-7 -> physical offset: {0,1,4,5,8,9,12,13}
// Modulator reg = base + ch_offset[ch]
// Carrier  reg = base + ch_offset[ch] + 2
// ============================================================
#define YM7129_NUM_CH 8
extern const uint8_t ym7129_ch_offset[YM7129_NUM_CH];

// ============================================================
// F-Number table for 3.579545 MHz clock (one octave, C..B)
// 11-bit FNUM values from FITOMApp Fnum358 table
// ============================================================
extern const uint16_t ym7129_fnum_table[12];

// ============================================================
// Patch (voice) definition
// ============================================================
typedef struct {
    uint8_t feedback_lr; // written to reg 0x7x  (FB + L/R)
    uint8_t m_mlws;      // Modulator: reg 0x1x
    uint8_t m_tl;        // Modulator: reg 0x2x
    uint8_t m_ardr;      // Modulator: reg 0x3x
    uint8_t m_slrr;      // Modulator: reg 0x4x
    uint8_t c_mlws;      // Carrier:   reg 0x1x+2
    uint8_t c_tl;        // Carrier:   reg 0x2x+2  (set by velocity at note-on)
    uint8_t c_ardr;      // Carrier:   reg 0x3x+2
    uint8_t c_slrr;      // Carrier:   reg 0x4x+2
} ym7129_patch_t;

// ============================================================
// Channel state
// ============================================================
typedef struct {
    bool     active;
    uint8_t  midi_note;
    uint16_t fnum;
    uint8_t  blk;
    uint8_t  c_mlws;   // cached carrier MLWS (without KON bit)
    uint8_t  c_slrr;   // cached carrier SLRR (for note-off release)
    uint8_t  c_tl;     // cached carrier TL (bit6-0 volume)
} ym7129_ch_t;

// ============================================================
// API
// ============================================================

// Low-level: write one register (slot = SPFM slot 0 or 1)
void ym7129_write_reg(uint8_t slot, uint8_t addr, uint8_t data);

// Set per-write delay in microseconds (default 80)
void ym7129_set_write_delay(uint32_t us);

// Initialize chip: reset all regs, load patch on all channels
void ym7129_init(uint8_t slot);

// Silence all channels immediately
void ym7129_mute(uint8_t slot);

// Load a patch onto one logical channel (0-7)
void ym7129_load_patch(uint8_t slot, uint8_t ch, const ym7129_patch_t *p);

// Trigger a note-on  (midi_note 0-127, velocity 0-127)
void ym7129_note_on(uint8_t slot, uint8_t ch, uint8_t midi_note, uint8_t velocity);

// Trigger a note-off (release envelope)
void ym7129_note_off(uint8_t slot, uint8_t ch);

// Immediately kill a note (Kon=0 + max TL, no release)
void ym7129_note_kill(uint8_t slot, uint8_t ch);

// Higher-level: find a free channel and play note (returns ch used, -1 if none)
int  ym7129_play_note(uint8_t slot, uint8_t midi_note, uint8_t velocity);

// Stop a note by midi_note value (searches active channels)
void ym7129_stop_note(uint8_t slot, uint8_t midi_note);

// Channel state access
const ym7129_ch_t *ym7129_get_ch_state(uint8_t slot, uint8_t ch);

// Drum strike: set drum type then trigger 0→1 rising edge on $84
// channel: 0-3 ($80-$83), rhy_type: 0=BD,1=SD,2=C.HH,3=O.HH,4=Tom,5=Ride
// lh: 0=H(hi), 1=L(low), volume: 0=max, 15=min
void ym7129_drum_strike(uint8_t slot, uint8_t channel, uint8_t rhy_type, uint8_t lh, uint8_t volume);

// Drum mute: set $84 all bits to 0
void ym7129_drum_mute(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* YM7129_H */
