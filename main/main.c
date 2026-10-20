#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "armour_board.h"
#include "armour_btn.h"
#include "armour_leds.h"
#include "pinout.h"

static const char *TAG = "armor";

/* 长按判定与亮度步进同周期：按住满 0.5s 进预览，此后每满 0.5s 步进一档 */
#define KEY_LONG_MS   500
#define BRIGHT_INIT   CONFIG_ARMOUR_BRIGHTNESS   /* menuconfig 给初值，量纲 % */
#define BRIGHT_MIN    10
#define BRIGHT_MAX    60   /* USB 500mA 预算核定值，非灯珠能力上限；与 Kconfig 的 range 1 60 同处一改
                              推导见 按键点灯功能改造计划.md §5.1 */
#define BRIGHT_STEP   10
#define POLL_GAP_MS   20
#define HEARTBEAT_MS  5000

static const armour_rgb_t s_pal[] = {
    { 255,   0,   0 },
    {   0, 255,   0 },
    {   0,   0, 255 },
    { 255, 255, 255 },
};
#define PAL_N (sizeof(s_pal) / sizeof(s_pal[0]))

static const char *const s_pal_name[PAL_N] = { "R", "G", "B", "WHITE" };

static const char *const s_side_name[ARMOUR_SIDE_NUM] = {
    [ARMOUR_SIDE_F] = "F", [ARMOUR_SIDE_B] = "B",
    [ARMOUR_SIDE_L] = "L", [ARMOUR_SIDE_R] = "R",
};
static const int s_btn_pin[ARMOUR_SIDE_NUM] = {
    [ARMOUR_SIDE_F] = ARMOUR_PIN_BTN_F, [ARMOUR_SIDE_B] = ARMOUR_PIN_BTN_B,
    [ARMOUR_SIDE_L] = ARMOUR_PIN_BTN_L, [ARMOUR_SIDE_R] = ARMOUR_PIN_BTN_R,
};

static uint8_t  s_pct       = BRIGHT_INIT;
static uint8_t  s_color;              /* 初值 0 = 红 */
static uint32_t s_long_cnt;           /* 已完成的长按次数，决定下一次步进方向 */
static uint32_t s_press_cnt;          /* 四边侧键累计按压次数 */

void app_main(void)
{
    ESP_ERROR_CHECK(armour_board_init());
    ESP_ERROR_CHECK(armour_btn_init());

#if CONFIG_ARMOUR_DIAG_HOLD_HIGH
    /* 万用表档：四根数据线恒拉高，绕开 RMT 与主循环，只验"脚有没有驱动起来 + 线通不通" */
    static const int hold_pin[ARMOUR_SIDE_NUM] = {
        ARMOUR_PIN_WS_F, ARMOUR_PIN_WS_B, ARMOUR_PIN_WS_L, ARMOUR_PIN_WS_R,
    };
    for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << hold_pin[s],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_drive_capability(hold_pin[s], GPIO_DRIVE_CAP_3);
        gpio_set_level(hold_pin[s], 1);
    }
    ESP_LOGW(TAG, "HOLD-HIGH: WS F/B/L/R = GPIO%d/%d/%d/%d 恒高，任一根对 GND 应读 ~3.3V",
             hold_pin[0], hold_pin[1], hold_pin[2], hold_pin[3]);
    ESP_LOGW(TAG, "HOLD-HIGH: 逐段量 模组脚 -> 插座pad1 -> 首珠DIN，三段同压即通路正常");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    ESP_ERROR_CHECK(armour_leds_init());
    armour_leds_set_brightness(s_pct);
    ESP_LOGI(TAG, "Micro_Armor running: 侧键按住亮该边 / KEY0 短按选色 长按调亮度 (pct=%u%%)", s_pct);

#if CONFIG_ARMOUR_DIAG_TONE_HOLD
    /* 万用表档：四条 WS 线轮流常驻真实 RMT 波形（单通道一次只能占一根）。
     * 表笔搭在被测插座 pad1 上不动，一轮 4×10s：
     *   某一窗口跳到 ~2.2V → RMT 正常，且该 pad1 属于那一号 GPIO
     *   四个窗口全程 0V   → RMT 没驱动任何脚，问题在驱动侧 */
    static const int th_pin[ARMOUR_SIDE_NUM] = {
        ARMOUR_PIN_WS_F, ARMOUR_PIN_WS_B, ARMOUR_PIN_WS_L, ARMOUR_PIN_WS_R,
    };
    for (int round = 1;; round++) {
        for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
            ESP_LOGW(TAG, "TONE-HOLD round %d: side %d = GPIO%d，10s（PROBE 行=片内自读焊盘电平）",
                     round, s, th_pin[s]);
            if (armour_leds_diag_tone_start((armour_side_t)s) != ESP_OK) {
                ESP_LOGE(TAG, "tone start failed side %d", s);
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            armour_leds_diag_tone_stop();
        }
    }
#endif

