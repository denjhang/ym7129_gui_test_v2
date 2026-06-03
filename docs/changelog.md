# Changelog

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
