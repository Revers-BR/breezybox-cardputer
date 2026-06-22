#include "cardputer_keyboard.h"
#include "cardputer_keymap.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "cardputer_kbd";

/* =========================
 * MATRIX CONFIG
 * ========================= */

static const gpio_num_t s_select_pins[3] = {
    GPIO_NUM_8,
    GPIO_NUM_9,
    GPIO_NUM_11
};

static const gpio_num_t s_input_pins[7] = {
    GPIO_NUM_13,
    GPIO_NUM_15,
    GPIO_NUM_3,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
    GPIO_NUM_7,
};

/* =========================
 * STATE (ONLY MASKS)
 * ========================= */

static cardputer_keyboard_char_cb_t s_char_cb;
static cardputer_keyboard_key_cb_t s_key_cb;

static uint64_t s_prev_mask;

/* =========================
 * GPIO
 * ========================= */

static void select_line(uint8_t sel)
{
    for (int i = 0; i < 3; i++) {
        gpio_set_level(s_select_pins[i], (sel >> i) & 1);
    }
    esp_rom_delay_us(3);
}

/* =========================
 * OUTPUT
 * ========================= */

static void emit_char_safe(char c)
{
    if (s_char_cb) {
        s_char_cb(c);
    }
}

/* =========================
 * KEY PROCESS
 * ========================= */

static void handle_event(cardputer_keycoord_t coord, bool pressed)
{
    if (s_key_cb) {
        cardputer_keyboard_key_event_t ev = {
            .base = 0,
            .shifted = 0,
            .fn = false,
            .shift = false,
            .ctrl = false,
            .alt = false,
            .opt = false,
            .pressed = pressed,
            .row = coord.row,
            .col = coord.column,
        };
        s_key_cb(&ev);
    }

    /* only emit on press */
    if (!pressed) {
        return;
    }

    uint8_t ascii;

    if (cardputer_keymap_ascii_for_press(s_prev_mask, coord, &ascii)) {
        emit_char_safe((char)ascii);
    }
}

/* =========================
 * SCAN MATRIX (CORRECT MODEL)
 * ========================= */

static void scan_matrix(void)
{
    uint64_t current = 0;

    for (uint8_t sel = 0; sel < 8; sel++) {

        select_line(sel);

        for (uint8_t input = 0; input < 7; input++) {

            if (gpio_get_level(s_input_pins[input]) != 0) {
                continue;
            }

            cardputer_keycoord_t coord;

            if (!cardputer_keymap_decode_original(sel, input, &coord)) {
                continue;
            }

            current |= cardputer_keymap_mask_for_coord(coord);
        }
    }

    /* =========================
     * PRESS EVENTS
     * ========================= */
    uint64_t pressed = current & ~s_prev_mask;

    for (uint8_t i = 0; i < CARDPUTER_KEYMAP_KEYS; i++) {

        if (!(pressed & (1ULL << i))) {
            continue;
        }

        cardputer_keycoord_t coord;

        if (cardputer_keymap_coord_from_index(i, &coord)) {
            handle_event(coord, true);
        }
    }

    /* =========================
     * RELEASE EVENTS
     * ========================= */
    uint64_t released = s_prev_mask & ~current;

    for (uint8_t i = 0; i < CARDPUTER_KEYMAP_KEYS; i++) {

        if (!(released & (1ULL << i))) {
            continue;
        }

        cardputer_keycoord_t coord;

        if (cardputer_keymap_coord_from_index(i, &coord)) {
            handle_event(coord, false);
        }
    }

    s_prev_mask = current;
}

/* =========================
 * INIT
 * ========================= */

esp_err_t cardputer_keyboard_init(cardputer_keyboard_char_cb_t cb)
{
    s_char_cb = cb;

    for (int i = 0; i < 3; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_select_pins[i],
            .mode = GPIO_MODE_OUTPUT
        };
        gpio_config(&cfg);
    }

    for (int i = 0; i < 7; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_input_pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE
        };
        gpio_config(&cfg);
    }

    s_prev_mask = 0;

    ESP_LOGI(TAG, "Cardputer ORIGINAL keyboard ready");
    return ESP_OK;
}

/* =========================
 * POLL
 * ========================= */

void cardputer_keyboard_poll(void)
{
    static uint32_t last = 0;

    uint32_t now = esp_timer_get_time();

    if ((now - last) < 5000) {
        return;
    }

    last = now;

    scan_matrix();
}

/* =========================
 * COMPAT STUBS
 * ========================= */

void cardputer_keyboard_set_char_callback(cardputer_keyboard_char_cb_t cb)
{
    s_char_cb = cb;
}

void cardputer_keyboard_set_key_callback(cardputer_keyboard_key_cb_t cb)
{
    s_key_cb = cb;
}

int cardputer_keyboard_key_is_down(char keycode)
{
    (void)keycode;
    return 0;
}

void cardputer_keyboard_poll_direct(void) {}
void cardputer_keyboard_set_poll_interval_ms(uint32_t ms) {}
uint32_t cardputer_keyboard_get_poll_interval_ms(void) { return 5; }
void cardputer_keyboard_set_background_poll_enabled(int enabled) {}

int cardputer_keyboard_pop_event(cardputer_keyboard_key_event_t *event) { return 0; }
int cardputer_keyboard_peek_event(cardputer_keyboard_key_event_t *event) { return 0; }
void cardputer_keyboard_flush_events_queue(void) {}