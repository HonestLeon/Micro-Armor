---
name: armor-build
description: 编译本装甲板 ESP-IDF 工程（目标 esp32c3）。当需要构建固件、set-target、或验证代码能否编译时使用。处理 PowerShell-only 激活、MSYS 泄漏与 venv 修复等已知坑。
---

用于构建本目录下的 ESP32-C3 装甲板固件。

## 前置

- 若本目录还没有 ESP-IDF 工程（无 `CMakeLists.txt` / `main/`），先建立最小骨架再构建，不要凭空 `idf.py build`。
- ESP-IDF 位于 `C:\esp\v6.0.1\esp-idf`，目标 `esp32c3`。

## 关键坑（务必遵守）

1. **只能在 PowerShell 激活**，不要在 Git Bash `source export.sh`（报 `MSys/Mingw is not supported`）。当从本会话（Git Bash）触发时，用 `powershell.exe` 包一层，并清除泄漏变量：
   ```bash
   powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
     Remove-Item Env:MSYSTEM -EA SilentlyContinue
     Remove-Item Env:MSYS -EA SilentlyContinue
     . 'C:\esp\v6.0.1\esp-idf\export.ps1' | Out-Null
     Set-Location '<工程绝对路径>'
     idf.py set-target esp32c3
     idf.py build
   "
   ```
2. **venv 损坏识别**：若激活报 `did not find executable ... anaconda` 或 `exit 103`，说明 `C:\Espressif\tools\python\v6.0.1\venv\pyvenv.cfg` 的 base 解释器失效。修法：把 `home`/`executable`/`command` 重新指向一个可用的 **Python 3.13.x**（ABI 必须 cp313）。原备份见同目录 `pyvenv.cfg.bak`。修好后先跑 `idf.py --version` 确认输出 `ESP-IDF v6.0.1` 再构建。

## 步骤

1. 确认在已激活的 PowerShell 环境（`idf.py --version` 通过）。
2. 首次或切换芯片：`idf.py set-target esp32c3`。
3. 构建：`idf.py build`。
4. 报告产物：`build/<项目名>.bin`、`.elf`，以及 flash 所需偏移。

## 排错

- 只链接失败且报 `undefined reference to app_main`：多为 `main/` 缺 `CMakeLists.txt`（`idf_component_register(SRCS ... INCLUDE_DIRS .)`），或需 `idf.py fullclean` 重建（配置缓存会漏收源码）。
- 想看规模/分区：`idf.py size`、`idf.py partition-table`。
