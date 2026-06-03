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
