# Micro_Armor（ESP32-C3 可编程 RGB 装甲板）

一块主板 + 四块灯带子板的组合：主板上的 ESP32-C3 用 RMT 驱四边 WS2812B 灯带，
用输入轮询读四边子板上的机械侧键，主板自己的 BOOT 键（KEY0）负责选颜色与调亮度。
当前固件已完成"按住点亮、松手熄灭、短按换色、长按调亮度、按压计数"这套交互，实机验证通过。

- 硬件与原理图要点：`装甲板工程说明.md`
- 引脚唯一出处：`docs/pinout.md`（代码里对应 `components/armour_board/include/pinout.h`）
- 灯珠时序/电平的分析与结论：`docs/灯珠电平与时序分析.md`
- 调试方法（OpenOCD + GDB 读寄存器）：`docs/调试指南_OpenOCD_GDB.md`
- 本次交互功能的设计与验收：`按键点灯功能改造计划.md`

## 硬件

| 项 | 内容 |
|---|---|
| 模组 | ESP32-C3-WROOM-02U-N4（RISC-V，4MB flash），USB-C 原生 USB 直接烧录调试 |
| 灯珠 | 每边子板 3 颗 XL-5050RGBC-WS2812B 手拉手，VDD 取自 5V 轨（实测 4.8V），**无电平转换器** |
| 侧键 | 每边 3~4 只机械键轴并到一根总线，板上 5.1kΩ 上拉至 3V3，按下拉低；同边多键硬件上不可区分 |
| 电源 | USB-C → 5V → ME6118A33 LDO → 3V3 |
| 主控板按键 | KEY0 = GPIO9（BOOT strapping，按住复位可进 USB 下载模式） |

引脚分配（`docs/pinout.md §2` 的预算表，WROOM-02U 只有 15 个引出脚，已全部用满，余 GPIO2/8 两个 strapping 预留）：

| 功能 | GPIO |
|---|---|
| 灯带数据 F / B / L / R | 10 / 5 / 6 / 7 |
| 按键总线 F / R / L / B | 0 / 1 / 3 / 4（亦在 ADC1 上，可改采样） |
| 调试串口 TXD / RXD | 21 / 20（排针 H1/CN1） |
| KEY0 | 9 |

插座↔边的实测映射（真机验证，`docs/pinout.md §3.1`）：装甲板1=F/side 0，装甲板2=L/side 2，装甲板3=B/side 1，装甲板4=R/side 3（推定，针脚歪斜待复测）。

## 当前功能

1. **平时四边全灭**；上电先刷一帧全黑，冲掉灯珠的随机上电态。
2. **侧键按住即亮该边**，颜色与亮度取当前设置；松手立刻熄灭。只点亮被按的那一边。
3. **KEY0 短按**换色：红 → 绿 → 蓝 → 白 → 红。
4. **KEY0 长按**（按住满 0.5s）调亮度：判定成立的瞬间四边全白、按当前亮度点亮做预览，
   此后每满 0.5s 步进 10%；第 1、3、5… 次长按 +10%，第 2、4、6… 次 −10%，限幅 10%~60%。
   上限 60% 由 USB 500mA 预算核定（每颗全白 3×12mA=36mA，四边同白 12 颗已≈430mA），不是灯珠能力上限。
5. **按压计数**：四边侧键每稳定按下一次计 1 次（按住不放不重复计），串口逐条打印累计值，
   5s 一次心跳行汇总 `cnt / color / pct / btn_low_mask`。掉电清零。

## 构建 / 烧录

环境激活**只能在 PowerShell**（在 Git Bash 里 source 会报 `MSys/Mingw is not supported`，详见 `AGENTS.md`）：

```powershell
cd C:\esp\v6.0.1\esp-idf
.\export.ps1
cd <本目录>
idf.py set-target esp32c3     # 首次
idf.py build
idf.py -p COM18 flash monitor # USB-C 原生 USB-Serial-JTAG，无需外接串口芯片
```

也可用项目技能 `/armor-build`、`/armor-flash`。工具链：ESP-IDF v6.0.1 + riscv32-esp-elf 15.2.0。

## 代码地图

| 位置 | 内容 |
|---|---|
| `main/main.c` | 装配 + 20ms 轮询主循环：读消抖后的按键持续状态 → 算目标颜色 → 只对"颜色或亮度变了的边"下发；KEY0 短按/长按状态机；按压计数与日志 |
| `components/armour_leds` | WS2812 RMT 驱动。单 TX 通道（ch0，48 symbol）经 `rmt_tx_switch_gpio` 换脚轮转四边；后台 `refresh_task` 每拍刷一边；亮度在写入时预乘 |
| `components/armour_btn` | 四边按键总线轮询，三段式消抖，对外给"按下事件"与"是否仍按住" |
| `components/armour_board` | KEY0 消抖 + 按住时长；`pinout.h` 引脚定义；`armour_rgb_t`/`armour_side_t` 类型 |

