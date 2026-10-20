---
name: armor-flash
description: 通过 USB-C 原生 USB (USB-Serial-JTAG) 烧录装甲板固件并打开串口监视。当需要 idf.py flash / monitor / 下载固件到 ESP32-C3 时使用。
disable-model-invocation: true
---

用于把已编译固件烧录到装甲板（ESP32-C3），走板上 USB‑C 的原生 USB（USB‑Serial‑JTAG），无需外接串口芯片、无需手动 BOOT/KEY 进下载模式。

## 前置

- 固件已构建（见 `/armor-build`），存在 `build/*.bin`、`.elf`、bootloader、partition-table。
- 板已用 USB‑C 线连接 PC 并上电。

## 步骤

1. **找 COM 口**（PowerShell，在 Git Bash 里也需清 MSYS 泄漏）：
   ```bash
   powershell.exe -NoProfile -Command "
     Get-CimInstance Win32_PnPEntity | Where-Object { \$_.Name -match 'USB-Serial-JTAG|USB Serial|COM\d' } | Select-Object Name, \$_.PNPClass | Format-Table -Auto
   "
   ```
   目标设备名含 `USB-Serial-JTAG`（或描述里有 `COM` 号）。记下 `COMx`。

2. **烧录+监视**（在已激活 PowerShell、工程根目录）：
   ```bash
   powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
     Remove-Item Env:MSYSTEM -EA SilentlyContinue; Remove-Item Env:MSYS -EA SilentlyContinue
     . 'C:\esp\v6.0.1\esp-idf\export.ps1' | Out-Null
     Set-Location '<工程绝对路径>'
     idf.py -p COMx flash monitor
   "
   ```
   - 只烧录：`idf.py -p COMx flash`；只监视：`idf.py -p COMx monitor`。
   - 退出 monitor：`Ctrl+]`。

## 排错

- 找不到端口：重插 USB；确认线是数据线；`export.ps1` 报 venv 错误见 `/armor-build` 的 venv 修复。
- 烧写报 port busy/permission：关掉占用该 COM 的程序（旧 monitor、串口助手）。
- 同步失败(sync timeout)：多为供电不足或需按住 BOOT 重插一次触发进入下载；原生 USB 一般自动 reset，优先排查线缆与电源。
- 应用走原生 USB CDC 打印：确认 `sdkconfig` 里 console 输出到 `USB-Serial-JTAG`（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），否则 monitor 看不到日志。
