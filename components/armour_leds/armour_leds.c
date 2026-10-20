#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "armour_leds.h"

static const char *TAG = "leds";

/* 时序基准 = 同型号硬件上实测能亮的参考实现，不是灯珠手册的窗口。
 * 参考 NJUPT-robomasters/SmallMaster：同为 ESP32-C3-WROOM-02U-N4 + XL-5050RGBC-WS2812B，
 * WS 脚同为 GPIO10/5/6/7，BOM 内无电平转换器；其底层 Adafruit_NeoPixel 的 IDF>=5 RMT
 * 路径（esp.c）在 10MHz 下取 1 码 8/4 tick、0 码 4/8 tick。手册 P10 的 TOH 窗口是
 * 200~350ns，参考实现用 400ns 照样点亮，说明该窗口不是实际判据。
 * 故本工程照抄参考实现：0=400/800ns，1=800/400ns，周期 1.2µs。
 * 本板已用该组数值实测点亮（3 灯边、无电平转换器）。 */
#define WS_CLK_RES_HZ     10000000
/* 亮度是运行期变量（KEY0 长按调），初值取 menuconfig 的百分比。
 * 这条断言是给"量纲刚从 0~255 改成 0~100"上的锁：旧 sdkconfig 里残留的 160
 * 若被静默截成 uint8_t 会算出 408&0xFF=152（≈60%），宁可编译失败也不要猜 */
_Static_assert(CONFIG_ARMOUR_BRIGHTNESS >= 1 && CONFIG_ARMOUR_BRIGHTNESS <= 100,
               "CONFIG_ARMOUR_BRIGHTNESS 的量纲是百分比 1~100");
#define BRIGHTNESS_MAX_PCT  CONFIG_ARMOUR_BRIGHTNESS
static uint8_t  s_scale = BRIGHTNESS_MAX_PCT * 255 / 100;  /* 0~255 的像素乘数 */
#define REFRESH_GAP_MS    6           /* 名义 6ms，100Hz tick 下钳成 10ms → 4 边整轮约 40ms */
/* C3 每通道 48 symbol、无 DMA。这里取 48 图的是内存不跨通道圈占；3 灯一帧 72 symbol
 * 超出部分由驱动的半内存搬运（wrap ISR）补齐，实测无丢帧。
 * 副作用：loop_count>0 的硬件循环模式要求整帧装进内存，48 symbol 下不可用。
 * 注：早先"申请 96 时焊盘不动"的现象，事后查明是 encoder 漏写 level 位所致，
 * 与 96/48 无关——若需要硬件循环模式，可以重新试 96。 */
#define WS_MEM_SYMBOLS    48
#define N_PHYS_CH         1

typedef struct {
    gpio_num_t pin;
    uint8_t    grb[ARMOUR_LED_MAX][3];
    uint8_t    n;
    volatile bool dirty;
} edge_t;

static edge_t s_edge[ARMOUR_SIDE_NUM] = {
    [ARMOUR_SIDE_F] = { .pin = ARMOUR_PIN_WS_F },
    [ARMOUR_SIDE_B] = { .pin = ARMOUR_PIN_WS_B },
    [ARMOUR_SIDE_L] = { .pin = ARMOUR_PIN_WS_L },
    [ARMOUR_SIDE_R] = { .pin = ARMOUR_PIN_WS_R },
};

static rmt_channel_handle_t   s_ch[N_PHYS_CH];
static rmt_encoder_handle_t   s_enc;
static gpio_num_t             s_ch_pin[N_PHYS_CH];   /* 通道当前占用的引脚 */
static int                    s_rr;                  /* 轮转公平游标 */
static volatile int           s_lock = -1;           /* >=0：诊断模式，只刷该边 */
static volatile bool          s_tone;                /* tone 期间调度任务挂起 */

static void park_pin_low(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);   /* 空闲期数据线保持低 = WS2812 reset，防浮空噪声 */
}

static esp_err_t remux_channel(int ci, gpio_num_t pin)
{
    if (s_ch_pin[ci] == pin) {
        return ESP_OK;
    }
    gpio_num_t old = s_ch_pin[ci];
    ESP_RETURN_ON_ERROR(rmt_disable(s_ch[ci]), TAG, "disable ch%d", ci);
    ESP_RETURN_ON_ERROR(rmt_tx_switch_gpio(s_ch[ci], pin, false), TAG, "switch ch%d", ci);
    ESP_RETURN_ON_ERROR(rmt_enable(s_ch[ci]), TAG, "enable ch%d", ci);
    if (old >= 0) {
        park_pin_low(old);
    }
    s_ch_pin[ci] = pin;
    return ESP_OK;
}

static esp_err_t flush_edge(armour_side_t side, int ci)
{
    edge_t *e = &s_edge[side];
    esp_err_t err = remux_channel(ci, e->pin);
    if (err != ESP_OK) {
        return err;
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    err = rmt_transmit(s_ch[ci], s_enc, e->grb, (size_t)e->n * 3, &tc);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ch[ci], 500);
    }
    if (err == ESP_OK) {
        e->dirty = false;
    }
    return err;
}

