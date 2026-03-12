/**
 * FreqHunter for Flipper Zero
 * Scans Sub-GHz frequencies and logs RSSI detections to SD card CSV.
 *
 * Controls:
 *   Up / Down   - change frequency
 *   Left        - lower threshold (5 dBm)
 *   Right       - raise threshold (5 dBm)
 *   OK          - start / stop logging
 *   Back        - exit
 *
 * Log: /ext/apps_data/freqhunter/log.csv
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG         "FreqHunter"
#define LOG_DIR     "/ext/apps_data/freqhunter"
#define LOG_FILE    LOG_DIR "/log.csv"
#define HISTORY_LEN 110
#define DWELL_MS    100

static const uint32_t FREQ_LIST[] = {
    300000000,
    315000000,
    345000000,
    390000000,
    418000000,
    433920000,
    434420000,
    868350000,
    915000000,
    928000000,
};
#define FREQ_COUNT ((int)(sizeof(FREQ_LIST) / sizeof(FREQ_LIST[0])))

#define DEFAULT_THRESHOLD -85.0f
#define THRESHOLD_MIN     -110.0f
#define THRESHOLD_MAX     -40.0f
#define THRESHOLD_STEP    5.0f

/*
 * OOK 650kHz async preset register bytes for CC1101.
 * These are the raw register address+value pairs taken directly from
 * the Flipper Zero firmware (furi_hal_subghz_preset_ook_650khz_async_regs).
 * Terminated with a 0x00,0x00 pair.
 */
static const uint8_t OOK_650K_REGS[] = {
    0x02, 0x0D, /* IOCFG0   */
    0x03, 0x07, /* FIFOTHR  */
    0x08, 0x32, /* PKTCTRL0 */
    0x0B, 0x06, /* FSCTRL1  */
    0x10, 0x17, /* MDMCFG4  */
    0x11, 0x32, /* MDMCFG3  */
    0x12, 0x30, /* MDMCFG2  */
    0x13, 0x20, /* MDMCFG1  */
    0x14, 0x75, /* MDMCFG0  */
    0x15, 0x00, /* DEVIATN  */
    0x19, 0x18, /* FOCCFG   */
    0x1A, 0x18, /* BSCFG    */
    0x1B, 0x1D, /* AGCCTRL2 */
    0x1C, 0x1C, /* AGCCTRL1 */
    0x1D, 0xC7, /* AGCCTRL0 */
    0x20, 0xFB, /* WORCTRL  */
    0x21, 0xB6, /* FREND1   */
    0x22, 0x11, /* FREND0   */
    0x23, 0xEA, /* FSCAL3   */
    0x24, 0x2A, /* FSCAL2   */
    0x25, 0x00, /* FSCAL1   */
    0x26, 0x1F, /* FSCAL0   */
    0x00, 0x00  /* END      */
};

typedef enum { AppEventInput, AppEventTimer } AppEventType;

typedef struct {
    AppEventType type;
    InputEvent   input;
} AppEvent;

typedef struct {
    int      freq_index;
    float    rssi;
    float    threshold;
    bool     logging;
    float    history[HISTORY_LEN];
    int      hist_pos;
    uint32_t log_count;
    uint32_t detections;
    FuriMessageQueue* queue;
    ViewPort*         vp;
    Gui*              gui;
    FuriTimer*        timer;
    NotificationApp*  notif;
    Storage*          storage;
    File*             log_file;
} App;

/* ── helpers ─────────────────────────────────────────────────────────── */

static int rssi_px(float rssi, int max) {
    float v = rssi < -110.0f ? -110.0f : rssi > -20.0f ? -20.0f : rssi;
    return (int)((v + 110.0f) / 90.0f * (float)max);
}

static void dashed_line(Canvas* c, int y, int x0, int x1) {
    for(int x = x0; x <= x1; x += 4) {
        canvas_draw_dot(c, x, y);
        canvas_draw_dot(c, x + 1, y);
    }
}

/* ── draw ────────────────────────────────────────────────────────────── */

static void draw_cb(Canvas* c, void* ctx) {
    App* app = (App*)ctx;
    canvas_clear(c);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 10, "FreqHunter");
    if(app->logging) {
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 95, 10, "[REC]");
    }

    uint32_t hz  = FREQ_LIST[app->freq_index];
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu.%03lu MHz",
             (unsigned long)mhz, (unsigned long)khz);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 21, buf);

    snprintf(buf, sizeof(buf), "%d/%d", app->freq_index + 1, FREQ_COUNT);
    canvas_draw_str(c, 95, 21, buf);

    snprintf(buf, sizeof(buf), "RSSI:%d dBm", (int)app->rssi);
    canvas_draw_str(c, 0, 31, buf);
    snprintf(buf, sizeof(buf), "T:%d", (int)app->threshold);
    canvas_draw_str(c, 88, 31, buf);

    const int WY0 = 33, WY1 = 55, WH = WY1 - WY0;
    canvas_draw_frame(c, 0, WY0, HISTORY_LEN, WH + 1);
    int ty = WY1 - rssi_px(app->threshold, WH);
    dashed_line(c, ty, 1, HISTORY_LEN - 2);
    for(int i = 0; i < HISTORY_LEN; i++) {
        int idx = (app->hist_pos + i) % HISTORY_LEN;
        int px  = rssi_px(app->history[idx], WH);
        if(px > 0) canvas_draw_line(c, i, WY1 - px, i, WY1);
    }

    snprintf(buf, sizeof(buf), "Log:%lu Det:%lu",
             (unsigned long)app->log_count,
             (unsigned long)app->detections);
    canvas_draw_str(c, 0, 64, buf);
    canvas_draw_str(c, 90, 64, app->logging ? "OK=Stop" : "OK=Log");
}

