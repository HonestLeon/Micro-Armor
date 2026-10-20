# AGENTS.md

本文件为 AI 在此仓库工作时提供指引。

## 项目性质

`Micro_Armor` 是一块 **ESP32-C3 可编程 RGB 装甲板** 的硬件+固件工作区：
- 硬件：嘉立创EDA专业版工程 `ProPrj_装甲板_*.epro2`（本质是 ZIP 容器，可解压读取）。
- 固件：在本目录内新建 ESP-IDF 工程（`main/`、`CMakeLists.txt`、`sdkconfig`）。
- 硬件/原理图细节见 `@装甲板工程说明.md`。
- **上游开源参考（硬件负责人指定）**：`https://github.com/njupt-robomasters/SmallMaster`（南邮 Potential 战队）。其装甲板与本板同为 ESP32-C3-WROOM-02U-N4 + XL-5050RGBC-WS2812B，WS 脚同为 GPIO10/5/6/7、传感脚同为 GPIO0/1/3/4，BOM 内无电平转换器。固件在 `code/armor/src/`（`hit.cpp` 击打判定与灯控、`air.cpp` ESP-NOW 回传、`main.cpp` OneButton 按键与 NVS 存色），PCB 工程在 `裁判系统/PCB/*.epro2`。**判断"能不能点亮/该怎么驱动"时以它为实测基准**，对照结论见 `docs/灯珠电平与时序分析.md §6`。差异：他们用压电片 + ADC DMA 阈值判击打，本板已改成机械按键 + 上拉轮询（按下拉低），触发逻辑不可照抄。

注释与文档以**中文**为主；代码标识符用英文。

## ESP-IDF 环境（关键，易踩坑）

- 版本：**ESP-IDF v6.0.1**，路径 `C:\esp\v6.0.1\esp-idf`（git `8c19b156`）。
- 目标芯片：**esp32c3**（RISC-V）。
- **激活只能在 PowerShell 里做**，不要在 Git Bash / MSYS 里 `source export.sh`——会报 `MSys/Mingw is not supported`（`MSYSTEM`/`MSYS` 环境变量泄漏所致）。激活：
  ```powershell
  cd C:\esp\v6.0.1\esp-idf
  .\export.ps1
  ```
- **venv 已修复过一次**：`C:\Espressif\tools\python\v6.0.1\venv\pyvenv.cfg` 的 `home/executable` 原本指向已失效的旧 Anaconda。现指向系统 **Python 3.13.x**。若 `export.ps1` 又报 `did not find executable`：把 `pyvenv.cfg` 的 `home`/`executable` 重新指到任一可用的 3.13.x 解释器（原备份在 `pyvenv.cfg.bak`）。改后 `idf.py --version` 应输出 `ESP-IDF v6.0.1`。
- 工具链 `riscv32-esp-elf 15.2.0`、cmake、ninja、esptool 均已装并验证可产出 `.bin`/`.elf`。
- **C3 RMT 实测硬坑**：① 致命：`rmt_bytes_encoder_config_t.bit0/bit1` 是 `rmt_symbol_word_t`（位域 `duration0:15|level0:1|duration1:15|level1:1`）模板，驱动**原样**写入符号内存、不代填电平。只写 `duration0/duration1` 会让两个 level 位被指定初始化器静默置 0 → 数据线恒低 → 灯永不亮，而 `rmt_transmit` 和 TX-done 中断全都"成功"。本工程踩过这个坑，排查记录见 `docs/灯珠电平与时序分析.md §0`。② RMT 无 DMA、每通道 48 symbol——`mem_block_symbols=96` 会独占两通道内存、全芯片只剩 1 个 TX（当前用 48，3 灯一帧 72 symbol 由驱动半内存 ISR 回填，实测无丢帧）。③ 默认 tick=100Hz 下 `pdMS_TO_TICKS(<10ms)` 整除为 0，`vTaskDelay(0)` 忙等会饿死 IDLE 触发 task_wdt；同样地，片内 GPIO 回读探针单次采样别超过几百次。
- **灯珠时序以开源实机参考为准，不以 XL-5050RGBC-WS2812B 手册窗口为准**：手册（`docs/E1004CAA80592E31D08BFE019083806F.pdf`）P5 给 VIH=0.7×VDD、P10 给 TOH 200~350ns，按字面算 3.3V 驱不动 4.8V 轨；但同型号硬件的开源实机（见下）无电平转换器能点亮，且其 0 码用 400ns（超手册上限）。故本工程取 **0=400/800ns、1=800/400ns、周期 1.2µs、TRST≥80µs**，本板已用该组数值实测点亮（3 灯边、无电平转换器）。数值合同与推导见 `docs/灯珠电平与时序分析.md` + `docs/pinout.md §5.1`。

## 构建 / 烧录 / 调试

在已激活的 PowerShell 中，于工程根目录：
```powershell
idf.py set-target esp32c3      # 首次配置
idf.py build                   # 编译
idf.py -p COMx flash monitor   # 烧录+监视
```
- **烧录用 USB‑C 原生 USB（USB‑Serial‑JTAG）**：板把 ESP32‑C3 的 `D+/D-` 直接接到 Type‑C，无需外接 CH340/CP210x。插入后设备枚举成一个 CDC 串口（Windows 显示 `USB-Serial-JTAG`），`-p` 指向该 COM 口即可。
- 若端口占用/下载失败：关闭占用该 COM 的程序（旧 monitor/串口助手），或拔插 USB 重连（重插后 COM 号可能变化）；无需 BOOT/KEY 手动进下载模式（原生 USB 支持自动 reset）。
- 备用 UART 路径为板上 `TXD/RXD`，仅在原生 USB 不可用时考虑。

## 硬件 → 外设映射（写驱动时对照）

| 功能 | 网络/引脚 | ESP-IDF 组件 |
|---|---|---|
| 四边 WS2812B 灯带 | `WS_F/B/L/R` = **GPIO10/5/6/7** | `esp_driver_rmt`（**C3 仅 2 个 TX 通道**，4 条线需轮转/广播，见 `docs/pinout.md`） |
| 四边按键总线 | `ADC_F/R/L/B` = **GPIO0/1/3/4**（5.1k 上拉，按键对 GND） | 按数字输入 `esp_driver_gpio` 轮询；这些脚亦在 ADC1 上可采样。**同边多键不可区分** |
| 下载/调试串口 | `TXD/RXD` = GPIO21/20（排针 H1/CN1） | `esp_driver_uart` |
| 按键/控制 | KEY0=GPIO9(BOOT)；空闲：GPIO2、GPIO8（均上拉，strapping） | `esp_driver_gpio` |
| 无线控制 | — | `esp_wifi`（2.4GHz） |
| 电源 | USB‑C→5V→LDO(ME6118A33)→3V3 | C3 3.3V 供电 |

引脚硬约束：WROOM‑02U 仅引出 GPIO0–10、18–21，本工程已用满（余 GPIO2/8 空闲）。**具体分配一律以 `docs/pinout.md` 为准，勿凭网络名猜测功能**（如 `ADC_x` 实为按键总线）；骨架方案见 `工程骨架搭建计划.md`。

## 约定

- 新增/修改固件用 ESP-IDF 推荐风格；`.c/.h` 遵循 ESP-IDF 命名与错误处理（`ESP_ERROR_CHECK`、`esp_err_t` 返回值）。
- 不在仓库内提交 `build/`、`sdkconfig` 生成产物（应保持 `.gitignore` 覆盖 ESP-IDF 构建目录）。
