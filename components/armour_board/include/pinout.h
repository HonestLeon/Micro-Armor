#pragma once
#include "driver/gpio.h"

/* 引脚分配唯一出处：docs/pinout.md（网表实证）。改板只动本文件 */

/* 四边 WS2812 数据线（RMT 输出） */
#define ARMOUR_PIN_WS_F   GPIO_NUM_10
#define ARMOUR_PIN_WS_B   GPIO_NUM_5
#define ARMOUR_PIN_WS_L   GPIO_NUM_6
#define ARMOUR_PIN_WS_R   GPIO_NUM_7

/* 四边按键总线：同边按键并联对 GND，板上 5.1k 上拉至 3V3；低=按下。
 * 同边多键硬件上不可区分，只报"该边触发" */
#define ARMOUR_PIN_BTN_F  GPIO_NUM_0
#define ARMOUR_PIN_BTN_R  GPIO_NUM_1
#define ARMOUR_PIN_BTN_L  GPIO_NUM_3
#define ARMOUR_PIN_BTN_B  GPIO_NUM_4

/* GPIO9 = strapping：复位时按住进 USB 下载模式（正常用法，勿内部上拉叠加） */
#define ARMOUR_PIN_KEY0   GPIO_NUM_9

/* 每边板载 3~4 颗（子板文档存在两种变体）。当前在测子板实装 3 颗，与开源参考
 * SmallMaster 的每边 3 颗一致；帧长 3×24=72 symbol，也留出余量不顶满 96 symbol
 * 的通道内存。换 4 颗的子板时改回 4。 */
#define ARMOUR_LED_MAX    3