/* ── input ───────────────────────────────────────────────────────────── */

static void input_cb(InputEvent* ev, void* ctx) {
    App* app = (App*)ctx;
    AppEvent e = { .type = AppEventInput, .input = *ev };
    furi_message_queue_put(app->queue, &e, FuriWaitForever);
}

/* ── timer ───────────────────────────────────────────────────────────── */

static void timer_cb(void* ctx) {
    App* app = (App*)ctx;
    AppEvent e = { .type = AppEventTimer };
    furi_message_queue_put(app->queue, &e, 0);
}

/* ── radio ───────────────────────────────────────────────────────────── */

static void radio_rx(App* app) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(OOK_650K_REGS);
    furi_hal_subghz_set_frequency_and_path(FREQ_LIST[app->freq_index]);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();
}

/* ── storage ─────────────────────────────────────────────────────────── */

static bool log_open(App* app) {
    app->log_file = storage_file_alloc(app->storage);
    if(!storage_file_open(
           app->log_file, LOG_FILE, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(app->log_file);
        app->log_file = NULL;
        return false;
    }
    if(storage_file_size(app->log_file) == 0) {
        const char* h = "timestamp_ms,frequency_hz,rssi_dbm\n";
        storage_file_write(app->log_file, h, strlen(h));
    }
    return true;
}

static void log_entry(App* app, uint32_t ts, uint32_t freq, int rssi) {
    if(!app->log_file) return;
    char line[64];
    int len = snprintf(
        line, sizeof(line), "%lu,%lu,%d\n",
        (unsigned long)ts, (unsigned long)freq, rssi);
    storage_file_write(app->log_file, line, (size_t)len);
    app->log_count++;
}

static void log_close(App* app) {
    if(app->log_file) {
        storage_file_close(app->log_file);
        storage_file_free(app->log_file);
        app->log_file = NULL;
    }
}

/* ── entry point ─────────────────────────────────────────────────────── */

int32_t freqhunter_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "FreqHunter start");

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->freq_index = 5; /* 433.92 MHz default */
    app->threshold  = DEFAULT_THRESHOLD;
    app->rssi       = -100.0f;
    for(int i = 0; i < HISTORY_LEN; i++) app->history[i] = -110.0f;

    app->queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->vp    = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    app->gui     = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    app->notif   = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(app->storage, LOG_DIR);

    radio_rx(app);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(DWELL_MS));

    bool run = true;
    AppEvent ev;

    while(run) {
        if(furi_message_queue_get(
               app->queue, &ev, FuriWaitForever) != FuriStatusOk)
            continue;

        if(ev.type == AppEventTimer) {
            app->rssi = furi_hal_subghz_get_rssi();
            app->history[app->hist_pos] = app->rssi;
            app->hist_pos = (app->hist_pos + 1) % HISTORY_LEN;

            if(app->rssi > app->threshold) {
                app->detections++;
                notification_message(app->notif, &sequence_blink_green_10);
                if(app->logging) {
                    uint32_t ts = furi_get_tick() /
                        (furi_kernel_get_tick_frequency() / 1000);
                    log_entry(app, ts,
                              FREQ_LIST[app->freq_index],
                              (int)app->rssi);
                }
            }
            view_port_update(app->vp);

        } else if(ev.type == AppEventInput) {
            if(ev.input.type == InputTypeShort ||
               ev.input.type == InputTypeRepeat) {
                switch(ev.input.key) {
                case InputKeyUp:
                    app->freq_index =
                        (app->freq_index - 1 + FREQ_COUNT) % FREQ_COUNT;
                    furi_hal_subghz_sleep();
                    radio_rx(app);
                    break;
                case InputKeyDown:
                    app->freq_index =
                        (app->freq_index + 1) % FREQ_COUNT;
                    furi_hal_subghz_sleep();
                    radio_rx(app);
                    break;
                case InputKeyLeft:
                    if(app->threshold > THRESHOLD_MIN)
                        app->threshold -= THRESHOLD_STEP;
                    break;
                case InputKeyRight:
                    if(app->threshold < THRESHOLD_MAX)
                        app->threshold += THRESHOLD_STEP;
                    break;
                case InputKeyOk:
                    if(!app->logging) {
                        if(log_open(app)) {
                            app->logging = true;
                            notification_message(
                                app->notif, &sequence_blink_blue_10);
                        }
                    } else {
                        log_close(app);
                        app->logging = false;
                        notification_message(
                            app->notif, &sequence_blink_red_10);
                    }
                    break;
                case InputKeyBack:
                    run = false;
                    break;
                default:
                    break;
                }
            }
        }
    }

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    furi_hal_subghz_sleep();
    if(app->logging) log_close(app);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_message_queue_free(app->queue);
    free(app);

    FURI_LOG_I(TAG, "FreqHunter exit");
    return 0;
}
