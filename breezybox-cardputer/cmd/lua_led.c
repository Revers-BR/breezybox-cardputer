/*
 * lua_led.c - breezy.led: addressable LED strips (WS2812 / NeoPixel).
 *
 * These units take a single data line with timing far too tight to bit-bang
 * from Lua, so this drives the RMT peripheral: symbols are emitted by hardware
 * and the timing holds regardless of what the CPU is doing.
 *
 * Why this exists at all: pointing i2c_scan at a NeoPixel unit feeds 128
 * addresses of garbage into its data line, which lights the LEDs at arbitrary
 * values, spikes the current draw on the shared 5V rail and browns the board
 * out. The unit is not an I2C device and never was. With a real driver the
 * agent can set a colour deliberately -- and modestly.
 *
 * Power: the Grove 5V pin shares the board rail. A handful of LEDs at full
 * white is enough to reset the device on battery, which is why brightness is
 * scaled and defaults well below maximum.
 */

#include "breezy_cmd.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"

#include "lauxlib.h"
#include "lua.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "breezy_led";

/* 10 MHz: one tick is 0.1 us, which expresses WS2812 timing exactly. */
#define LED_RESOLUTION_HZ  (10 * 1000 * 1000)
#define LED_MAX_PIXELS     256

/* Default brightness, as a percentage. Deliberately low: the first thing a
 * user does is ask for white, and full white on the Grove rail is what caused
 * the brownout this driver was written in response to. */
#define LED_DEFAULT_BRIGHTNESS 25

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static uint8_t *s_pixels;          /* GRB, three bytes per LED */
static int      s_count;
static int      s_brightness = LED_DEFAULT_BRIGHTNESS;

static void led_teardown(void)
{
    if (s_chan) {
        rmt_disable(s_chan);
        rmt_del_channel(s_chan);
        s_chan = NULL;
    }
    if (s_encoder) {
        rmt_del_encoder(s_encoder);
        s_encoder = NULL;
    }
    free(s_pixels);
    s_pixels = NULL;
    s_count = 0;
}

