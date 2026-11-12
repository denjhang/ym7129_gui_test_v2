# YM7129 Drum Pad GUI

YM7129 音色编辑器 + 鼓垫 + FM 钢琴键盘，基于 ImGui + DirectX11，使用 SPFM Light 3字节 raw 协议。

## 功能

### 鼓垫 (Drum Pad)
- 12 个鼓垫按钮：BD / SD / C.HH / O.HH / Tom / Ride × 2 (L/H)
- 鼠标点击触发 + 击打动画
- 键盘快捷键：`0-5` (L), `7,8` (H), `Num*/-` (Tom L/H), `Num//+` (Ride L/H)
- `[` `]` 调节音量 (0-15)

### FM 钢琴键盘 (Piano)
- 可视化钢琴键盘（C2-B7），鼠标点击弹奏
- 主键盘映射：
  - `Z-M` 行: C3-E4 白键
  - `S/D/G/H/J/K/L` 行: 黑键
  - `Q-U` 行: C4-G5 白键
  - `2/3/5/6/7/8/9/0`: 黑键
- `PageUp/PageDown` 切换八度 (0-5)
- `Velocity` 滑块控制力度 (0-127)

### FM 音色编辑器 (FM Voice)
- 双算子 (Modulator + Carrier) 参数编辑
- 每个算子独立控制：AR, DR, SL, RR, TL, ML, EG-TYP, DM/WS, VIB/AM, DP
- 全局参数：FB (Feedback 0-7), L/R 声道, FM/PM 模式切换
- 波形预览 + ADSR 包络预览
- 实时调整参数并听到变化

### 预设系统
- 10 个预设槽，支持自定义名称（双击按钮编辑）
- 点击切换预设，松开鼠标自动保存到当前预设槽
- 所有预设自动持久化到 `ym7129_config.ini`
- 预设音色详见 [docs/presets.md](docs/presets.md)

### 其他
- Connect / Disconnect / Reset 按钮
- 延时滑块 (0-200us)
- 寄存器调试面板 ($70-$88)
- TEST-E 寄存器直接控制

## 内置预设

| # | 名称 | 说明 |
|---|------|------|
| 0 | Sine | 纯正弦波 |
| 1 | Piano | 钢琴音色 |
| 2 | E-Guitar | 电吉他 |
| 3 | Bell | 铃音 |
| 4 | Acid-Bass | 酸性贝斯 |
| 5 | Distortion-EG | 失真包络 |
| 6 | E-Piano | 电钢琴 |
| 7 | Accordion | 手风琴 |
| 8 | Guitar | 吉他 |
| 9 | Organ | 风琴 |

## 协议

SPFM Light 3字节 raw 模式：`[slot, 0x80, addr] [slot, 0x81, data]`
- 0x80 = A0=0 地址锁存
- 0x81 = A0=1 数据写入

## YM7129 FM 寄存器

YM7129 为 2-op FM 合成器，兼容 YM2413 (OPLL) 风格。每个通道有 2 个算子（Modulator + Carrier）。

| 寄存器 | 位 | 说明 |
|--------|-----|------|
| `$10+$off` | bit6=VIB/AM, bit5=DM/WS, bit4=EG-TYP, bit3-0=ML | Modulator 多功能 |
| `$20+$off` | bit7-0 | Modulator TL (0-255) |
| `$30+$off` | bit7-4=AR, bit3-0=DR | AR / DR |
| `$40+$off` | bit7-4=SL, bit3-0=RR | SL / RR |
| `$50+$off` | bit7-4=BLK, bit3-0=FNUM-H | 频率高位 |
| `$60+$off` | bit7-0 | FNUM-L 频率低位 |
| `$70+ch` | bit7=FM/PM, bit6=R, bit5=L, bit2-0=FB | L/R/FB 控制 |

## 编译

```bash
make
```

依赖：MinGW-w64, FTDI D2XX 库 (`libftd2xx.a`, `ftd2xx64.dll`)

链接使用 `-static`，因此运行时不需要额外分发 `libgcc_s_seh-1.dll` 和 `libstdc++-6.dll`；
唯一需要随 exe 一起拷贝的动态库是 `ftd2xx64.dll`。

## 运行

```bash
./build/drum_gui.exe
```

## 项目结构

```
├── drum_gui.cpp       # 主程序 (ImGui GUI + 逻辑)
├── ym7129.c/h         # YM7129 寄存器操作
├── spfm_lite.c/h      # SPFM Light 通信
├── imgui/             # ImGui 源码 (Docking branch)
├── docs/
│   ├── presets.md      # 预设音色详细参数
│   ├── bugs.md         # Bug 记录
│   └── changelog.md    # 更新日志
└── build/
    └── ym7129_config.ini  # 用户配置持久化
```
