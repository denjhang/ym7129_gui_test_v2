#include "ym7129.h"
#include "spfm.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static void spfm_sleep_us(unsigned int us) {
    if (us >= 1000) {
        Sleep(us / 1000);
    } else {
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        double ticks = (double)us * freq.QuadPart / 1e6;
        do { QueryPerformanceCounter(&now); } while ((double)(now.QuadPart - start.QuadPart) < ticks);
    }
}
#else
#include <unistd.h>
static void spfm_sleep_us(unsigned int us) { usleep(us); }
#endif

// ============================================================
// Channel offset table (non-linear register mapping)
// Source: FITOMApp reverse engineering, confirmed in README
// Logical ch: 0  1  2  3  4  5  6  7
// Physical:   0  1  4  5  8  9 12 13
// ============================================================
const uint8_t ym7129_ch_offset[YM7129_NUM_CH] = {0, 1, 4, 5, 8, 9, 12, 13};

// ============================================================
// F-Number table: 3.579545 MHz clock, one octave (C..B)
// 11-bit values from FITOMApp Fnum358 table
// ============================================================
const uint16_t ym7129_fnum_table[12] = {
    0x2B1, 0x2E5, 0x311, 0x340,  // C  C# D  D#
    0x365, 0x398, 0x3DC, 0x409,  // E  F  F# G
    0x446, 0x488, 0x4DE, 0x516   // G# A  A# B
};

// ============================================================
// Per-slot channel state (2 SPFM slots max)
// ============================================================
static ym7129_ch_t ch_state[2][YM7129_NUM_CH];

// Drum trigger state (forward declaration for ym7129_init)
static uint8_t drum_trigger_state[2] = {0, 0};

// ============================================================
// Low-level register write (hardcoded timing: flush + 90us)
// YM7129 requires 89.2us between consecutive writes (wuqi verified)
// ============================================================
void ym7129_write_reg(uint8_t slot, uint8_t addr, uint8_t data) {
    uint8_t cmd[6];
    cmd[0] = slot; cmd[1] = 0x80; cmd[2] = addr;  // A0=0, latch address
    cmd[3] = slot; cmd[4] = 0x81; cmd[5] = data;  // A0=1, write data
    spfm_write_raw(cmd, 6);
    spfm_flush();
    spfm_sleep_us(90);
}

// ============================================================
// Silence all 8 channels
// Clear KON bit in reg 0x1x (Modulator) and 0x1x+2 (Carrier)
// Set TL (volume) to max attenuation (0x7F)
// ============================================================
void ym7129_mute(uint8_t slot) {
    for (int ch = 0; ch < YM7129_NUM_CH; ch++) {
        uint8_t off = ym7129_ch_offset[ch];
        // Clear KON on both operators
        ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off,     0x00);
        ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off + 2, 0x00);
        // Max attenuation
        ym7129_write_reg(slot, YM7129_REG_BASE_TL + off,       0x7F);
        ym7129_write_reg(slot, YM7129_REG_BASE_TL + off + 2,   0x7F);
    }
}

// ============================================================
// Initialize chip
// ============================================================
void ym7129_init(uint8_t slot) {
    memset(ch_state[slot], 0, sizeof(ch_state[slot]));

    // Test register must be FF
    ym7129_write_reg(slot, YM7129_REG_TEST, 0xFF);

    // Full register init per waveform analysis section 6:
    // All channels: Carrier MLWS=0x0D (ML=13,KON=0), Carrier SLRR=0x0B (SL=0,RR=11)
    // Modulator: MLWS=0, TL=max(mute), ARDR=0, SLRR=0
    // Carrier: TL=max, ARDR=0
    // Frequency: 0, L/R/FB: 0
    for (int ch = 0; ch < YM7129_NUM_CH; ch++) {
        uint8_t off = ym7129_ch_offset[ch];

        // Modulator
        ym7129_write_reg(slot, 0x10 + off,     0x00); // MLWS: KON=0, ML=0
        ym7129_write_reg(slot, 0x20 + off,     0xFF); // TL: max (mute)
        ym7129_write_reg(slot, 0x30 + off,     0x00); // ARDR: AR=0, DR=0
        ym7129_write_reg(slot, 0x40 + off,     0x00); // SLRR: SL=0, RR=0

        // Carrier
        ym7129_write_reg(slot, 0x10 + off + 2, 0x0D); // MLWS: KON=0, ML=13
        ym7129_write_reg(slot, 0x20 + off + 2, 0x7F); // TL: max (mute)
        ym7129_write_reg(slot, 0x30 + off + 2, 0x00); // ARDR: AR=0, DR=0
        ym7129_write_reg(slot, 0x40 + off + 2, 0x0B); // SLRR: SL=0, RR=11

        // Frequency
        ym7129_write_reg(slot, 0x50 + off,     0x00); // BLK=0, FNUM-H=0
        ym7129_write_reg(slot, 0x60 + off,     0x00); // FNUM-L=0

        // L/R/FB: enable L+R for all channels (also needed for drum output routing)
        ym7129_write_reg(slot, 0x70 + ch,      0x60); // L+R, FB=0
    }
}
}

