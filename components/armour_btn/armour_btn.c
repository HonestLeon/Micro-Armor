#include "armour_btn.h"
#include "pinout.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define BTN_DEBOUNCE_MS 30

static const gpio_num_t s_pins[ARMOUR_SIDE_NUM] = {
    [ARMOUR_SIDE_F] = ARMOUR_PIN_BTN_F,
    [ARMOUR_SIDE_B] = ARMOUR_PIN_BTN_B,
    [ARMOUR_SIDE_L] = ARMOUR_PIN_BTN_L,
    [ARMOUR_SIDE_R] = ARMOUR_PIN_BTN_R,
};

typedef struct {
    bool     stable;    /* 消抖后的持续状态：true=按住 */
    bool     raw;       /* 上次采到的原始状态 */
    uint64_t change_ms; /* raw 上次跳变时刻 */
    bool     pending;   /* 本次按压未取走的事件 */
} btn_state_t;

static btn_state_t s_state[ARMOUR_SIDE_NUM];

esp_err_t armour_btn_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < ARMOUR_SIDE_NUM; i++) {
        mask |= 1ULL << s_pins[i];
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE, /* 板上每线已有 5.1k 上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

void armour_btn_poll(void)
{
    uint64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < ARMOUR_SIDE_NUM; i++) {
        btn_state_t *st = &s_state[i];
        bool low = (gpio_get_level(s_pins[i]) == 0);
        if (low != st->raw) {                 /* 跳变：只重新起算稳定窗 */
            st->raw = low;
            st->change_ms = now;
        } else if (low != st->stable && now - st->change_ms >= BTN_DEBOUNCE_MS) {
            st->stable = low;                 /* 电平稳住，才更新持续状态 */
            if (low) {
                st->pending = true;           /* 按下沿发一次事件，须松手后才会再来 */
            }
        }
    }
}

bool armour_btn_pressed(armour_side_t side)
{
    if (side >= ARMOUR_SIDE_NUM) {
        return false;
    }
    return s_state[side].stable;
}

uint8_t armour_btn_raw_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < ARMOUR_SIDE_NUM; i++) {
        if (gpio_get_level(s_pins[i]) == 0) {
            m |= (1U << i);
        }
    }
    return m;
}

bool armour_btn_take_event(armour_side_t side)
{
    if (side >= ARMOUR_SIDE_NUM) {
        return false;
    }
    bool ev       = s_state[side].pending;
    s_state[side].pending = false;
    return ev;
}
