/*
 * rgb_display.c - Cardputer ADV text-mode display backend
 *
 * The original component targeted a 1024x600 RGB panel with DMA bounce buffers.
 * For Cardputer ADV we keep the same public API, but render a compact text mode
 * and push it to the ST7789 SPI panel at a steady refresh rate.
 *
 * The panel is fed in horizontal strips rather than from a full-screen copy.
 * A 240x135 RGB565 frame is 64.8 KB of DMA memory, which was the largest
 * single allocation in the firmware and the reason WiFi and Bluetooth could
 * not run together; two strips of STRIP_LINES lines are 7.7 KB.
 *
 * Two strips so one can be filled while the other is on the wire. The SPI
 * panel IO drains every queued transfer before it starts the next
 * draw_bitmap (tx_param and tx_color both wait on num_trans_inflight), so at
 * most one transfer is in flight and it is always the one queued last: the
 * other strip is free to write. s_strip_lock stops the refresh task and the
 * direct-draw calls from interleaving, which would break that alternation.
 */

#include "rgb_display.h"
#include "rgb_gfx.h"
#include "board_runtime.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#define PROGMEM
#include "glcdfont.h"

static const char *TAG = "display";

#define LCD_MAX_DRAW_WIDTH      240
#define LCD_MAX_DRAW_HEIGHT     135
#define GFX_FB_VGA_WIDTH        320
#define GFX_FB_VGA_HEIGHT       200
#define GFX_FB_150P_WIDTH       240
#define GFX_FB_150P_HEIGHT      150
#define GFX_FB_150P_BYTES       (GFX_FB_150P_WIDTH * GFX_FB_150P_HEIGHT)

#define FONT_BITMAP_WIDTH       5
#define FONT_WIDTH              6
#define FONT_HEIGHT             8
#define TEXT_COLS               DISPLAY_COLS
#define TEXT_ROWS               DISPLAY_ROWS
#define CURSOR_HEIGHT           2
#define REFRESH_PERIOD_MS       33
#define STRIP_LINES             8   /* must be >= FONT_HEIGHT: a text row is one strip */

static lcd_cell_t *s_display_buffer;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static const board_display_config_t *s_cfg;
static screen_mode_t s_screen_mode = SM_TEXT;
static const rgb_display_callbacks_t *s_callbacks;
static uint16_t *s_strip[2];
static int s_strip_last = 1;   /* index of the strip queued most recently */
static SemaphoreHandle_t s_strip_lock;
static uint8_t *s_gfx_buffer;
static int s_gfx_width;
static int s_gfx_height;
static size_t s_gfx_capacity_bytes;
static TaskHandle_t s_refresh_task;
static volatile int s_cursor_col = -1;
static volatile int s_cursor_row = -1;
static volatile bool s_text_refresh_enabled = true;
static uint32_t s_frame_count;
static uint32_t s_text_colors[16];
static uint16_t s_vga_palette[256];
static uint8_t s_backlight_level = 255;
static bool s_gfx_lent;

static void release_gfx_buffer(void);
static void apply_backlight_level(uint8_t level);

static inline int lcd_draw_width(void)
{
    return s_cfg ? s_cfg->draw_width : LCD_MAX_DRAW_WIDTH;
}

static inline int lcd_draw_height(void)
{
    return s_cfg ? s_cfg->draw_height : LCD_MAX_DRAW_HEIGHT;
}

static inline int text_offset_y(void)
{
    return (lcd_draw_height() - (TEXT_ROWS * FONT_HEIGHT)) / 2;
}

static const uint16_t s_cga_colors[16] = {
    0x0000, 0x0015, 0x0540, 0x0555,
    0xA800, 0xA815, 0xA520, 0xAD55,
    0x52AA, 0x52BF, 0x57EA, 0x57FF,
    0xFAAA, 0xFABF, 0xFFE0, 0xFFFF,
};

static inline uint8_t glyph_col_bits(uint8_t ch, int col)
{
    return font[((uint8_t)ch * FONT_BITMAP_WIDTH) + col];
}

static void init_vga_palette(void)
{
    memcpy(s_vga_palette, s_cga_colors, sizeof(s_cga_colors));
    for (int i = 16; i < 256; ++i) {
        s_vga_palette[i] = s_cga_colors[i & 0x0F];
    }
}

static void rebuild_text_palette(void)
{
    const uint16_t *palette = (s_callbacks && s_callbacks->get_text_palette)
        ? s_callbacks->get_text_palette()
        : s_cga_colors;

    for (int i = 0; i < 16; ++i) {
        s_text_colors[i] = palette[i];
    }
}