// ============================================================
// Load patch onto one logical channel
// Based on OPK.cpp UpdateVoice:
//   MLWS  @ 0x10+off / 0x12+off  (bit7=KON, bit6=VIB, bit5=WS, bit3-0=MUL)
//   TL    @ 0x20+off (mod, value<<1) / 0x22+off (car, bit6-0)
//   ARDR  @ 0x30+off / 0x32+off  ((AR>>3)<<4 | (DR>>3))
//   SLRR  @ 0x40+off / 0x42+off  ((SL>>3)<<4 | (SR>>3))
//   LRFB  @ 0x70+ch  (linear, bit6=R, bit5=L, bit2-0=FB)
// ============================================================
void ym7129_load_patch(uint8_t slot, uint8_t ch, const ym7129_patch_t *p) {
    if (ch >= YM7129_NUM_CH) return;
    uint8_t off = ym7129_ch_offset[ch];

    // L/R/FB (linear per channel, not per operator)
    ym7129_write_reg(slot, YM7129_REG_BASE_LRFB + ch, p->feedback_lr);

    // Modulator (off): no KON bit on modulator, write directly
    ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off,     p->m_mlws & 0x7F);
    ym7129_write_reg(slot, YM7129_REG_BASE_TL   + off,     p->m_tl);
    ym7129_write_reg(slot, YM7129_REG_BASE_ARDR + off,     p->m_ardr);
    ym7129_write_reg(slot, YM7129_REG_BASE_SLRR + off,     p->m_slrr);

    // Carrier (off+2): preserve current KON state, only write bit6-0
    // KON must be strictly managed by note_on/note_off/note_kill only
    uint8_t cur_kon = ch_state[slot][ch].active ? 0x80 : 0x00;
    ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off + 2, (p->c_mlws & 0x7F) | cur_kon);
    // Carrier TL: bit6-0 direct (velocity overrides at note-on)
    ym7129_write_reg(slot, YM7129_REG_BASE_TL   + off + 2, p->c_tl & 0x7F);
    ym7129_write_reg(slot, YM7129_REG_BASE_ARDR + off + 2, p->c_ardr);
    ym7129_write_reg(slot, YM7129_REG_BASE_SLRR + off + 2, p->c_slrr);

    // Cache carrier params for note-on/off (without KON)
    ch_state[slot][ch].c_mlws = p->c_mlws & 0x7F;
    ch_state[slot][ch].c_slrr = p->c_slrr;
}

// ============================================================
// Note On
// ============================================================
void ym7129_note_on(uint8_t slot, uint8_t ch, uint8_t midi_note, uint8_t velocity) {
    if (ch >= YM7129_NUM_CH) return;
    uint8_t off = ym7129_ch_offset[ch];

    // Calculate BLK and FNUM
    // MIDI note: C-1=0, C0=12, C4=60
    // BLK = octave-1 (clamp 0-7), FNUM from table
    uint8_t  octave = midi_note / 12;
    uint8_t  note12 = midi_note % 12;
    uint8_t  blk    = (octave > 0) ? (octave - 1) : 0;
    if (blk > 7) blk = 7;
    uint16_t fnum   = ym7129_fnum_table[note12];

    ch_state[slot][ch].active    = true;
    ch_state[slot][ch].midi_note = midi_note;
    ch_state[slot][ch].fnum      = fnum;
    ch_state[slot][ch].blk       = blk;

    // Carrier TL from velocity: 0=loud(0), 127=quiet(63)
    uint8_t c_tl = (uint8_t)((127 - velocity) >> 1);

    // Write frequency registers (OPK.cpp UpdateFreq):
    // If fnum >= 0x800, shift right and increment block (avoid overflow)
    if (fnum & 0x800) { fnum >>= 1; if (blk < 7) blk++; }
    // Reg 0x52+off: (blk<<5) | (fnum>>6)
    // Reg 0x62+off: (fnum<<2) & 0xF0  -- high-nibble only
    ym7129_write_reg(slot, 0x52 + off, ((blk & 0x07) << 5) | ((fnum >> 6) & 0x1F));
    ym7129_write_reg(slot, 0x62 + off, (uint8_t)((fnum << 2) & 0xF0));

    // Carrier TL from velocity (0=loud, 127=quiet), bit6-0
    ym7129_write_reg(slot, YM7129_REG_BASE_TL + off + 2, c_tl & 0x7F);

    // Key-On: set bit7 of Carrier MLWS (0x12+off = 0x10+off+2)
    // OPK.cpp UpdateKey: SetReg(0x12+chofs[ch], (cur & 0x7f) | 0x80)
    ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off + 2,
                     (ch_state[slot][ch].c_mlws & 0x7F) | 0x80);
}