/* breezy.led.open(pin, count [, { brightness = 0..100 }]) */
static int l_led_open(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int count = (int)luaL_checkinteger(L, 2);

    if (count < 1 || count > LED_MAX_PIXELS) {
        return luaL_error(L, "count must be 1..%d", LED_MAX_PIXELS);
    }

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "brightness");
        if (lua_isnumber(L, -1)) {
            s_brightness = (int)lua_tointeger(L, -1);
            if (s_brightness < 0)   s_brightness = 0;
            if (s_brightness > 100) s_brightness = 100;
        }
        lua_pop(L, 1);
    }

    led_teardown();

    s_pixels = calloc((size_t)count, 3);
    if (!s_pixels) {
        return luaL_error(L, "out of memory for %d pixels", count);
    }
    s_count = count;

    rmt_tx_channel_config_t chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = pin,
        .mem_block_symbols = 64,
        .resolution_hz     = LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        led_teardown();
        return luaL_error(L, "cannot open RMT on pin %d: %s", pin, esp_err_to_name(err));
    }

    /* WS2812: a 0 bit is 0.3 us high then 0.9 us low, a 1 bit the reverse. */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        led_teardown();
        return luaL_error(L, "cannot create encoder: %s", esp_err_to_name(err));
    }

    /*
     * Estimate the worst case and say so. A WS2812 draws roughly 60 mA at full
     * white, and the Grove rail is shared with the CPU, screen and radio --
     * whose transmit bursts already peak above 300 mA. Being told the number
     * beforehand is better than discovering it as a brownout.
     */
    int worst_ma = (count * 60 * s_brightness) / 100;
    if (worst_ma > 150) {
        ESP_LOGW(TAG, "%d LEDs at %d%% can draw about %d mA at full white; "
                      "the Grove rail is shared with the radio and screen. "
                      "Power the strip separately, or lower the brightness.",
                 count, s_brightness, worst_ma);
        printf("note: %d LEDs at %d%% brightness can draw ~%d mA at full white.\n"
               "      That may brown out the board. Lower the brightness, or\n"
               "      power the strip from its own 5V supply sharing only GND.\n",
               count, s_brightness, worst_ma);
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        led_teardown();
        return luaL_error(L, "cannot enable RMT: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static uint8_t scale(int v)
{
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)((v * s_brightness) / 100);
}

/* breezy.led.set(index, r, g, b)  -- index is 1-based, as Lua tables are */
static int l_led_set(lua_State *L)
{
    if (!s_pixels) {
        return luaL_error(L, "call breezy.led.open first");
    }
    int i = (int)luaL_checkinteger(L, 1);
    if (i < 1 || i > s_count) {
        return luaL_error(L, "index %d out of range 1..%d", i, s_count);
    }
    uint8_t r = scale((int)luaL_checkinteger(L, 2));
    uint8_t g = scale((int)luaL_checkinteger(L, 3));
    uint8_t b = scale((int)luaL_checkinteger(L, 4));

    uint8_t *p = s_pixels + (size_t)(i - 1) * 3;
    p[0] = g;                      /* WS2812 wants GRB, not RGB */
    p[1] = r;
    p[2] = b;
    return 0;
}

static int l_led_fill(lua_State *L)
{
    if (!s_pixels) {
        return luaL_error(L, "call breezy.led.open first");
    }
    uint8_t r = scale((int)luaL_checkinteger(L, 1));
    uint8_t g = scale((int)luaL_checkinteger(L, 2));
    uint8_t b = scale((int)luaL_checkinteger(L, 3));

    for (int i = 0; i < s_count; i++) {
        uint8_t *p = s_pixels + (size_t)i * 3;
        p[0] = g;
        p[1] = r;
        p[2] = b;
    }
    return 0;
}

static int l_led_clear(lua_State *L)
{
    if (!s_pixels) {
        return luaL_error(L, "call breezy.led.open first");
    }
    memset(s_pixels, 0, (size_t)s_count * 3);
    return 0;
}

/* Nothing reaches the strip until show(). */
static int l_led_show(lua_State *L)
{
    if (!s_pixels || !s_chan || !s_encoder) {
        return luaL_error(L, "call breezy.led.open first");
    }
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, s_pixels,
                                 (size_t)s_count * 3, &tx);
    if (err != ESP_OK) {
        return luaL_error(L, "transmit failed: %s", esp_err_to_name(err));
    }
    rmt_tx_wait_all_done(s_chan, 1000);
    return 0;
}

/* breezy.led.brightness([pct]) -> pct */
static int l_led_brightness(lua_State *L)
{
    if (lua_isnumber(L, 1)) {
        int b = (int)lua_tointeger(L, 1);
        if (b < 0)   b = 0;
        if (b > 100) b = 100;
        s_brightness = b;
    }
    lua_pushinteger(L, s_brightness);
    return 1;
}

/* Turn the strip off before releasing it: a WS2812 latches its last value, so
 * closing without clearing leaves the LEDs lit indefinitely. */
static int l_led_close(lua_State *L)
{
    if (s_pixels && s_chan && s_encoder) {
        memset(s_pixels, 0, (size_t)s_count * 3);
        rmt_transmit_config_t tx = { .loop_count = 0 };
        if (rmt_transmit(s_chan, s_encoder, s_pixels,
                         (size_t)s_count * 3, &tx) == ESP_OK) {
            rmt_tx_wait_all_done(s_chan, 1000);
        }
    }
    led_teardown();
    ESP_LOGD(TAG, "closed");
    return 0;
}

static const luaL_Reg s_breezy_led_lib[] = {
    { "open",       l_led_open },
    { "set",        l_led_set },
    { "fill",       l_led_fill },
    { "clear",      l_led_clear },
    { "show",       l_led_show },
    { "brightness", l_led_brightness },
    { "close",      l_led_close },
    { NULL, NULL },
};

const luaL_Reg *breezy_lua_led_lib(void)
{
    return s_breezy_led_lib;
}