/* The strip that is not on the wire. Call with s_strip_lock held. */
static uint16_t *strip_next(void)
{
    s_strip_last ^= 1;
    return s_strip[s_strip_last];
}

static esp_err_t ensure_gfx_buffer(size_t bytes)
{
    if (s_gfx_buffer && s_gfx_capacity_bytes >= bytes) {
        return ESP_OK;
    }
    if (s_gfx_lent) {
        /* Its memory is someone's heap right now; replacing it would free
         * that out from under them. */
        return ESP_ERR_INVALID_STATE;
    }

    release_gfx_buffer();

    s_gfx_buffer = heap_caps_malloc(
        bytes,
        MALLOC_CAP_8BIT
    );
    if (!s_gfx_buffer) {
        ESP_LOGE(TAG,
                 "Failed to allocate graphics framebuffer (%u bytes, free=%u, largest=%u)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    s_gfx_capacity_bytes = bytes;

    return ESP_OK;
}

static void release_gfx_buffer(void)
{
    if (s_gfx_buffer) {
        heap_caps_free(s_gfx_buffer);
        s_gfx_buffer = NULL;
    }
    s_gfx_capacity_bytes = 0;
}

/*
 * Lend the graphics framebuffer's memory, and take it back.
 *
 * The buffer is reserved at boot because the heap fragments within seconds and
 * a 36 KB contiguous block never becomes available again. claw needs that
 * memory while it runs. It used to free the buffer and allocate it again on
 * exit, but allocations made during the session (lwIP, WiFi, caches) landed in
 * the gap, and the 36 KB block could not be had back: graphics then needed a
 * reboot. Lending keeps the allocation in place -- the borrower uses the
 * memory as its own heap and hands it back whole.
 */
void *rgb_display_lend_gfx(size_t *size)
{
    if (s_gfx_lent || rgb_display_get_mode() != SM_TEXT ||
        ensure_gfx_buffer(GFX_FB_150P_BYTES) != ESP_OK) {
        return NULL;
    }
    s_gfx_lent = true;
    if (size) {
        *size = s_gfx_capacity_bytes;
    }
    return s_gfx_buffer;
}

void rgb_display_return_gfx(void)
{
    s_gfx_lent = false;
}


static void apply_backlight_level(uint8_t level)
{
    board_set_backlight(level);
    s_backlight_level = level;
}

static void render_text_line(uint16_t *dst, int y, bool blink_on)
{
    const int width = lcd_draw_width();
    const int ty = y - text_offset_y();

    if (!s_display_buffer || ty < 0 || ty >= TEXT_ROWS * FONT_HEIGHT) {
        const uint16_t bg = (uint16_t)s_text_colors[0];
        for (int x = 0; x < width; ++x) {
            dst[x] = bg;
        }
        return;
    }

    const int row = ty / FONT_HEIGHT;
    const int glyph_row = ty % FONT_HEIGHT;
    const int cursor_col = (blink_on && row == s_cursor_row &&
                            glyph_row >= FONT_HEIGHT - CURSOR_HEIGHT) ? s_cursor_col : -1;
    const lcd_cell_t *cell_row = &s_display_buffer[row * TEXT_COLS];

    for (int col = 0; col < TEXT_COLS; ++col) {
        const lcd_cell_t cell = cell_row[col];
        const uint16_t fg = (uint16_t)s_text_colors[LCD_ATTR_FG(cell.attr)];
        const uint16_t bg = (uint16_t)s_text_colors[LCD_ATTR_BG(cell.attr)];
        uint16_t *px = &dst[col * FONT_WIDTH];

        if (col == cursor_col) {
            for (int x = 0; x < FONT_WIDTH; ++x) {
                px[x] = fg;
            }
            continue;
        }
        for (int glyph_col = 0; glyph_col < FONT_BITMAP_WIDTH; ++glyph_col) {
            const uint8_t bits = glyph_col_bits((uint8_t)cell.ch, glyph_col);
            px[glyph_col] = (bits & (1U << glyph_row)) ? fg : bg;
        }
        px[FONT_BITMAP_WIDTH] = bg;
    }
    for (int x = TEXT_COLS * FONT_WIDTH; x < width; ++x) {
        dst[x] = (uint16_t)s_text_colors[0];
    }
}

static void render_gfx_line(uint16_t *dst, int y)
{
    const int src_w = s_gfx_width;
    const int src_h = s_gfx_height;
    const int draw_width = lcd_draw_width();
    const int sy = (y * src_h) / lcd_draw_height();
    const uint8_t *src_row = &s_gfx_buffer[sy * src_w];

    for (int x = 0; x < draw_width; ++x) {
        const int sx = (x * src_w) / draw_width;
        dst[x] = s_vga_palette[src_row[sx]];
    }
}

/* Render and send one whole frame, STRIP_LINES lines at a time. */
static void push_frame(bool gfx)
{
    const int width = lcd_draw_width();
    const int height = lcd_draw_height();
    const bool blink_on = ((s_frame_count / 15U) & 1U) != 0;

    xSemaphoreTake(s_strip_lock, portMAX_DELAY);
    for (int y0 = 0; y0 < height; y0 += STRIP_LINES) {
        const int lines = (height - y0 < STRIP_LINES) ? (height - y0) : STRIP_LINES;
        uint16_t *strip = strip_next();
        for (int i = 0; i < lines; ++i) {
            if (gfx) {
                render_gfx_line(&strip[i * width], y0 + i);
            } else {
                render_text_line(&strip[i * width], y0 + i, blink_on);
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, width, y0 + lines, strip);
    }
    xSemaphoreGive(s_strip_lock);
}

static void render_text_cell_to_buffer(uint16_t *dst, lcd_cell_t cell, bool cursor)
{
    uint16_t fg = (uint16_t)s_text_colors[LCD_ATTR_FG(cell.attr)];
    uint16_t bg = (uint16_t)s_text_colors[LCD_ATTR_BG(cell.attr)];

    for (int glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
        uint16_t *row_dst = &dst[glyph_row * FONT_WIDTH];
        for (int glyph_col = 0; glyph_col < FONT_BITMAP_WIDTH; ++glyph_col) {
            uint8_t bits = glyph_col_bits((uint8_t)cell.ch, glyph_col);
            row_dst[glyph_col] = (bits & (1U << glyph_row)) ? fg : bg;
        }
        row_dst[FONT_BITMAP_WIDTH] = bg;
    }

    if (cursor) {
        uint16_t *cursor_dst = &dst[(FONT_HEIGHT - CURSOR_HEIGHT) * FONT_WIDTH];
        for (int y = 0; y < CURSOR_HEIGHT; ++y) {
            for (int x = 0; x < FONT_WIDTH; ++x) {
                cursor_dst[x] = fg;
            }
            cursor_dst += FONT_WIDTH;
        }
    }
}

static void direct_draw_cell_internal(int col, int row, lcd_cell_t cell, bool cursor)
{
    if (!s_panel || col < 0 || col >= TEXT_COLS || row < 0 || row >= TEXT_ROWS) {
        return;
    }

    const int pixel_x = col * FONT_WIDTH;
    const int pixel_y = text_offset_y() + (row * FONT_HEIGHT);

    /* The tile goes through a strip rather than the stack: the transfer is
     * still reading it after draw_bitmap returns. */
    xSemaphoreTake(s_strip_lock, portMAX_DELAY);
    uint16_t *tile = strip_next();
    render_text_cell_to_buffer(tile, cell, cursor);
    esp_lcd_panel_draw_bitmap(s_panel,
                              pixel_x,
                              pixel_y,
                              pixel_x + FONT_WIDTH,
                              pixel_y + FONT_HEIGHT,
                              tile);
    xSemaphoreGive(s_strip_lock);
}

static void direct_draw_row_internal(const lcd_cell_t *row_cells, int row, int cursor_col)
{
    if (!s_panel || !row_cells || row < 0 || row >= TEXT_ROWS) {
        return;
    }

    xSemaphoreTake(s_strip_lock, portMAX_DELAY);
    uint16_t *strip = strip_next();
    const int draw_width = lcd_draw_width();
    const int pixel_y = text_offset_y() + (row * FONT_HEIGHT);

    for (int col = 0; col < TEXT_COLS; ++col) {
        uint16_t tile[FONT_WIDTH * FONT_HEIGHT];
        render_text_cell_to_buffer(tile, row_cells[col], cursor_col == col);
        for (int glyph_row = 0; glyph_row < FONT_HEIGHT; ++glyph_row) {
            memcpy(&strip[glyph_row * draw_width + col * FONT_WIDTH],
                   &tile[glyph_row * FONT_WIDTH],
                   FONT_WIDTH * sizeof(uint16_t));
        }
    }

    esp_lcd_panel_draw_bitmap(s_panel,
                              0,
                              pixel_y,
                              draw_width,
                              pixel_y + FONT_HEIGHT,
                              strip);
    xSemaphoreGive(s_strip_lock);
}

static void refresh_task(void *arg)
{
    while (1) {
        if (s_screen_mode == SM_TEXT && s_panel && s_text_refresh_enabled) {
            s_frame_count++;
            push_frame(false);
        }
        else if (s_screen_mode != SM_TEXT && s_panel && s_gfx_buffer) {
            push_frame(true);
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_PERIOD_MS));
    }
}

static int enter_graphics_mode(screen_mode_t mode)
{
    if (s_gfx_lent) {
        ESP_LOGW(TAG, "graphics framebuffer is lent out (claw is running)");
        return -1;
    }
    if (mode == SM_150P) {
        s_gfx_width = GFX_FB_150P_WIDTH;
        s_gfx_height = GFX_FB_150P_HEIGHT;
    } else {
        s_gfx_width = GFX_FB_VGA_WIDTH;
        s_gfx_height = GFX_FB_VGA_HEIGHT;
    }

    if (ensure_gfx_buffer((size_t)s_gfx_width * (size_t)s_gfx_height) != ESP_OK) {
        s_gfx_width = 0;
        s_gfx_height = 0;
        return -1;
    }

    memset(s_gfx_buffer, 0, s_gfx_width * s_gfx_height);
    if (s_callbacks && s_callbacks->enter_graphics) {
        if (s_callbacks->enter_graphics() != 0) {
            return -1;
        }
    }
    s_screen_mode = mode;
    return 0;
}

void rgb_display_init(void)
{
    s_cfg = board_get_display_config();

    spi_bus_config_t buscfg = {
        .sclk_io_num = s_cfg->pin_sclk,
        .mosi_io_num = s_cfg->pin_mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_cfg->draw_width * STRIP_LINES * sizeof(uint16_t),
    };

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = s_cfg->pin_dc,
        .cs_gpio_num = s_cfg->pin_cs,
        .pclk_hz = s_cfg->lcd_pixel_clock_hz,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = s_cfg->pin_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };

    volatile const void *exports[] = {
        (void *)rgb_display_refresh_palette,
        (void *)rgb_display_set_mode,
        (void *)rgb_display_get_mode,
        (void *)rgb_display_get_framebuffer,
        (void *)rgb_display_get_fb_width,
        (void *)rgb_display_get_fb_height,
        (void *)rgb_display_set_vga_palette,
        (void *)rgb_display_set_vga_palette_entry,
        (void *)rgb_display_get_vga_palette_entry,
        (void *)rgb_display_wait_vsync,
        (void *)rgb_gfx_clear,
        (void *)rgb_gfx_pixel,
        (void *)rgb_gfx_hline,
        (void *)rgb_gfx_vline,
        (void *)rgb_gfx_rect,
        (void *)rgb_gfx_rectfill,
        (void *)rgb_gfx_blit,
        (void *)rgb_gfx_blit_flip,
    };
    (void)exports;

    init_vga_palette();
    rebuild_text_palette();

    // Reserve the lighter 150p graphics framebuffer up front so later Lua/script
    // activity does not fragment the heap before first graphics-mode entry.
    //
    // Reserved on every profile, including the slim ESP-Claw one.
    //
    // Skipping it there looked like the single largest saving available on a
    // PSRAM-less board, and ensure_gfx_buffer() does allocate on demand -- but
    // on demand is too late. The heap fragments within seconds of boot, and a
    // 36 KB contiguous block is never available again: measured free=72328
    // with largest=31744, so the memory that was "saved" is unusable anyway.
    // Taking it here, while the heap is still whole, is what makes graphics
    // possible at all.
    if (ensure_gfx_buffer(GFX_FB_150P_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "150p graphics buffer was not preallocated at startup");
    }

    ESP_ERROR_CHECK(board_display_power_init());

    for (int i = 0; i < 2; ++i) {
        s_strip[i] = heap_caps_malloc(
            s_cfg->draw_width * STRIP_LINES * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT
        );
        ESP_ERROR_CHECK(s_strip[i] ? ESP_OK : ESP_ERR_NO_MEM);
    }
    s_strip_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_strip_lock ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)s_cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)s_cfg->spi_host, &io_config, &s_panel_io));
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, s_cfg->invert_color));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, s_cfg->gap_x, s_cfg->gap_y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, s_cfg->swap_xy));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, s_cfg->mirror_x, s_cfg->mirror_y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    push_frame(false);   /* no text buffer yet, so this clears the panel */
    apply_backlight_level(255);

    if (!s_refresh_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            refresh_task, "lcd_refresh", 4096, NULL, 4, &s_refresh_task, 1
        );
        ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);
    }

    ESP_LOGI(TAG, "%s display ready: %dx%d pixels, %dx%d chars",
             board_runtime_name(), s_cfg->draw_width, s_cfg->draw_height, TEXT_COLS, TEXT_ROWS);
}

