# Changelog

## 2026-11-12 — 静态链接修复运行时 DLL 缺失

### 修复
- exe 启动报 "找不到 libgcc_s_seh-1.dll / libstdc++-6.dll"：链接参数改为 `-static`，
  把 MinGW(GCC 16.2) 的 C/C++ 运行时和 libwinpthread 全部静态链接进 exe。
- 现在 `build/drum_gui.exe` 只依赖系统 DLL + `ftd2xx64.dll`，直接双击可运行，
  不需要额外的运行时 DLL 或安装 MinGW。

### 注意
- 由于编译器为 GCC 16.2.0 (MSYS2)，重新编译后 exe 会使用新版运行时；如需在别处运行，
  直接用新的静态 exe，不要从旧机器拷 `libstdc++-6.dll`。

## 2026-06-04 — FM 钢琴键盘支持

### 新增
- FM 钢琴键盘窗口（独立 docking 窗口 "Piano"）
- 主键盘字母键映射（参考 DMPlayer YM2163）:
  - Z-M 行: C3-E4 白键 + 逗号/句号/斜杠 扩展
  - S/D/G/H/J/K/L 行: 黑键
  - Q-U 行: C4-G5 白键
  - I/O/P + [/]: 扩展白键
  - 数字行 2/3/5/6/7/8/9/0: 黑键
- PageUp/PageDown 切换八度 (范围 0-5)
- 可视化钢琴键盘: 白键 20×100px, 黑键 12×60px, 按下高亮
- Connect 时自动加载默认 FM patch 到所有 8 通道
- WM_KEYUP 处理钢琴键释放 (ym7129_stop_note)

## 2026-06-04 — 鼓声触发修复 + 键盘输入修复

### 修复
- 修复鼓声触发错误（点击一个鼓出现别的声音）:
  - DRUM_PADS 全部改用 channel=0，不再分散到 4 个 slot
  - 每次 strike 前调用 ym7129_init 重新初始化芯片（与 ym7129_drum_all.c 一致）
  - $84 trigger 前先 mute（$84=0x00），防止累积 state 污染
  - 时序对齐数据流实测值 ~1700us（原 1500us+3000us 不准确）
- 修复鼓声 panning 偏右: 每次触发前 init 芯片确保寄存器状态正确
- 修复数字键无效: 去掉 NavEnableKeyboard（避免 ImGui 消费按键事件）
- 键盘输入改用 Win32 WM_KEYDOWN + VK 键码（与 DMPlayer 一致），不再依赖 ImGui::IsKeyPressed

### 待修复
- 小键盘按键（Num*, Num-, Num/, Num+）仍无反应

## 2026-06-03 — 初始版本

### 新增
- YM7129 Drum Pad GUI 测试程序 (ImGui + DX11)
- 12 个鼓垫按钮 (6 种鼓 x L/H): BD, SD, C.HH, O.HH, Tom, Ride
- 鼠标点击触发 + 键盘快捷键 (0-5, 7, 8, Num*, Num-, Num/, Num+)
- Connect / Disconnect / Reset 按钮
- 延时滑块 0-200us
- 音量控制 [ / ] 键 (0-15)
- 击打动画 (红色闪烁衰减)
- 3 字节 SPFM Light raw 协议: `[slot, 0x80, addr] [slot, 0x81, data]`

### 修复
- 修复启动 segfault: 同步 imgui include 路径、启用 Docking/Viewports 标志、添加 PlatformWindows 渲染、清理旧 .o 缓存
- 对齐 DMPlayer v16 的 DX11/DPI/Docking/HiDPI 字体配置
