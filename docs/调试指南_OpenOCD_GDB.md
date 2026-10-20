# 用 OpenOCD + GDB 查装甲板（C3 内置 USB-JTAG）

面向当前这个具体问题：RMT 事务报告成功、焊盘却一动不动。要查的三件事是——RMT 数据 RAM 里到底写了什么、实际分到第几号通道、焊盘的输出通路有没有真的交给 RMT 信号。这三件事都能在不停机、不接示波器的情况下读出来。

文中所有 IDF 路径基于 `C:\esp\v6.0.1\esp-idf`（v6.0.1），芯片 ESP32-C3，工程 `Micro_Armor`。

## 0. 实战：本次查出来的结果

按 §6 读通道 0 的符号内存 `0x60016400`，47 个已填词全是 `0x00040008`，即上表第三行——**level 位是 0，发出来永远是低**。病根在 `armour_leds_init()` 的编码器配置：`bit0/bit1` 只写了 `duration0/duration1`，`level0/level1` 被 C 的指定初始化器静默置 0，而驱动是把这两个模板原样搬进内存的。补成

```c
.bit0 = { .duration0 = 4, .level0 = 1, .duration1 = 8, .level1 = 0 },
.bit1 = { .duration0 = 8, .level0 = 1, .duration1 = 4, .level1 = 0 },
```

后灯立刻亮，片内 GPIO 回读探针同时从 `hi=0 edges=0` 变成 `hi=220 edges=125`。

顺序上浪费时间的地方，下次别再重复：先怀疑 `mem_block_symbols=96`、时序宽度、电平门限，全是外围猜测；`x/48wx` 读一次数据 RAM 十几秒就能定死。判据只有一条——**符号字里的电平位**。事务返回 ESP_OK、TX-done 中断正常来，都不能证明线上有波形。

## 1. 一根线两件事，先搞清冲突关系

板子走的是 C3 的原生 USB（USB-Serial-JTAG），它同时提供两个通道：CDC 串口（就是 COM18，控制台和烧录都走它）和 JTAG 调试通道（OpenOCD 走它）。两者共用同一个 USB 设备，所以：

- 烧录（esptool）和 OpenOCD 不能同时用。要烧录前先停 OpenOCD。
- OpenOCD 挂上后 COM 口有时会被占掉、monitor 打不开。遇到就把 monitor 关掉，日志改从 GDB/OpenOCD 那边看；或者反过来，需要 monitor 时停掉 OpenOCD。
- 调试本身不需要 BOOT/KEY，也不用进下载模式。

## 2. 启动 OpenOCD

### 方式 A：VSCode 插件

先在 `.vscode/settings.json` 里把目标与调试配置固定下来（插件是按这份配置拉起 OpenOCD 的）：

```json
{
  "idf.adapterTargetName": "esp32c3",
  "idf.port": "COM18",
  "idf.openOcdConfigs": [
    "interface/esp_usb_jtag.cfg",
    "target/esp32c3.cfg"
  ]
}
```

`interface/esp_usb_jtag.cfg` 这一行是关键：不写它就会去找外部 JTAG 适配器（FTDI/LambdaStage），而我们没有。

然后命令面板执行 `ESP-IDF: Launch OpenOCD Server`（或点状态栏那个 OpenOCD 图标）。看到输出里出现 `Listening on port 3333 for gdb connections` 就算起来了。再执行一次同一条命令即可停止。

### 方式 B：终端

```powershell
idf.py -c openocdopenocd .
# 或者直接指定配置，最清楚：
openocd -f interface/esp_usb_jtag.cfg -f target/esp32c3.cfg
```

注意 `idf.py openocd` 需要 `OPENOCD_HOME`/PATH 里有 openocd（idf_tools.py export 会带上）。如果插件装过 OpenOCD 工具，命令行里直接 `openocd` 就能找到。

### 三个端口

| 端口 | 用途 |
|---|---|
| 3333 | GDB 远程目标（`target remote`） |
| 4444 | OpenOCD telnet 控制台，`mdw`/`reg`/`halt` 这类命令，不需要 ELF，最省事 |
| 6666 | TCL，脚本化用，暂时用不上 |

telnet 控制台在 Windows 上用 `telnet localhost 4444`（需要启用"Telnet 客户端"功能），或者用 `plink`/PuTTY。嫌麻烦就全程用 GDB。

## 3. 挂 GDB