void rgb_display_set_buffer(lcd_cell_t *cells)
{
    s_display_buffer = cells;
}

void rgb_display_set_callbacks(const rgb_display_callbacks_t *cb)
{
    s_callbacks = cb;
    rebuild_text_palette();
}

void rgb_display_refresh_palette(void)
{
    rebuild_text_palette();
}

void rgb_display_set_cursor(int col, int row)
{
    s_cursor_col = col;
    s_cursor_row = row;
}

void rgb_display_direct_text_begin(void)
{
    s_text_refresh_enabled = false;
}

void rgb_display_direct_text_end(void)
{
    s_text_refresh_enabled = true;
}

void rgb_display_direct_text_redraw(const lcd_cell_t *cells, int cursor_col, int cursor_row)
{
    if (!cells || !s_panel) {
        return;
    }

    const lcd_cell_t *saved = s_display_buffer;
    int saved_col = s_cursor_col;
    int saved_row = s_cursor_row;

    s_display_buffer = (lcd_cell_t *)cells;
    s_cursor_col = cursor_col;
    s_cursor_row = cursor_row;
    push_frame(false);

    s_display_buffer = (lcd_cell_t *)saved;
    s_cursor_col = saved_col;
    s_cursor_row = saved_row;
}

void rgb_display_direct_text_draw_cell(int col, int row, lcd_cell_t cell, int draw_cursor)
{
    direct_draw_cell_internal(col, row, cell, draw_cursor != 0);
}

