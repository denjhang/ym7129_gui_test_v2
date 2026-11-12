# Changelog

## 2026-11-12 — $01 重新定义为鼓左右声道选择

依据 2026-06-23 更新版 `数据分析.xlsx`（Sheet1 第 3 行）。

### 修正
- `$01` 原标注为 "TEST 寄存器，初始化必须写 $FF"，实际是**鼓左右声道选择**：
  bit0-3 = Rhy0-3 的 L 输出，bit4-7 = Rhy0-3 的 R 输出。`$FF` 只是"四路鼓全开左右声道"。
- 头文件改为 `YM7129_REG_RHY_PAN` / `YM7129_RHY_PAN_ALL`，保留 `YM7129_REG_TEST` 作为兼容别名。
- 删除 `ym7129_drum_strike()` 里对 `$78-$7F` 的写入：`数据分析.xlsx` 中 `$78-$7F` 无任何定义
  （旧代码把它当"未文档化的鼓声道"猜着写 `0x60`，属于无效写入）。
- 鼓触发前不再整片重写通道 `$70-$77` 的 L/R，只写 `$01`。

### 新增
- `ym7129_drum_set_pan()` / `ym7129_drum_get_pan()`：按通道设置/读取 `$01`。
- Drum Pad 窗口新增 "Drum L/R output ($01)" 面板：BD / SD / C.HH / O.HH 各有 L、R 复选框，
  实时写寄存器并显示当前 `$01` 值。
- 寄存器调试面板改为 `$80-$84`（鼓类型/音量/触发），原来那个 `$70-$88` 面板里的
  "Drum Pan?" 猜测已删除。

## 2026-11-12 — 静态链接修复运行时 DLL 缺失

### 修复
- exe 启动报 "找不到 libgcc_s_seh-1.dll / libstdc++-6.dll"：链接参数改为 `-static`，
  把 MinGW(GCC 16.2) 的 C/C++ 运行时和 libwinpthread 全部静态链接进 exe。
- 现在 `build/drum_gui.exe` 只依赖系统 DLL + `ftd2xx64.dll`，直接双击可运行，
  不需要额外的运行时 DLL 或安装 MinGW。

### 注意
- 由于编译器为 GCC 16.2.0 (MSYS2)，重新编译后 exe 会使用新版运行时；如需在别处运行，
  直接用新的静态 exe，不要从旧机器拷 `libstdc++-6.dll`。

### 防复发
- `makefile` 链接后自动检查导入表：一旦发现 `libgcc_*` / `libstdc++*` /
  `libwinpthread*` / `libssp-*`，直接报错并让构建失败，不再生成跑不起来的 exe。
- `CMakeLists.txt` 在 MinGW 下同样加 `-static`，两条构建路径行为一致。
- `-static` 必须写在 `LDFLAGS` 里而不是只写 `-static-libgcc -static-libstdc++`：
  后者会漏掉 `libwinpthread-1.dll`。

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