### 方式 A：VSCode 按 F5

命令面板执行 `ESP-IDF: Add Debugger Configuration to .vscode`，它会生成一份 `launch.json`（`"type": "espidf"`, `"mode": "auto"`）。之后 OpenOCD 起着、板子插着，直接 F5 就能停到 `app_main`。想让它 attach 到正在跑的固件而不是从头停，把 `"stopAtEntry"` 设为 `false`，进去后按暂停（或点 Debug 工具栏的 Pause）。

### 方式 B：命令行（更快，推荐排查时用这个）

```powershell
C:\esp\v6.0.1\esp-idf\tools\idf.py --version > $null   # 确保 PATH 里有工具链，或直接下面用绝对路径
riscv32-esp-elf-gdb build\micro_armor.elf -ex "target remote localhost:3333"
```

进去后先 `monitor halt`（或 GDB 里直接 Ctrl-C）把 CPU 停住再读东西。读寄存器/内存这类操作 CPU 停不停都行，读变量建议停住，免得值在变。

完事 `monitor resume` 让它继续跑，`monitor shutdown` 关 OpenOCD。

## 4. 本次要用到的地址与常量（都从 IDF v6.0.1 的 C3 头文件里核过）

| 名称 | 值 | 出处 |
|---|---|---|
| RMT 外设基址 `DR_REG_RMT_BASE` | `0x60016000` | `soc/esp32c3/register/soc/reg_base.h` |
| RMT 数据 RAM，通道 0 | `0x60016400` | 基址 + `RMT_CH0DATA_REG` 偏移 0x400 |
| 每通道数据 RAM 大小 | 48 word = `0xC0` 字节 | `SOC_RMT_MEM_WORDS_PER_CHANNEL = 48` |
| 通道 1 / 2 / 3 数据 RAM | `0x600164C0` / `0x60016580` / `0x60016640` | 上一条递推 |
| GPIO 基址 | `0x60004000` | `reg_base.h` |
| IO_MUX 基址 | `0x60009000` | `reg_base.h`（注意不是 0x60008000） |
| `RMT_SIG_OUT0_IDX` / `OUT1` | **51 / 52** | `soc/esp32c3/include/soc/gpio_sig_map.h` |
| `SIG_GPIO_OUT_IDX` | **128**（= 由 GPIO 输出寄存器驱动，不接外设） | 同上 |
| C3 只有 ch0/ch1 有 `tx_sig` | ch2/ch3 的 `tx_sig = -1` | `esp_hal_rmt/esp32c3/rmt_periph.c` |

RMT 符号（`rmt_symbol_word_t`）位域是 `duration0:15 | level0:1 | duration1:15 | level1:1`。按本工程当前时序（10MHz，1 tick=100ns）：

| 码元 | duration0/level0/duration1/level1 | 32 位字应等于 |
|---|---|---|
| 1 码（800/400ns） | 8 / 1 / 4 / 0 | **`0x00048008`** |
| 0 码（400/800ns） | 4 / 1 / 8 / 0 | **`0x00088004`** |
| 若 level 位是 0 | 8 / 0 / 4 / 0 | `0x00040008`（发出来永远是低） |

tone 档发的是全 0xFF 像素，所以数据 RAM 里应当清一色 `0x00048008`。

## 5. 检查③：实际分到第几号通道（两秒钟，先做这个）

`s_ch[0]` 是 `rmt_channel_handle_t`，指向 `rmt_tx_channel_t`，而它的第一个成员就是基类 `rmt_channel_t`。该结构前四个字段依次是 `channel_id`、`gpio_num`、`channel_mask`、`mem_block_num`，所以不用管类型名，直接按字读：

```
(gdb) x/4wx s_ch[0]
0x3fcxxxxx: 0x00000000 0x0000000a 0x00000003 0x00000001
   channel_id      gpio_num     mem_mask    mem_block_num
```

判读：

- `channel_id` 只可能是 **0 或 1**。如果读到 **2 或 3**，病根当场找到：C3 的 ch2/ch3 没有 `tx_sig`，`esp_rom_gpio_connect_out_signal(pin, -1, ...)` 会把一个不存在的信号接到焊盘上，外设照样空转、照样发中断、焊盘永远不动。
- `gpio_num` 应当等于当前 tone 那一边的引脚号（第一轮是 `0x0a` = GPIO10）。
- `mem_block_num` 现在是 1（我们把 `WS_MEM_SYMBOLS` 改回 48 了）。之前是 2，两种情况现象一样，所以这一项不是病根，但顺手确认一下。

