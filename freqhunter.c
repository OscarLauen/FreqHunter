/**
 * FreqHunter v2.0 for Flipper Zero
 * by Smoodiehacking
 *
 * NEW in v2.0:
 *   - Auto frequency hop mode (hold OK to toggle)
 *   - Peak hold line on waveform
 *   - Signal strength meter bar
 *   - Frequency name labels (e.g. "433 ISM", "915 US")
 *   - Detection flash effect on screen
 *   - Log count resets each session
 *
 * Controls:
 *   Up / Down        - change frequency
 *   Left             - lower threshold (5 dBm)
 *   Right            - raise threshold (5 dBm)
 *   OK (short)       - start / stop logging
 *   OK (long)        - toggle auto hop mode
 *   Back             - exit
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
#define HOP_MS      400  /* auto-hop interval */

typedef struct {
    uint32_t    hz;
    const char* name;
} FreqEntry;

static const FreqEntry FREQ_LIST[] = {
    { 300000000, "300 MHz" },
    { 315000000, "315 MHz" },
    { 345000000, "345 MHz" },
    { 390000000, "390 MHz" },
    { 418000000, "418 MHz" },
    { 433920000, "433 ISM" },
    { 434420000, "434 MHz" },
    { 868350000, "868 EU " },
    { 915000000, "915 US " },
    { 928000000, "928 MHz" },
};
#define FREQ_COUNT ((int)(sizeof(FREQ_LIST) / sizeof(FREQ_LIST[0])))

#define DEFAULT_THRESHOLD -85.0f
#define THRESHOLD_MIN     -110.0f
#define THRESHOLD_MAX     -40.0f
#define THRESHOLD_STEP    5.0f

/* OOK 650kHz CC1101 register preset */
static const uint8_t OOK_650K_REGS[] = {
    0x02, 0x0D,
    0x03, 0x07,
    0x08, 0x32,
    0x0B, 0x06,
    0x10, 0x17,
    0x11, 0x32,
    0x12, 0x30,
    0x13, 0x20,
    0x14, 0x75,
    0x15, 0x00,
    0x19, 0x18,
    0x1A, 0x18,
    0x1B, 0x1D,
    0x1C, 0x1C,
    0x1D, 0xC7,
    0x20, 0xFB,
    0x21, 0xB6,
    0x22, 0x11,
    0x23, 0xEA,
    0x24, 0x2A,
    0x25, 0x00,
    0x26, 0x1F,
    0x00, 0x00
};

typedef enum { AppEventInput, AppEventTimer } AppEventType;

typedef struct {
    AppEventType type;
    InputEvent   input;
} AppEvent;

typedef struct {
    int      freq_index;
    float    rssi;
    float    peak;           /* peak hold value */
    float    threshold;
    bool     logging;
    bool     auto_hop;       /* auto frequency hopping */
    bool     detected;       /* flash effect trigger */
    int      detect_flash;   /* countdown for flash frames */
    float    history[HISTORY_LEN];
    int      hist_pos;
    uint32_t log_count;
    uint32_t detections;
    int      hop_tick;       /* counts ticks for auto hop */
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

    /* detection flash — invert screen briefly */
    if(app->detect_flash > 0) {
        canvas_set_color(c, ColorBlack);
        canvas_draw_box(c, 0, 0, 128, 64);
        canvas_set_color(c, ColorWhite);
    }

    /* title */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 10, "FreqHunter");

    /* status badges */
    canvas_set_font(c, FontSecondary);
    if(app->logging)  canvas_draw_str(c, 83, 10, "[REC]");
    if(app->auto_hop) canvas_draw_str(c, 108, 10, "[H]");

    /* frequency + name */
    uint32_t hz  = FREQ_LIST[app->freq_index].hz;
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu.%03lu  %s",
             (unsigned long)mhz, (unsigned long)khz,
             FREQ_LIST[app->freq_index].name);
    canvas_draw_str(c, 0, 21, buf);

    snprintf(buf, sizeof(buf), "%d/%d", app->freq_index + 1, FREQ_COUNT);
    canvas_draw_str(c, 104, 21, buf);

    /* RSSI + threshold */
    snprintf(buf, sizeof(buf), "RSSI:%d", (int)app->rssi);
    canvas_draw_str(c, 0, 31, buf);
    snprintf(buf, sizeof(buf), "Pk:%d", (int)app->peak);
    canvas_draw_str(c, 50, 31, buf);
    snprintf(buf, sizeof(buf), "T:%d", (int)app->threshold);
    canvas_draw_str(c, 95, 31, buf);

    /* signal strength bar (5 segments) */
    int strength = rssi_px(app->rssi, 5 * 5) / 5; /* 0-5 */
    for(int i = 0; i < 5; i++) {
        if(i < strength)
            canvas_draw_box(c, 50 + i * 7, 23, 5, 5);
        else
            canvas_draw_frame(c, 50 + i * 7, 23, 5, 5);
    }

    /* waveform */
    const int WY0 = 33, WY1 = 55, WH = WY1 - WY0;
    canvas_draw_frame(c, 0, WY0, HISTORY_LEN, WH + 1);

    /* threshold dashed line */
    int ty = WY1 - rssi_px(app->threshold, WH);
    dashed_line(c, ty, 1, HISTORY_LEN - 2);

    /* peak hold line */
    int py = WY1 - rssi_px(app->peak, WH);
    canvas_draw_line(c, 1, py, HISTORY_LEN - 2, py);

    /* waveform bars */
    for(int i = 0; i < HISTORY_LEN; i++) {
        int idx = (app->hist_pos + i) % HISTORY_LEN;
        int px  = rssi_px(app->history[idx], WH);
        if(px > 0) canvas_draw_line(c, i, WY1 - px, i, WY1);
    }

    /* status bar */
    snprintf(buf, sizeof(buf), "Log:%lu Det:%lu",
             (unsigned long)app->log_count,
             (unsigned long)app->detections);
    canvas_draw_str(c, 0, 64, buf);
    canvas_draw_str(c, 90, 64, app->logging ? "OK=Stop" : "OK=Log");

    /* reset color */
    canvas_set_color(c, ColorBlack);
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
    furi_hal_subghz_set_frequency_and_path(FREQ_LIST[app->freq_index].hz);
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
        const char* h = "timestamp_ms,frequency_hz,freq_name,rssi_dbm\n";
        storage_file_write(app->log_file, h, strlen(h));
    }
    return true;
}