数据流：

```
GPIO 电平 ──armour_btn_poll/armour_board_poll(30ms 消抖)──► stable 按下状态 + held_ms
                                                                 │
   main 每 20ms：目标颜色 = 预览?白 : 按住?当前色 : 黑 ───────────┘
                              │  （与上次下发的"颜色+亮度"比较，变了才写）
                     armour_leds_fill(side, rgb)  ← 乘 s_scale
                              │  标 dirty
                  refresh_task（每拍 1 边）→ rmt_transmit → WS2812
```

## 关键实现点（都踩过或验过，改动前请先读）

- **`rmt_bytes_encoder_config_t.bit0/bit1` 必须显式写 `level0/level1`**。它们是 `rmt_symbol_word_t`
  模板、被驱动原样搬进符号内存，只写 `duration` 会让两个电平位被指定初始化器静默置 0 →
  数据线恒低 → 灯永不亮，而 `rmt_transmit` 与 TX-done 中断全都"成功"。排查记录 `docs/灯珠电平与时序分析.md §0`。
- **时序取同型号开源实机值，不取手册窗口**：0=400/800ns、1=800/400ns、周期 1.2µs、TRST≥80µs
  （`docs/pinout.md §5.1`）。手册的 VIH=0.7VDD 推出的"3.3V 驱不动 4.8V 轨"被参考板与本机实测共同否证。
- **C3 每通道 48 symbol、无 DMA**；帧长 3×24=72 symbol 由驱动半内存 ISR 回填，实测无丢帧。
- **刷新整轮约 40ms 不是 6ms**：`REFRESH_GAP_MS=6` 在默认 100Hz tick 下 `pdMS_TO_TICKS(6)` 整除为 0，
  被钳到 1 tick=10ms，每趟只刷一边。同理主循环里任何 `vTaskDelay(0)` 都会忙等饿死 IDLE 触发 task_wdt。
- **亮度是写入时预乘的**：`armour_leds_set_brightness()` 只影响之后的 write，改完必须重新 fill 才会看到效果；
  所以 main 的比较缓存键是"颜色 + 亮度百分比"两元组，缺一个就会得出"预览步进无效"的错误结论。
- **按键消抖用 `raw/change_ms/stable` 三段式**，`stable` 才是可以拿来做"按住/松开"判断的量；
  直接返回原始电平会在抖动窗内把同一只手按读出多次。
- **时间基准用 `esp_timer_get_time()` 而非 FreeRTOS tick**，长按的 0.5s 档位用 `(held_ms-500)/500` 算绝对档，
  某拍被 RMT 阻塞晚了也不漏档、不多档。

## 可调项（`menuconfig` → Micro Armor 配置）

- `CONFIG_ARMOUR_BRIGHTNESS`（默认 50，`range 1 60`）：**亮度初值，单位百分比**。上限 60 是电流预算值，
  放宽前要先按每颗全白 36mA 重算（`按键点灯功能改造计划.md §5.1`）。量纲曾是 0~255，`sdkconfig` 里的旧值要一起改，
  代码里有 `_Static_assert` 拦这件事。
- `CONFIG_ARMOUR_BOOT_DIAG`（默认关）：逐边锁最亮红 3s（核对某边灯带接线与插座↔边映射）→ 逐边连续白帧 8s，无限循环。
  主循环里的 sweep 模式已随旧灯效删除，本档是它的替代品。
- `CONFIG_ARMOUR_DIAG_TONE_HOLD`（默认关）：四边 WS 线轮流常驻真实 RMT 波形各 10s，
  万用表直流档搭 pad1 期望 ~2.2V（全 1 码 67% 占空），判"线通不通"。
- `CONFIG_ARMOUR_DIAG_HOLD_HIGH`（默认关）：四根数据线推挽恒高，绕开 RMT 只验通路；优先级最高。

## 仓库约定

`build/`、`sdkconfig`、`sdkconfig.old`、`managed_components/`、`dependencies.lock`、`__pycache__/`、
`.qoder/settings.local.json`（机器专属）均已忽略，不要提交生成物。

## 后续

与服务器的通信（击打/状态回传）尚未实现，原因与分工见 `校内赛装甲板工作记录.md`。