如果 GDB 认得类型，也可以直接：

```
(gdb) p *(rmt_channel_t *)s_ch[0]
```

## 6. 检查①：RMT 数据 RAM 里写了什么

先看驱动记的基址，再 dump：

```
(gdb) p ((rmt_channel_t *)s_ch[0])->hw_mem_base
$1 = (rmt_symbol_word_t *) 0x60016400 <sys_ctl+1024>
(gdb) x/16wx 0x60016400
```

或者不经过 GDB，直接在 OpenOCD telnet 里：

```
mdw 0x60016400 16
```

判读：

| 看到的内容 | 含义 | 下一步 |
|---|---|---|
| 一片 `0x00048008` | 编码器写对了、硬件确实在发"高 8 tick / 低 4 tick" | 病在焊盘通路，做检查② |
| `0x00040008`、`0x00080004` 这类 level 位为 0 的字 | 编码路径有问题，发出来的就是全低电平 | 查 `rmt_new_bytes_encoder` 的配置与 `msb_first` 写法 |
| 全 0 | 帧根本没进内存 | 查 `rmt_transmit` 的 payload 与 `mem_block_num` 是否够装 72 symbol |
| 有 `0x00048008` 但只有前 8 个 word | 帧被截断 | 同上，内存块数与帧长不匹配 |

注意 tone 档是背靠背重发同一帧，所以随时 dump 到的都应该是同一批内容；CPU 停不停都不影响这段内存（硬件只在发送时读它）。

## 7. 检查②：焊盘的输出通路有没有真的交给 RMT

C3 的 GPIO 矩阵里，每个引脚的输出源选择寄存器字段名是 `out_sel`（8 位）、`inv_sel`、`oen_sel`、`oen_inv_sel`。GDB 里直接读：

```
(gdb) p GPIO.func_out_sel_cfg[10]
$2 = {val = 131, {out_sel = 131 '\x83', inv_sel = 0, oen_sel = 0, oen_inv_sel = 0}}
(gdb) p/x GPIO.func_out_sel_cfg[10].val
(gdb) p GPIO.enable.data
(gdb) p GPIO.pin[10].val
```

判读（把 `10` 换成你在查的引脚号）：

| 字段 | 正常值 | 异常值与含义 |
|---|---|---|
| `func_out_sel_cfg[10].out_sel` | **51 或 52**（RMT_SIG_OUT0/1） | **128** = 焊盘被 GPIO 输出寄存器接管，RMT 信号没接上（`gpio_config`/`park_pin_low` 事后把它抢走了）；**0 或 255** = 接了个无效信号，等于没接 |
| `func_out_sel_cfg[10].oen_sel` | 0（输出使能由 `GPIO.enable` 位控制） | 1 = 让外设自己的 OEN 信号控制，而 RMT 没有 OEN 输出，焊盘可能一直是禁止状态 |
| `GPIO.enable.data` 的第 10 位 | **1** | 0 = 输出没使能，焊盘高阻/不驱动，这正是"外设忙、脚不动" |
| `GPIO.pin[10].pad_driver` | 0 | 1 = 强制驱动模式，会盖过矩阵的 OE 逻辑 |

`GPIO.func_out_sel_cfg` 的绝对地址不用手算，让 GDB 告诉你，再拿去喂 OpenOCD：

```
(gdb) p &GPIO.func_out_sel_cfg[10]
$3 = (union (anonymous) *) 0x60004550
```

想要人肉可读版，让目标自己打印（`gpio_dump_io_configuration` 会把 IO_MUX 与矩阵两侧字段全打出来，需要一个可写的 FILE*）：

```
(gdb) call (int)gpio_dump_io_configuration(stdout, 1ULL << 10)
```

如果 GDB 报 `stdout` 未定义，就退回上面逐字段 `p` 的读法——信息量是一样的。

顺带把四个引脚一次看完：

```
(gdb) p GPIO.func_out_sel_cfg[5]
(gdb) p GPIO.func_out_sel_cfg[6]
(gdb) p GPIO.func_out_sel_cfg[7]
(gdb) p GPIO.enable.data
```

## 8. 顺手能确认的几件事

