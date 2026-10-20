#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ARMOUR_SIDE_F = 0,
    ARMOUR_SIDE_B,
    ARMOUR_SIDE_L,
    ARMOUR_SIDE_R,
    ARMOUR_SIDE_NUM,
} armour_side_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} armour_rgb_t;

esp_err_t armour_board_init(void);

/* 主循环每拍调用：读 KEY0 并做消抖 */
void armour_board_poll(void);

/* 消抖后 KEY0 是否仍被按住 */
bool armour_board_key0_is_down(void);

/* 已按住多久（ms）；未按下返回 0 */
uint32_t armour_board_key0_held_ms(void);
