#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "armour_board.h"

esp_err_t armour_btn_init(void);

/* 主循环每拍调用：读四边总线并消抖 */
void armour_btn_poll(void);

/* 消抖后的持续状态：true=该边按键仍被按住（同边多键不可区分） */
bool armour_btn_pressed(armour_side_t side);

/* 取走一次"某边按下"事件（消费式，一按一次；同边多键不可区分） */
bool armour_btn_take_event(armour_side_t side);

/* 诊断：当前四线原始状态位图，bit=1 表示该线被拉低 */
uint8_t armour_btn_raw_mask(void);