#if CONFIG_ARMOUR_BOOT_DIAG
    /* 开机诊断：逐边锁最亮红 3s（核对该边灯带接线与插座↔边映射），再逐边连续白帧供万用表量。
     * 主循环里的 sweep 模式已随灯效一起删除，本档是它的替代品。 */
    static const int tone_pin[ARMOUR_SIDE_NUM] = {
        ARMOUR_PIN_WS_F, ARMOUR_PIN_WS_B, ARMOUR_PIN_WS_L, ARMOUR_PIN_WS_R,
    };
    for (int round = 1;; round++) {
        ESP_LOGI(TAG, "=== DIAG round %d ===", round);
        for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
            ESP_LOGI(TAG, "boot-sweep: LOCK side %d red 3s", s);
            armour_leds_test_single(s, (armour_rgb_t){ 255, 0, 0 });
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        armour_leds_test_single(ARMOUR_SIDE_NUM, (armour_rgb_t){ 0, 0, 0 });

        for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
            ESP_LOGI(TAG, "DIAG-TONE side %d (GPIO%d WS): DMM DC-V on WS line expect ~2.2V for 8s",
                     s, tone_pin[s]);
            if (armour_leds_diag_tone_start(s) != ESP_OK) {
                ESP_LOGE(TAG, "tone start failed side %d", s);
            }
            vTaskDelay(pdMS_TO_TICKS(8000));
            armour_leds_diag_tone_stop();
        }
    }
#endif

    /* 缓存"上次下发的颜色 + 当时的亮度"。亮度是 write 时预乘进像素缓冲的，
     * 所以只有把 pct 也放进比较键，亮度变化才会触发重填（否则预览步进看不见） */
    armour_rgb_t last_rgb[ARMOUR_SIDE_NUM];
    uint8_t      last_pct[ARMOUR_SIDE_NUM];
    for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
        last_rgb[s] = (armour_rgb_t){ 0, 0, 0 };
        last_pct[s] = s_pct;
        armour_leds_fill(s, last_rgb[s]);   /* 上电先刷全黑，冲掉灯珠随机上电态 */
    }

    bool     key_was_down = false;
    bool     in_preview   = false;
    uint32_t step_applied = 0;
    int8_t   dir          = 1;
    int      tick         = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_GAP_MS));

        armour_board_poll();
        armour_btn_poll();

        for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
            if (armour_btn_take_event(s)) {
                ESP_LOGI(TAG, "press #%u side %d (%s/GPIO%d)",
                         ++s_press_cnt, s, s_side_name[s], s_btn_pin[s]);
            }
        }

        bool     down  = armour_board_key0_is_down();
        uint32_t held  = armour_board_key0_held_ms();

        if (down != key_was_down) {
            if (down) {
                step_applied = 0;
                dir = (s_long_cnt % 2 == 0) ? 1 : -1;   /* 接下来这次是第奇数次长按 → 增亮 */
            } else if (in_preview) {
                s_long_cnt++;
                ESP_LOGI(TAG, "KEY0 long #%u done: pct=%u%%", s_long_cnt, s_pct);
            } else {
                s_color = (uint8_t)((s_color + 1) % PAL_N);
                ESP_LOGI(TAG, "KEY0 short: color=%s", s_pal_name[s_color]);
            }
            key_was_down = down;
        }

        in_preview = down && held >= KEY_LONG_MS;
        if (in_preview) {
            uint32_t n = (held - KEY_LONG_MS) / KEY_LONG_MS;   /* 用绝对时刻算档位，晚一拍不漏档 */
            while (step_applied < n) {
                step_applied++;
                int p = (int)s_pct + dir * BRIGHT_STEP;
                if (p < BRIGHT_MIN) {
                    p = BRIGHT_MIN;
                }
                if (p > BRIGHT_MAX) {
                    p = BRIGHT_MAX;
                }
                if ((uint8_t)p != s_pct) {
                    s_pct = (uint8_t)p;
                    armour_leds_set_brightness(s_pct);
                }
            }
        }

        for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
            armour_rgb_t tgt;
            if (in_preview) {
                tgt = (armour_rgb_t){ 255, 255, 255 };   /* 预览期四边全白，压过侧键颜色 */
            } else if (armour_btn_pressed(s)) {
                tgt = s_pal[s_color];
            } else {
                tgt = (armour_rgb_t){ 0, 0, 0 };
            }
            if (tgt.r != last_rgb[s].r || tgt.g != last_rgb[s].g || tgt.b != last_rgb[s].b
                || s_pct != last_pct[s]) {
                armour_leds_fill(s, tgt);
                last_rgb[s] = tgt;
                last_pct[s] = s_pct;
            }
        }

        if (++tick % (HEARTBEAT_MS / POLL_GAP_MS) == 0) {
            ESP_LOGI(TAG, "heartbeat (%ds) cnt=%u color=%s pct=%u%% btn_low_mask=0x%x",
                     (int)(tick * POLL_GAP_MS / 1000), s_press_cnt, s_pal_name[s_color],
                     s_pct, armour_btn_raw_mask());
        }
    }
}