// ============================================================
// Note Off
// ============================================================
void ym7129_note_off(uint8_t slot, uint8_t ch) {
    if (ch >= YM7129_NUM_CH) return;
    uint8_t off = ym7129_ch_offset[ch];

    ch_state[slot][ch].active = false;

    // Note-off: clear KON, switch to release rate (OPK.cpp UpdateKey)
    // SLRR = (SL<<4) | RR  where RR comes from c_slrr low nibble
    uint8_t rr = ch_state[slot][ch].c_slrr & 0x0F;
    uint8_t sl = (ch_state[slot][ch].c_slrr >> 4) & 0x0F;
    ym7129_write_reg(slot, YM7129_REG_BASE_SLRR + off + 2, (sl << 4) | rr);
    ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off + 2,
                     ch_state[slot][ch].c_mlws & 0x7F); // KON=0
}

// ============================================================
// Note Kill (immediate silence, no release envelope)
// ============================================================
void ym7129_note_kill(uint8_t slot, uint8_t ch) {
    if (ch >= YM7129_NUM_CH) return;
    uint8_t off = ym7129_ch_offset[ch];

    ch_state[slot][ch].active = false;

    // Immediate kill: first silence carrier, then clear KON
    // Order matters: TL=max before KON=0 prevents any sound glitch
    ym7129_write_reg(slot, YM7129_REG_BASE_TL + off + 2, 0x7F);     // carrier TL=max (silent)
    ym7129_write_reg(slot, YM7129_REG_BASE_MLWS + off + 2,
                     ch_state[slot][ch].c_mlws & 0x7F);              // KON=0
}

// ============================================================
// Higher-level: find free channel and play
// ============================================================
int ym7129_play_note(uint8_t slot, uint8_t midi_note, uint8_t velocity) {
    for (int ch = 0; ch < YM7129_NUM_CH; ch++) {
        if (!ch_state[slot][ch].active) {
            ym7129_note_on(slot, (uint8_t)ch, midi_note, velocity);
            return ch;
        }
    }
    return -1; // no free channel
}

// ============================================================
// Stop note by midi_note value
// ============================================================
void ym7129_stop_note(uint8_t slot, uint8_t midi_note) {
    for (int ch = 0; ch < YM7129_NUM_CH; ch++) {
        if (ch_state[slot][ch].active && ch_state[slot][ch].midi_note == midi_note) {
            ym7129_note_off(slot, (uint8_t)ch);
        }
    }
}

// ============================================================
// Channel state access
// ============================================================
const ym7129_ch_t *ym7129_get_ch_state(uint8_t slot, uint8_t ch) {
    if (ch >= YM7129_NUM_CH) return NULL;
    return &ch_state[slot][ch];
}

// ============================================================
// Drum: strike one channel with 0→1 rising edge on $84
// ============================================================

void ym7129_drum_strike(uint8_t slot, uint8_t channel, uint8_t rhy_type, uint8_t lh, uint8_t volume) {
    if (channel > 3) return;

    // Mute all drums first (reset $84 to 0)
    drum_trigger_state[slot] = 0;
    ym7129_write_reg(slot, 0x84, 0x00);

    // Write $78-$7F = L+R (possibly drum channel pan, undocumented)
    for (int i = 0; i < 8; i++)
        ym7129_write_reg(slot, 0x78 + i, 0x60);

    // Write rhythm type/volume to $80+channel
    uint8_t type_val = (lh << 7) | ((rhy_type & 7) << 4) | (volume & 0x0F);
    ym7129_write_reg(slot, 0x80 + channel, type_val);

    // Clear target bit, then set (rising edge trigger)
    uint8_t bit_mask = (1 << channel);
    drum_trigger_state[slot] &= ~bit_mask;
    ym7129_write_reg(slot, 0x84, drum_trigger_state[slot]);

    drum_trigger_state[slot] |= bit_mask;
    ym7129_write_reg(slot, 0x84, drum_trigger_state[slot]);
}

void ym7129_drum_mute(uint8_t slot) {
    drum_trigger_state[slot] = 0;
    ym7129_write_reg(slot, 0x84, 0x00);
}