static void refresh_task(void *arg)
{
    /* 默认 tick=100Hz：pdMS_TO_TICKS(<10ms) 会整除为 0 → 必须钳到 ≥1 tick，否则忙等饿死 IDLE */
    TickType_t gap = pdMS_TO_TICKS(REFRESH_GAP_MS);
    if (gap < 1) {
        gap = 1;
    }
    for (;;) {
        vTaskDelay(gap);
        if (s_tone) {
            continue;
        }
        if (s_lock >= 0) {
            if (s_edge[s_lock].dirty) {
                flush_edge(s_lock, 0);
            }
            continue;
        }
        int budget = N_PHYS_CH;
        for (int i = 0; i < ARMOUR_SIDE_NUM && budget > 0; i++) {
            int s = (s_rr + i) % ARMOUR_SIDE_NUM;
            if (s_edge[s].dirty) {
                esp_err_t err = flush_edge(s, N_PHYS_CH - budget);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "flush side %d: %s", s, esp_err_to_name(err));
                }
                budget--;
            }
        }
        s_rr = (s_rr + N_PHYS_CH) % ARMOUR_SIDE_NUM;
    }
}

esp_err_t armour_leds_init(void)
{
    /* level 位必须显式写：bit0/bit1 是原样塞进 symbol 的 rmt_symbol_word_t 模板，
     * 驱动不会代填电平。漏写 = 两个电平位都为 0 = 数据线恒低，永不点亮 */
    rmt_bytes_encoder_config_t be_cfg = {
        .bit0 = { .duration0 = 4, .level0 = 1, .duration1 = 8, .level1 = 0 },   /* 400ns hi / 800ns lo */
        .bit1 = { .duration0 = 8, .level0 = 1, .duration1 = 4, .level1 = 0 },   /* 800ns hi / 400ns lo */
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&be_cfg, &s_enc), TAG, "enc create");

    for (int ci = 0; ci < N_PHYS_CH; ci++) {
        rmt_tx_channel_config_t ch_cfg = {
            .gpio_num          = s_edge[0].pin,      /* 初始挂 F，轮转时切换 */
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = WS_CLK_RES_HZ,
            .mem_block_symbols = WS_MEM_SYMBOLS,
            .trans_queue_depth = 1,
        };
        ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&ch_cfg, &s_ch[ci]), TAG, "ch%d create", ci);
        ESP_RETURN_ON_ERROR(rmt_enable(s_ch[ci]), TAG, "ch%d enable", ci);
        s_ch_pin[ci] = ch_cfg.gpio_num;
    }
    /* 未被通道占用的数据线先钉低；四条线一律拉到最高驱动档，让 400ns 窄沿尽量陡。
     * 注：VDD 由灯珠自 5V 轨取电、数据线仍是 3.3V 摆幅，但同型号无转换器的参考板
     * 实测能亮（本板修好 level 位后也亮），故不做电平抬高。 */
    for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
        if (s >= N_PHYS_CH) {
            park_pin_low(s_edge[s].pin);
        }
        gpio_set_drive_capability(s_edge[s].pin, GPIO_DRIVE_CAP_3);
        s_edge[s].n = ARMOUR_LED_MAX;
    }

    if (xTaskCreate(refresh_task, "leds_rfs", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS2812 ready: %d TX ch × %d edges rotation", N_PHYS_CH, ARMOUR_SIDE_NUM);
    return ESP_OK;
}

esp_err_t armour_leds_write(armour_side_t side, const armour_rgb_t *px, size_t n)
{
    if (side >= ARMOUR_SIDE_NUM || n > ARMOUR_LED_MAX || (px == NULL && n > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    edge_t *e = &s_edge[side];
    for (size_t i = 0; i < n; i++) {
        e->grb[i][0] = (uint8_t)((px[i].g * s_scale) >> 8);   /* GRB 序 */
        e->grb[i][1] = (uint8_t)((px[i].r * s_scale) >> 8);
        e->grb[i][2] = (uint8_t)((px[i].b * s_scale) >> 8);
    }
    e->n     = (uint8_t)n;
    e->dirty = true;
    return ESP_OK;
}

esp_err_t armour_leds_fill(armour_side_t side, armour_rgb_t color)
{
    if (side >= ARMOUR_SIDE_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    armour_rgb_t buf[ARMOUR_LED_MAX];
    for (int i = 0; i < ARMOUR_LED_MAX; i++) {
        buf[i] = color;
    }
    return armour_leds_write(side, buf, ARMOUR_LED_MAX);
}

/* 缩放是在 armour_leds_write() 里预乘进像素缓冲的，flush 只回放存量字节，
 * 所以本函数只影响之后的写入——改完亮度必须重新 write/fill 才会看到效果。
 * 不标 dirty：标了也只是把旧亮度的字节再发一遍。 */
void armour_leds_set_brightness(uint8_t percent)
{
    s_scale = (uint8_t)((uint32_t)percent * 255 / 100);
}

void armour_leds_test_single(armour_side_t side, armour_rgb_t color)
{
    if (side < ARMOUR_SIDE_NUM) {
        s_lock = side;
        armour_leds_fill(side, color);
    } else {
        s_lock = -1;
    }
}

/* 片内回读探针：把该脚输入通路打开（gpio_input_enable 只置 fun_ie，不动输出路由），
 * 再连采 ~2 万次。焊盘真在翻转 → edges 远大于 0；焊盘被钉住 → edges=0。
 * 用来区分"RMT 没送到焊盘"和"焊盘在动但万用表读不出"，不依赖任何外部仪器。 */
#define PROBE_SAMPLES 500   /* ≈120µs，刚好盖住一帧 86µs，且不至于饿死 IDLE */

static void probe_pad(gpio_num_t pin, uint32_t *hi, uint32_t *lo, uint32_t *edges)
{
    *hi = *lo = *edges = 0;
    uint32_t prev = (uint32_t)gpio_get_level(pin);
    for (uint32_t i = 0; i < PROBE_SAMPLES; i++) {
        uint32_t now = (uint32_t)gpio_get_level(pin);
        if (now) {
            (*hi)++;
        } else {
            (*lo)++;
        }
        if (now != prev) {
            (*edges)++;
            prev = now;
        }
    }
}

/* 探针自检：在空闲脚 GPIO2（未被 RMT 占用，不会撞引脚预留）上输出高电平再回读。
 * 读到 PROBE_SAMPLES 说明 gpio_input_enable + gpio_get_level 这条回读通路本身可信，
 * 于是"WS 脚 edges=0"就只能解释为焊盘真的没动。 */
#define SELFTEST_PIN GPIO_NUM_2

static uint32_t probe_selftest(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SELFTEST_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return 0;
    }
    gpio_input_enable(SELFTEST_PIN);
    gpio_set_level(SELFTEST_PIN, 1);
    uint32_t hi, lo, edges;
    probe_pad(SELFTEST_PIN, &hi, &lo, &edges);
    gpio_set_level(SELFTEST_PIN, 0);
    return hi;
}

/* tone = 背靠背单发同一帧（loop_count=0）。硬件循环模式要求整帧装进通道内存，
 * 48 symbol 下 72 symbol 的帧放不下，所以只能靠反复单发。
 * 采样安排在 rmt_transmit 返回之后、wait_all_done 之前——此刻硬件正在发，
 * 回读才有意义（上一版在发完之后才采样，edges=0 是无效读数）。
 * 每轮 vTaskDelay(1) 让出 CPU，否则采样循环会饿死 IDLE 触发 task_wdt。
 * 全 255 不经亮度缩放 → 比特流全 1 → 高电平占空 67%。 */
#define TONE_REPEAT 0

static void tone_task(void *arg)
{
    int side = (int)(intptr_t)arg;
    edge_t *e = &s_edge[side];

    uint32_t rounds = 0, hi, lo, edges;
    uint32_t self_hi = 0;
    while (s_tone) {
        if (rounds == 0) {
            gpio_input_enable(e->pin);   /* 只开输入通路，不动输出路由 */
            self_hi = probe_selftest();
        }
        esp_err_t err = remux_channel(0, e->pin);
        if (err == ESP_OK) {
            rmt_transmit_config_t tc = { .loop_count = TONE_REPEAT };
            err = rmt_transmit(s_ch[0], s_enc, e->grb, (size_t)e->n * 3, &tc);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone start side %d: %s", side, esp_err_to_name(err));
            break;
        }
        probe_pad(e->pin, &hi, &lo, &edges);   /* 此刻硬件仍在发，采样才有意义 */
        err = rmt_tx_wait_all_done(s_ch[0], 500);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone wait side %d: %s", side, esp_err_to_name(err));
            break;
        }
        if (++rounds % 20 == 0) {
            ESP_LOGI(TAG, "PROBE GPIO%d rounds=%u self=%u/%u hi=%u edges=%u -> %s",
                     (int)e->pin, rounds, self_hi, (unsigned)PROBE_SAMPLES, hi, edges,
                     edges > 5 ? "焊盘在翻转" : (self_hi < PROBE_SAMPLES / 2 ? "探针不可信"
                                              : "焊盘恒低(没送出)"));
        }
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

esp_err_t armour_leds_diag_tone_start(armour_side_t side)
{
    if (side >= ARMOUR_SIDE_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    edge_t *e = &s_edge[side];
    memset(e->grb, 255, sizeof(e->grb));
    e->n     = ARMOUR_LED_MAX;
    e->dirty = true;
    s_lock   = side;
    s_tone   = true;
    if (xTaskCreate(tone_task, "leds_tone", 3072, (void *)(intptr_t)side, 6, NULL) != pdPASS) {
        s_tone = false;
        s_lock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void armour_leds_diag_tone_stop(void)
{
    if (!s_tone) {
        return;
    }
    s_tone = false;
    vTaskDelay(pdMS_TO_TICKS(20));   /* 等 tone 任务退出，避免与 rmt_disable 抢通道 */
    s_lock = -1;
    rmt_disable(s_ch[0]);
    rmt_enable(s_ch[0]);
    for (int s = 0; s < ARMOUR_SIDE_NUM; s++) {
        s_edge[s].dirty = true;   /* 恢复调度后立刻重建视觉状态 */
    }
}
