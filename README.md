# YM7129 Drum Pad GUI

Win32 GDI 鼓垫测试工具，使用已验证的 SPFM 3字节 raw 协议。

## 功能

- 12 个鼓垫：BD / SD / C.HH / O.HH / Tom / Ride 各 x2 (L/H)
- 鼠标点击触发
- 数字小键盘快捷键：Num7/8(BD), Num4/5(SD), Num1/2(C.HH), Num0/.(O.HH), *-/ (Tom), /+(Ride)
- `[` `]` 调节音量 (0=最响, 15=最轻)
- 连接状态显示

## 协议

SPFM 3字节 raw 模式：`[slot, 0x80, addr] [slot, 0x81, data]`
- 0x80 = A0=0 地址锁存
- 0x81 = A0=1 数据写入

## 编译

```bash
make
```

需要：FTDI D2XX 库 (libftd2xx.a, ftd2xx64.dll)

## 运行

```bash
./drum_gui.exe
```
