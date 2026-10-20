# Micro_Armor 固件（ESP32-C3 装甲板）

ESP-IDF 工程：四边 WS2812 灯效 + 四边按键总线 + KEY0。硬件说明见 `装甲板工程说明.md`，
引脚唯一出处 `docs/pinout.md`（代码里对应 `components/armour_board/include/pinout.h`），
方案与调试记录见 `工程骨架搭建计划.md`。

## 构建 / 烧录

环境激活**只能在 PowerShell**（详见 AGENTS.md 的坑位清单）：

```powershell
cd C:\esp\v6.0.1\esp-idf
.\export.ps1
cd <本目录>
idf.py set-target esp32c3     # 首次
idf.py build
idf.py -p COM18 flash monitor # USB-C 原生 USB-Serial-JTAG，无需外接串口芯片
```

也可用项目技能 `/armor-build`、`/armor-flash`。

## 代码地图

| 位置 | 内容 |
|---|---|
| `main/main.c` | 装配 + 主循环（20ms tick 轮询按键、跑灯效：循环变色/流水/呼吸/sweep 锁边） |
| `components/armour_leds` | WS2812 RMT 驱动：C3 单 TX 通道 × 4 边轮转（`rmt_tx_switch_gpio`），亮度统一缩放 |
| `components/armour_btn` | 四边按键总线轮询（GPIO0/1/3/4，外部 5.1k 上拉，同边多键不可区分） |
| `components/armour_board` | KEY0 轮询消抖 + `pinout.h` |

## 可调项（`menuconfig` → Micro Armor 配置）

- `CONFIG_ARMOUR_BOOT_DIAG`（默认关）：开机自动"逐边锁红 → 逐边 WS tone 连续白帧"并无限循环，
  供肉眼/万用表定位灯带链路故障；关掉即直接进主循环。
- `CONFIG_ARMOUR_DIAG_TONE_HOLD`（默认关）：四边 WS 线轮流常驻真实 RMT 波形各 10s，
  万用表直流档搭 pad1 期望 ~2.2V（全 1 码 67% 占空），用于判"线通不通"。
- `CONFIG_ARMOUR_DIAG_HOLD_HIGH`（默认关）：四根数据线推挽恒高，绕开 RMT 只验通路；开启时优先于上两项。
- `CONFIG_ARMOUR_BRIGHTNESS`（默认 160）：灯带全局亮度上限，多灯量产建议 64。

## 灯带现状（2026-10-20 已点亮）

四边灯带在 3.3V 直驱、无电平转换器的条件下实测点亮。此前"灯不亮"与硬件无关：
`rmt_bytes_encoder_config_t.bit0/bit1` 漏写 `level0/level1`，驱动原样使用模板不代填电平，
数据线恒低。根因、手册 VIH/时序窗口的推导、以及 OpenOCD+GDB 的定位过程分别见
`docs/灯珠电平与时序分析.md §0`、`docs/pinout.md §5/§5.1`、`docs/调试指南_OpenOCD_GDB.md §0`。
