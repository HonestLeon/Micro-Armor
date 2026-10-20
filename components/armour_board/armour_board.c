#include "armour_board.h"
#include "pinout.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define KEY_DEBOUNCE_MS 30

static bool     s_key_stable;     /* 消抖后的持续状态：true=按住 */
static bool     s_key_raw;
static uint64_t s_key_change_ms;  /* raw 上次跳变时刻 */
static uint64_t s_key_down_ms;    /* stable 变 true 的时刻 */

esp_err_t armour_board_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ARMOUR_PIN_KEY0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE, /* 板上 R12 已上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

void armour_board_poll(void)
{
    uint64_t now  = esp_timer_get_time() / 1000;
    bool     down = (gpio_get_level(ARMOUR_PIN_KEY0) == 0);

    if (down != s_key_raw) {
        s_key_raw = down;
        s_key_change_ms = now;
    } else if (down != s_key_stable && now - s_key_change_ms >= KEY_DEBOUNCE_MS) {
        s_key_stable = down;
        if (down) {
            s_key_down_ms = now;
        }
    }
}

bool armour_board_key0_is_down(void)
{
    return s_key_stable;
}

uint32_t armour_board_key0_held_ms(void)
{
    if (!s_key_stable) {
        return 0;
    }
    return (uint32_t)(esp_timer_get_time() / 1000 - s_key_down_ms);
}