```
(gdb) info threads                      # 确认 leds_tone 真的在跑
(gdb) thread apply all bt               # 它停在 rmt_transmit / vTaskDelay 的哪里
(gdb) p s_tone
(gdb) p s_lock
(gdb) p s_ch_pin[0]
(gdb) p s_edge[0]                       # n、dirty、pin
(gdb) x/9bx &s_edge[0].grb[0][0]        # tone 期间应当全是 0xff
```

`s_edge[0].grb` 若不是全 `0xff`，说明 tone 的 `memset` 被后面的正常刷新覆盖了（`armour_leds_write` 会按亮度缩放重写这块缓冲），那 dump 出来的 RMT RAM 内容就不是全 1 码，判读要相应调整。

## 9. 判读决策树

```
检查③ channel_id ∈ {2,3} ?          （实测 = 0，本板不是这条）
  ├─ 是 → 病根＝拿到了没有 tx_sig 的 RX 通道。改法：显式限定候选通道，或把 mem_block_symbols
  │        配到能落在 ch0/ch1 上的值；轮转方案本来只需要 1 个 TX 通道。
  └─ 否 → 检查① RMT RAM 有 0x00048008 ?
            ├─ 全是 0x00040008 → 病根＝encoder 的 level 位没写（bit0/bit1 被驱动原样使用，
            │        不代填电平）。这是本工程实际命中的分支，修法见 §0。
            ├─ 全 0 / 字数不对 → 病根＝装载。查 payload 长度、mem_block_num 与帧长(72) 的关系。
            └─ 有 → 检查② out_sel 是 51/52 且 enable 位是 1 ?
                      ├─ out_sel=128 → 病根＝焊盘被 GPIO 抢走。查 rmt_new_tx_channel 之后
                      │                谁又调了 gpio_config/park_pin_low 在这个脚上。
                      ├─ enable 位=0 → 病根＝输出没使能。补 esp_rom_gpio_connect_out_signal
                      │                或查 rmt_tx_switch_gpio 的 gpio_output_disable 副作用。
                      └─ 全部正常 → 外设与路由都对，那就只剩物理层：量该脚对地有没有波形/电压，
                                    或把该脚用杜邦线跳到另一个空闲脚做 RMT RX 自收自发。
```

## 10. 几个会咬人的坑

- **OpenOCD 挂着的时候不能烧录**，`idf.py flash` 会报端口占用。改完代码要先停 OpenOCD。
- **`call` 是真的在目标上执行函数**，会改变状态。`gpio_dump_io_configuration` 只读寄存器，安全；别随手 `call` 驱动函数。
- **halt 不会停 RMT 硬件**。CPU 停下后 RMT 仍在把内存里的符号发出去，所以 `mdw` 读数据 RAM 是"实时内容"，而 `x/` 读变量是快照。
- **tone 档会持续刷日志**（每 20 轮一行，约 10 行/秒）。嫌吵就把 `armour_leds.c` 里 `rounds % 20` 的分母调大，别调采样次数——`PROBE_SAMPLES 500` 是刻意压小的，采样循环太长会饿死 IDLE 触发 task_wdt（这个坑已经踩过一次）。
- **诊断档已全部关闭**（2026-10-20 问题解决后）：`sdkconfig` 与 `sdkconfig.defaults` 均为 `BOOT_DIAG`/`DIAG_HOLD_HIGH`/`DIAG_TONE_HOLD` 关、`BRIGHTNESS` 回 160。要再排查灯的问题，用 `idf.py menuconfig` 打开 `Armour → Boot diagnostics` 或 `Diag tone hold` 重烧即可；两个档位都在 `components/armour_board/Kconfig` 里，代码路径没删。

## 11. 与代码的对应位置

| 要看的东西 | 源码位置 |
|---|---|
| 通道创建参数（`mem_block_symbols`、`trans_queue_depth`、`resolution_hz`） | `components/armour_leds/armour_leds.c` `armour_leds_init()` |
| 换脚轮转 `rmt_disable → rmt_tx_switch_gpio → rmt_enable → park_pin_low` | 同上 `remux_channel()` |
| 空闲脚钉低（会把 `out_sel` 改回 128） | 同上 `park_pin_low()` |
| tone 任务与片内回读探针 | 同上 `tone_task()` / `probe_pad()` / `probe_selftest()` |
| 引脚宏 | `components/armour_board/include/pinout.h` |
| 三个诊断档 | `components/armour_board/Kconfig` |
