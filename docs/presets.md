# YM7129 预设音色列表

预设数据存储在 `ym7129_config.ini` 的 `[Preset0]`~`[reset9]` 中。

寄存器说明：
- `feedback_lr`: bit7=FM/PM, bit6=R, bit5=L, bit2-0=FB(0-7)
- `m_mlws`: bit6=VIB, bit5=DM, bit4=EG-TYP, bit3-0=ML
- `m_tl`: Modulator Total Level (0-255, 255=静音)
- `m_ardr`: bit7-4=AR, bit3-0=DR
- `m_slrr`: bit7-4=SL, bit3-0=RR
- `c_mlws`: bit6=AM, bit5=WS, bit4=EG-TYP, bit3-0=ML
- `c_tl`: bit7=DP(FM/PM切换), bit6-0=Carrier Total Level (0-127)
- `c_ardr`: bit7-4=AR, bit3-0=DR
- `c_slrr`: bit7-4=SL, bit3-0=RR

---

## 预设列表

| # | 名称 | FB | L | R | FM/PM | M:AR | M:DR | M:SL | M:RR | M:TL | M:ML | M:EG | M:DM | M:VIB | C:AR | C:DR | C:SL | C:RR | C:TL | C:ML | C:EG | C:WS | C:AM | C:DP |
|---|------|----|---|---|-------|------|------|------|------|------|------|------|------|-------|------|------|------|------|------|------|------|------|------|------|
| 0 | Sine | 0 | ✓ | ✓ | PM | 0 | 0 | 0 | 0 | 255 | 0 | 0 | 0 | 0 | 15 | 1 | 0 | 5 | 34 | 1 | 1 | 0 | 0 | 0 |
| 1 | Piano | 0 | ✓ | ✓ | PM | 15 | 7 | 2 | 1 | 146 | 1 | 1 | 0 | 0 | 10 | 2 | 1 | 5 | 0 | 1 | 0 | 0 | 0 | 0 |
| 2 | E-Guitar | 7 | ✓ | ✓ | PM | 9 | 0 | 0 | 0 | 57 | 0 | 1 | 0 | 0 | 15 | 0 | 0 | 6 | 0 | 4 | 0 | 0 | 0 | 0 |
| 3 | Bell | 5 | ✓ | ✓ | PM | 15 | 6 | 3 | 2 | 148 | 15 | 0 | 1 | 1 | 11 | 5 | 4 | 4 | 0 | 4 | 0 | 0 | 0 | 0 |
| 4 | Acid-Bass | 0 | ✓ | ✓ | PM | 15 | 7 | 3 | 4 | 26 | 0 | 0 | 0 | 0 | 15 | 0 | 0 | 5 | 0 | 0 | 0 | 0 | 0 | 1 |
| 5 | Distortion-EG | 0 | ✓ | ✓ | PM | 11 | 8 | 3 | 3 | 0 | 0 | 0 | 0 | 0 | 9 | 0 | 0 | 4 | 0 | 4 | 0 | 0 | 0 | 1 |
| 6 | E-Piano | 7 | ✓ | ✓ | PM | 11 | 3 | 5 | 4 | 172 | 15 | 0 | 1 | 1 | 9 | 2 | 9 | 4 | 0 | 1 | 0 | 0 | 0 | 0 |
| 7 | User4 | - | - | - | FM | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 8 | User5 | - | - | - | FM | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 9 | User6 | - | - | - | FM | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

---

## 原始寄存器值

### Sine
```
feedback_lr=224  m_mlws=0    m_tl=255  m_ardr=0    m_slrr=0
c_mlws=17        c_tl=34     c_ardr=241 c_slrr=101
```

### Piano
```
feedback_lr=224  m_mlws=33   m_tl=146  m_ardr=247  m_slrr=33
c_mlws=1         c_tl=0      c_ardr=162 c_slrr=245
```

### E-Guitar
```
feedback_lr=231  m_mlws=16   m_tl=57   m_ardr=144  m_slrr=0
c_mlws=4         c_tl=0      c_ardr=240 c_slrr=246
```

### Bell
```
feedback_lr=229  m_mlws=47   m_tl=148  m_ardr=246  m_slrr=50
c_mlws=4         c_tl=0      c_ardr=181 c_slrr=68
```

### Acid-Bass
```
feedback_lr=224  m_mlws=0    m_tl=26   m_ardr=247  m_slrr=52
c_mlws=0         c_tl=128    c_ardr=240 c_slrr=101
```

### Distortion-EG
```
feedback_lr=224  m_mlws=0    m_tl=0    m_ardr=184  m_slrr=51
c_mlws=4         c_tl=128    c_ardr=144 c_slrr=100
```

### E-Piano
```
feedback_lr=231  m_mlws=47   m_tl=172  m_ardr=179  m_slrr=84
c_mlws=1         c_tl=0      c_ardr=146 c_slrr=148
```
