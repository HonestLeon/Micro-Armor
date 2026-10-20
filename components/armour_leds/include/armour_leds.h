#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "armour_board.h"
#include "pinout.h"

esp_err_t armour_leds_init(void);

/* 写 n 像素到某边（n <= ARMOUR_LED_MAX） */
esp_err_t armour_leds_write(armour_side_t side, const armour_rgb_t *px, size_t n);

/* 某边整条填充同一颜色 */
esp_err_t armour_leds_fill(armour_side_t side, armour_rgb_t color);

/* 设亮度百分比（0~100）。缩放预乘发生在 write 时，本函数不重算已写入的像素，
 * 也不自己重画——调用后须重新 fill 需要变亮的边 */
void armour_leds_set_brightness(uint8_t percent);

/* 诊断：停轮转调度，只把指定边常亮 color（传 side>=ARMOUR_SIDE_NUM 恢复轮转） */
void armour_leds_test_single(armour_side_t side, armour_rgb_t color);

/* 诊断 tone：该边连续重发纯白帧（loop_count=-1），全 1 码 67% 占空，
 * 万用表直流档量 WS 线应读到 ≈2.2V，可判"数据到底有没有发出来"；
 * 帧间无 TRST≥80µs 低电平，灯珠不锁存，此模式看不到点亮 */
esp_err_t armour_leds_diag_tone_start(armour_side_t side);
void armour_leds_diag_tone_stop(void);