void rgb_display_direct_text_draw_row(const lcd_cell_t *row_cells, int row, int cursor_col)
{
    direct_draw_row_internal(row_cells, row, cursor_col);
}

screen_mode_t rgb_display_get_mode(void)
{
    return s_screen_mode;
}

int rgb_display_set_mode(screen_mode_t mode)
{
    if (mode == SM_TEXT) {
        if (s_screen_mode != SM_TEXT && s_callbacks && s_callbacks->exit_graphics) {
            if (s_callbacks->exit_graphics() != 0) {
                return -1;
            }
        }
        s_screen_mode = SM_TEXT;
        // Keep the graphics buffer reserved across mode switches so later
        // Lua/demo graphics calls do not fail due to heap fragmentation.
        if (s_callbacks && s_callbacks->get_text_buffer) {
            s_display_buffer = s_callbacks->get_text_buffer();
        }
        if (s_callbacks && s_callbacks->flush_input) {
            s_callbacks->flush_input();
        }
        return 0;
    }

    if (mode == SM_VGA13H || mode == SM_150P) {
        return enter_graphics_mode(mode);
    }

    ESP_LOGW(TAG, "Unknown screen mode %d", mode);
    return -1;
}

uint8_t *rgb_display_get_framebuffer(void)
{
    return (s_screen_mode == SM_TEXT) ? NULL : s_gfx_buffer;
}

int rgb_display_get_fb_width(void)
{
    if (s_screen_mode == SM_VGA13H || s_screen_mode == SM_150P) {
        return s_gfx_width;
    }
    return 0;
}

int rgb_display_get_fb_height(void)
{
    if (s_screen_mode == SM_VGA13H || s_screen_mode == SM_150P) {
        return s_gfx_height;
    }
    return 0;
}

void rgb_display_set_vga_palette(const uint16_t palette[256])
{
    memcpy(s_vga_palette, palette, sizeof(s_vga_palette));
}

void rgb_display_set_vga_palette_entry(int index, uint16_t rgb565)
{
    if (index >= 0 && index < 256) {
        s_vga_palette[index] = rgb565;
    }
}

uint16_t rgb_display_get_vga_palette_entry(int index)
{
    if (index >= 0 && index < 256) {
        return s_vga_palette[index];
    }
    return 0;
}

void rgb_display_set_backlight(uint8_t level)
{
    apply_backlight_level(level);
}

uint8_t rgb_display_get_backlight(void)
{
    return s_backlight_level;
}

void rgb_display_wait_vsync(void)
{
}
