# Bug 记录

## 1. 启动时 Segfault (exit code 139)

**日期**: 2026-06-03
**文件**: `drum_gui.cpp`
**现象**: 程序启动后立即崩溃，退出码 139 (SIGSEGV)

**根本原因** (多重叠加):

1. **ImGui include 路径错误**: 使用 `#include "imgui.h"` 而非 `#include "imgui/imgui.h"`，导致链接了系统路径或其他版本的 imgui 头文件，与实际编译的 imgui 源码版本不一致。`ImGuiConfigFlags_DockingEnable` 等符号在旧版本中不存在。

2. **缺少 `#include "imgui/imgui_internal.h"`**: DockBuilder API (`DockBuilderRemoveNode`, `DockBuilderAddNode`, `DockBuilderDockWindow`, `DockBuilderFinish`, `DockBuilderGetNode`) 都在 `imgui_internal.h` 中声明，缺少此 include 会导致链接错误或运行时调用了未初始化的函数指针。

3. **缺少 `ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable`**: 没有启用 Docking 和 Viewports 配置标志，但代码中使用了 `ImGui::DockSpace()`、`SetNextWindowViewport()` 等 API。这些 API 在未启用对应 flag 时内部行为未定义，导致空指针访问。

4. **缺少 Viewport 平台窗口渲染**: 启用 `ViewportsEnable` 后，必须在每帧渲染时调用 `ImGui::UpdatePlatformWindows()` 和 `ImGui::RenderPlatformWindowsDefault()`。缺少这两行会导致 viewport 相关的内部状态不一致。

5. **旧的 .o 文件缓存**: imgui 源码从 ym2163-test (v1.92.5, 无 Docking) 切换到 DMPlayer v16 版本（有 Docking/Viewports），但 build 目录中的 .o 文件未清理，导致新旧版本的目标文件混链，符号表不一致。

6. **`AntiAliasedLines = true` (默认) 与 DMPlayer 不一致**: DMPlayer 使用 `AntiAliasedLines = false`，旧版 imgui 默认为 true，渲染管线差异可能导致 DX11 资源创建失败。

7. **字体 `OversampleH = 2` 与 DMPlayer 不一致**: DMPlayer 使用 `OversampleH = 1`，过高的 oversample 在高 DPI 下会生成过大的字体贴图。

**修复方法**:
- include 路径改为 `imgui/imgui.h`、`imgui/imgui_internal.h`
- 添加 `io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable`
- 添加 `ImGui::UpdatePlatformWindows()` / `ImGui::RenderPlatformWindowsDefault()`
- 设置 `style.AntiAliasedLines = false`
- 字体 `OversampleH = 1`
- 清理所有 .o 文件后全量重编译

**教训**: 从参考项目复制 imgui 源码时，必须同步更新 include 路径和配置标志，且必须清理旧的目标文件缓存。不要混用不同版本的 imgui 头文件和源码。

## 2. 鼓声触发错误（点击一个鼓出现别的声音）

**日期**: 2026-06-04
**文件**: `drum_gui.cpp`, `ym7129.c`
**现象**: 点击鼓垫触发错误的鼓声，且鼓声 panning 偏右

**根本原因**:

1. **DRUM_PADS channel 分配错误**: 12 种鼓分配到 4 个 $80-$83 slot，Tom(channel=0) 和 BD(channel=0) 共用 slot0，Ride(channel=1) 和 SD(channel=1) 共用 slot1。不同鼓的参数互相覆盖，`drum_trigger_state` 累积导致触发错误的鼓声。

2. **缺少 chip 重新初始化**: `ym7129_drum_all.c`（已验证正常）每次 strike 前都调用 `ym7129_init` 重新初始化芯片。GUI 程序只在 connect 时 init 一次，导致 FM 通道 L/R 寄存器状态不对，鼓声 panning 偏右。

3. **$84 trigger state 累积**: 原代码只 clear/set 当前 bit，其他 bit 保持。数据流分析表明触发前应先 mute（$84=0x00）。

4. **时序不准确**: 原代码 sleep 1500us + 3000us，数据流实测每步 ~1730us。

**修复方法**:
- DRUM_PADS 全部 channel=0，只使用 slot0（与 ym7129_drum_all.c 一致）
- 每次 strike 前调用 `ym7129_init` 重新初始化芯片
- `ym7129_drum_strike` 先写 $84=0x00 mute，再写参数，再 clear→set 上升沿
- 时序改为 ~1700us（对齐数据流）

**教训**: 应该首先参照已验证的程序（ym7129_drum_all.c）的行为，而不是自己猜测时序和寄存器操作顺序。

## 3. 数字键无效

**日期**: 2026-06-04
**文件**: `drum_gui.cpp`
**现象**: 按数字键 0-5, 7, 8 无鼓声响应

**根本原因**: `ImGuiConfigFlags_NavEnableKeyboard` 让 ImGui 内部消费数字键用于窗口导航，`ImGui::IsKeyPressed` 返回 false。

**修复方法**:
- 去掉 `ImGuiConfigFlags_NavEnableKeyboard`
- 键盘输入从 `ImGui::IsKeyPressed` 改为 Win32 `WM_KEYDOWN` + VK 键码（与 DMPlayer 一致）
- `WM_KEYDOWN` 处理放在 `ImGui_ImplWin32_WndProcHandler` 之前

## 4. 小键盘按键无反应（待修复）

**日期**: 2026-06-04
**文件**: `drum_gui.cpp`
**现象**: Num*, Num-, Num/, Num+ 按键无反应

**状态**: 可能是 NumLock 状态影响 VK_MULTIPLY/VK_SUBTRACT 等映射，待进一步调试。