static void log_entry(App* app, uint32_t ts, int idx, int rssi) {
    if(!app->log_file) return;
    char line[80];
    int len = snprintf(
        line, sizeof(line), "%lu,%lu,%s,%d\n",
        (unsigned long)ts,
        (unsigned long)FREQ_LIST[idx].hz,
        FREQ_LIST[idx].name,
        rssi);
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
    FURI_LOG_I(TAG, "FreqHunter v2.0 start");

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->freq_index   = 5;
    app->threshold    = DEFAULT_THRESHOLD;
    app->rssi         = -100.0f;
    app->peak         = -110.0f;
    app->auto_hop     = false;
    app->detect_flash = 0;
    for(int i = 0; i < HISTORY_LEN; i++) app->history[i] = -110.0f;

    app->queue   = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->vp      = view_port_alloc();
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

            /* peak hold */
            if(app->rssi > app->peak) app->peak = app->rssi;
            /* decay peak slowly */
            if(app->peak > -110.0f) app->peak -= 0.3f;

            /* detection */
            if(app->rssi > app->threshold) {
                app->detections++;
                app->detect_flash = 2;
                notification_message(app->notif, &sequence_blink_green_10);
                if(app->logging) {
                    uint32_t ts = furi_get_tick() /
                        (furi_kernel_get_tick_frequency() / 1000);
                    log_entry(app, ts, app->freq_index, (int)app->rssi);
                }
            }

            /* flash countdown */
            if(app->detect_flash > 0) app->detect_flash--;

            /* auto hop */
            if(app->auto_hop) {
                app->hop_tick++;
                if(app->hop_tick >= (HOP_MS / DWELL_MS)) {
                    app->hop_tick = 0;
                    app->freq_index = (app->freq_index + 1) % FREQ_COUNT;
                    app->peak = -110.0f; /* reset peak on hop */
                    furi_hal_subghz_sleep();
                    radio_rx(app);
                }
            }

            view_port_update(app->vp);

        } else if(ev.type == AppEventInput) {
            /* short press */
            if(ev.input.type == InputTypeShort) {
                switch(ev.input.key) {
                case InputKeyUp:
                    app->freq_index =
                        (app->freq_index - 1 + FREQ_COUNT) % FREQ_COUNT;
                    app->peak = -110.0f;
                    furi_hal_subghz_sleep();
                    radio_rx(app);
                    break;
                case InputKeyDown:
                    app->freq_index =
                        (app->freq_index + 1) % FREQ_COUNT;
                    app->peak = -110.0f;
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
                default: break;
                }
            }
            /* long press OK = toggle auto hop */
            if(ev.input.type == InputTypeLong &&
               ev.input.key == InputKeyOk) {
                app->auto_hop = !app->auto_hop;
                app->hop_tick = 0;
                app->peak = -110.0f;
                notification_message(app->notif, &sequence_blink_blue_10);
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

    FURI_LOG_I(TAG, "FreqHunter v2.0 exit");
    return 0;
}
