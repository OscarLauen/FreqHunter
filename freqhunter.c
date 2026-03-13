/**
 * FreqHunter v4.0 for Flipper Zero
 * by Smoodiehacking
 *
 * PAGES:
 *   Home     - animated start menu
 *   Scanner  - live RSSI waveform, logging, auto-hop
 *   Spectrum - all frequencies as live bar chart (Spectrum Analyzer style)
 *   Decoder  - ProtoView-style raw pulse capture & protocol decode
 *   Settings - sound, modulation, threshold, auto-hop speed
 *   About    - credits & version
 *
 * NAVIGATION:
 *   Home menu: Up/Down = move, OK = select
 *   Back (short) = return to Home from any page
 *   Back (hold 2s) = exit app
 *   Scanner: hold Right 3s = jump to Decoder
 *   Decoder: hold Left  3s = jump to Scanner
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

/* ── build constants ────────────────────────────────────────────────────── */
#define TAG            "FreqHunter"
#define LOG_DIR        "/ext/apps_data/freqhunter"
#define LOG_FILE       LOG_DIR "/log.csv"
#define HISTORY_LEN    110   /* waveform history columns */
#define DWELL_MS       80    /* timer period ms */
#define HOLD_3S        38    /* ticks for 3-second hold  (3000/80) */
#define HOLD_2S        25    /* ticks for 2-second hold  (2000/80) */
#define CAPTURE_MAX    512   /* max pulse edges in decoder */
#define SPEC_SAMPLES   6     /* samples averaged per freq in spectrum */

/* ── CC1101 register presets ────────────────────────────────────────────── */
static const uint8_t MOD_AM650[] = {          /* OOK 650 kHz BW */
    0x02,0x0D,0x03,0x07,0x08,0x32,0x0B,0x06,
    0x10,0x17,0x11,0x32,0x12,0x30,0x13,0x20,
    0x14,0x75,0x15,0x00,0x19,0x18,0x1A,0x18,
    0x1B,0x1D,0x1C,0x1C,0x1D,0xC7,0x20,0xFB,
    0x21,0xB6,0x22,0x11,0x23,0xEA,0x24,0x2A,
    0x25,0x00,0x26,0x1F,0x00,0x00
};
static const uint8_t MOD_AM270[] = {          /* OOK 270 kHz BW */
    0x02,0x0D,0x03,0x07,0x08,0x32,0x0B,0x06,
    0x10,0x57,0x11,0x32,0x12,0x30,0x13,0x20,
    0x14,0x75,0x15,0x00,0x19,0x18,0x1A,0x18,
    0x1B,0x1D,0x1C,0x1C,0x1D,0xC7,0x20,0xFB,
    0x21,0xB6,0x22,0x11,0x23,0xEA,0x24,0x2A,
    0x25,0x00,0x26,0x1F,0x00,0x00
};
static const uint8_t MOD_FM238[] = {          /* 2FSK 2.38 kbps */
    0x02,0x0D,0x03,0x07,0x08,0x05,0x0B,0x06,
    0x10,0xF5,0x11,0x83,0x12,0x13,0x13,0x22,
    0x14,0xF8,0x15,0x47,0x19,0x36,0x1A,0x6C,
    0x1B,0x03,0x1C,0x40,0x1D,0x91,0x20,0xFB,
    0x21,0x56,0x22,0x10,0x23,0xE9,0x24,0x2A,
    0x25,0x00,0x26,0x1F,0x00,0x00
};
static const uint8_t MOD_FM476[] = {          /* 2FSK 4.76 kbps */
    0x02,0x0D,0x03,0x07,0x08,0x05,0x0B,0x06,
    0x10,0xC5,0x11,0x83,0x12,0x13,0x13,0x22,
    0x14,0xF8,0x15,0x47,0x19,0x36,0x1A,0x6C,
    0x1B,0x03,0x1C,0x40,0x1D,0x91,0x20,0xFB,
    0x21,0x56,0x22,0x10,0x23,0xE9,0x24,0x2A,
    0x25,0x00,0x26,0x1F,0x00,0x00
};

typedef struct { const char* name; const uint8_t* regs; } ModPreset;
static const ModPreset MODS[] = {
    {"AM650", MOD_AM650}, {"AM270", MOD_AM270},
    {"FM238", MOD_FM238}, {"FM476", MOD_FM476},
};
#define MOD_COUNT 4

/* ── frequencies ────────────────────────────────────────────────────────── */
typedef struct { uint32_t hz; const char* label; } FreqEntry;
static const FreqEntry FREQS[] = {
    /* Low band 300-348 MHz */
    {300000000,"300"}, {310000000,"310"}, {315000000,"315"}, {318000000,"318"},
    {330000000,"330"}, {345000000,"345"},
    /* Mid band 387-464 MHz */
    {390000000,"390"}, {400000000,"400"}, {418000000,"418"}, {433920000,"433"},
    {434420000,"434"}, {450000000,"450"}, {460000000,"460"},
    /* High band 779-928 MHz */
    {779000000,"779"}, {800000000,"800"}, {820000000,"820"}, {868350000,"868"},
    {900000000,"900"}, {905000000,"905"}, {915000000,"915"}, {920000000,"920"},
    {928000000,"928"},
};
#define FREQ_COUNT 24

#define THR_DEFAULT  -85
#define THR_MIN      -110
#define THR_MAX      -40
#define THR_STEP     5

#define SET_N        5     /* number of settings: Sound, Mod, Freq, AutoHop, BinRaw */

/* ── pages ──────────────────────────────────────────────────────────────── */
typedef enum {
    PageHome, PageScanner, PageSpectrum,
    PageDecoder, PageSettings, PageAbout,
} AppPage;

#define HOME_N 5
static const char* HOME_LABELS[HOME_N] = {
    "  > Scanner",
    "  > Spectrum",
    "  > Decoder",
    "  > Settings",
    "  > About",
};


/* ── events ─────────────────────────────────────────────────────────────── */
typedef enum { EvInput, EvTimer } EvKind;
typedef struct { EvKind kind; InputEvent input; } AppEvent;

/* ── decoder ────────────────────────────────────────────────────────────── */
typedef enum { DecIdle, DecCapturing, DecDone } DecState;
typedef struct { uint32_t dur; bool hi; } Pulse;

/* ── app state ──────────────────────────────────────────────────────────── */
typedef struct {
    AppPage page;

    /* home */
    int     home_cur;
    int     home_anim;     /* tick counter for animation */

    /* hold tracking */
    bool     back_held;   uint32_t back_ticks;
    bool     right_held;  uint32_t right_ticks;
    bool     left_held;   uint32_t left_ticks;

    /* settings */
    bool sound_on;
    int  mod_idx;
    bool auto_hop;
    bool bin_raw;         /* binary raw format */
    int  threshold;       /* dBm integer */
    int  hop_speed;       /* 1=slow 2=med 3=fast */
    int  set_cur;

    /* scanner */
    int      freq_idx;
    int      rssi;        /* integer dBm */
    int      peak;
    float    history[HISTORY_LEN];
    int      hist_pos;
    bool     logging;
    uint32_t log_count;
    uint32_t detections;
    uint32_t det_per_freq[FREQ_COUNT];
    int      hop_tick;

    /* spectrum */
    int  spec_rssi[FREQ_COUNT];   /* averaged RSSI per freq */
    int  spec_peak[FREQ_COUNT];   /* peak per freq */
    int  spec_scan_idx;           /* which freq we're currently sampling */
    int  spec_sample_count;
    int  spec_accum;

    /* decoder */
    DecState dec_state;
    Pulse    pulses[CAPTURE_MAX];
    uint16_t pulse_count;
    bool     capture_ready;
    uint32_t te_us;
    char     proto_name[32];
    char     bits[65];
    uint16_t bit_count;
    uint32_t min_pulse;
    uint32_t max_pulse;
    /* raw signal display (ProtoView style) */
    uint8_t  sigmap[110];   /* 0=low 1=high for display */
    int      sigmap_len;

    /* furi */
    FuriMessageQueue* queue;
    ViewPort*         vp;
    Gui*              gui;
    FuriTimer*        timer;
    NotificationApp*  notif;
    Storage*          storage;
    File*             log_file;
} App;

static App* g_app = NULL;

/* ── ISR capture callback ───────────────────────────────────────────────── */
static void capture_cb(bool level, uint32_t duration, void* ctx) {
    UNUSED(ctx);
    App* app = g_app;
    if(!app || app->capture_ready) return;
    if(app->pulse_count < CAPTURE_MAX) {
        app->pulses[app->pulse_count].dur = duration;
        app->pulses[app->pulse_count].hi  = level;
        app->pulse_count++;
        if(!level && duration > 8000 && app->pulse_count > 8)
            app->capture_ready = true;
    } else {
        app->capture_ready = true;
    }
}

/* ── radio ──────────────────────────────────────────────────────────────── */
static void radio_start(App* app) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(MODS[app->mod_idx].regs);
    furi_hal_subghz_set_frequency_and_path(FREQS[app->freq_idx].hz);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();
}
static void radio_stop_plain(void) {
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
}
static void radio_start_async(App* app) {
    furi_hal_subghz_reset();
    furi_hal_subghz_load_custom_preset(MODS[app->mod_idx].regs);
    furi_hal_subghz_set_frequency_and_path(FREQS[app->freq_idx].hz);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_start_async_rx(capture_cb, NULL);
}
static void radio_stop_async(void) {
    furi_hal_subghz_stop_async_rx();
    furi_hal_subghz_sleep();
}

/* ── decoder helpers ────────────────────────────────────────────────────── */
static uint32_t find_te(App* app) {
    if(app->pulse_count < 4) return 0;
    uint16_t buckets[40];
    memset(buckets, 0, sizeof(buckets));
    for(int i = 0; i < app->pulse_count; i++) {
        uint32_t d = app->pulses[i].dur;
        if(d < 2000) {
            int b = (int)(d/50);
            if(b < 40) buckets[b]++;
        }
    }
    int best = 0;
    for(int i = 1; i < 40; i++) if(buckets[i] > buckets[best]) best = i;
    return (uint32_t)(best * 50 + 25);
}

static void build_sigmap(App* app) {
    /* Build a 110-pixel wide visual map of the first burst pulses */
    memset(app->sigmap, 0, sizeof(app->sigmap));
    app->sigmap_len = 0;
    if(app->pulse_count == 0) return;

    /* find total duration of first 80 pulses or until gap */
    uint32_t total = 0;
    int count = 0;
    for(int i = 0; i < app->pulse_count && i < 80; i++) {
        if(!app->pulses[i].hi && app->pulses[i].dur > 10000) break;
        total += app->pulses[i].dur;
        count++;
    }
    if(total == 0 || count == 0) return;

    /* map each pulse to pixel columns */
    float scale = 110.0f / (float)total;
    int col = 0;
    for(int i = 0; i < count && col < 110; i++) {
        int w = (int)((float)app->pulses[i].dur * scale);
        if(w < 1) w = 1;
        uint8_t v = app->pulses[i].hi ? 1 : 0;
        for(int j = 0; j < w && col < 110; j++)
            app->sigmap[col++] = v;
    }
    app->sigmap_len = col;
}

static void decode_protocol(App* app) {
    if(app->pulse_count < 4) {
        snprintf(app->proto_name, sizeof(app->proto_name), "Too few pulses");
        return;
    }
    app->min_pulse = 0xFFFFFFFF; app->max_pulse = 0;
    for(int i = 0; i < app->pulse_count; i++) {
        uint32_t d = app->pulses[i].dur;
        if(d > 0 && d < 5000) {
            if(d < app->min_pulse) app->min_pulse = d;
            if(d > app->max_pulse) app->max_pulse = d;
        }
    }
    app->te_us = find_te(app);
    uint32_t te = app->te_us;

    if(te == 0)        snprintf(app->proto_name, sizeof(app->proto_name), "Unknown");
    else if(te < 150)  snprintf(app->proto_name, sizeof(app->proto_name), "Keeloq/HCS");
    else if(te < 280)  snprintf(app->proto_name, sizeof(app->proto_name), "EV1527/PT2262");
    else if(te < 380)  snprintf(app->proto_name, sizeof(app->proto_name), "CAME/SMC");
    else if(te < 600)  snprintf(app->proto_name, sizeof(app->proto_name), "Nice/Linear");
    else if(te < 1200) snprintf(app->proto_name, sizeof(app->proto_name), "Slow OOK");
    else               snprintf(app->proto_name, sizeof(app->proto_name), "Very slow OOK");

    if(te == 0) { app->bit_count = 0; return; }
    uint32_t te2 = te * 2, tol = te / 2;
    int start = 0;
    for(int i = 0; i < app->pulse_count - 1; i++) {
        if(!app->pulses[i].hi && app->pulses[i].dur > te2 * 3)
            { start = i + 1; break; }
    }
    app->bit_count = 0;
    memset(app->bits, 0, sizeof(app->bits));
    for(int i = start; i + 1 < app->pulse_count && app->bit_count < 64; i += 2) {
        if(!app->pulses[i].hi) continue;
        uint32_t h = app->pulses[i].dur;
        uint32_t l = app->pulses[i+1].dur;
        char bit = '?';
        if(h < te+tol && l > te-tol && l < te2+tol) bit = '0';
        else if(h > te-tol && h < te2+tol && l < te+tol) bit = '1';
        app->bits[app->bit_count++] = bit;
    }
    app->bits[app->bit_count] = '\0';
    build_sigmap(app);
}

static void decoder_reset(App* app) {
    if(app->dec_state != DecIdle) {
        radio_stop_async();
    }
    app->pulse_count   = 0;
    app->capture_ready = false;
    app->dec_state     = DecCapturing;
    memset(app->pulses,     0, sizeof(app->pulses));
    memset(app->proto_name, 0, sizeof(app->proto_name));
    memset(app->bits,       0, sizeof(app->bits));
    memset(app->sigmap,     0, sizeof(app->sigmap));
    app->te_us = app->bit_count = app->min_pulse = app->max_pulse = 0;
    app->sigmap_len = 0;
    radio_start_async(app);
}

/* ── storage ────────────────────────────────────────────────────────────── */
static bool log_open(App* app) {
    app->log_file = storage_file_alloc(app->storage);
    if(!storage_file_open(app->log_file, LOG_FILE, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(app->log_file); app->log_file = NULL; return false;
    }
    if(storage_file_size(app->log_file) == 0) {
        const char* h = "timestamp_ms,freq_hz,freq_label,rssi_dbm\n";
        storage_file_write(app->log_file, h, strlen(h));
    }
    return true;
}
static void log_write(App* app, uint32_t ts, int idx, int rssi) {
    if(!app->log_file) return;
    char ln[80];
    int len = snprintf(ln, sizeof(ln), "%lu,%lu,%s,%d\n",
        (unsigned long)ts, (unsigned long)FREQS[idx].hz,
        FREQS[idx].label, rssi);
    storage_file_write(app->log_file, ln, (size_t)len);
    app->log_count++;
}
static void log_close(App* app) {
    if(app->log_file) {
        storage_file_close(app->log_file);
        storage_file_free(app->log_file);
        app->log_file = NULL;
    }
}

/* ── draw helpers ───────────────────────────────────────────────────────── */
static int rssi_bar(int rssi, int max_h) {
    int v = rssi < -110 ? -110 : rssi > -20 ? -20 : rssi;
    return (v + 110) * max_h / 90;
}

/* ── DRAW: HOME ─────────────────────────────────────────────────────────── */
static void draw_home(Canvas* c, App* app) {
    /* animated top bar - scrolling dots */
    for(int x = (app->home_anim % 20); x < 128; x += 20)
        canvas_draw_dot(c, x, 0);

    /* animated bottom bar - reverse scrolling dots */
    for(int x = 128 - (app->home_anim % 20); x >= 0; x -= 20)
        canvas_draw_dot(c, x, 63);

    /* title with pulse effect */
    canvas_set_font(c, FontPrimary);
    int pulse = (app->home_anim / 5) % 4;
    int title_y = 12 + (pulse > 1 ? 1 : 0);
    canvas_draw_str(c, 24, title_y, "FreqHunter");

    /* animated underline */
    canvas_draw_line(c, 0, 14, 127, 14);
    int anim_x = (app->home_anim * 2) % 128;
    canvas_draw_line(c, anim_x, 15, (anim_x + 20) % 128, 15);

    /* menu */
    canvas_set_font(c, FontSecondary);
    for(int i = 0; i < HOME_N; i++) {
        int y = 22 + i * 9;
        if(i == app->home_cur) {
            /* animated selection highlight */
            int highlight = (app->home_anim / 3) % 2;
            canvas_draw_box(c, 0, y - 8, 128, 10);
            canvas_set_color(c, ColorWhite);
            canvas_draw_str(c, 2 + highlight, y, HOME_LABELS[i]);
            canvas_set_color(c, ColorBlack);
        } else {
            canvas_draw_str(c, 2, y, HOME_LABELS[i]);
        }
    }

}

/* ── DRAW: SCANNER ──────────────────────────────────────────────────────── */
static void draw_scanner(Canvas* c, App* app) {
    char buf[36];

    /* header */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "Scanner");
    canvas_set_font(c, FontSecondary);
    if(app->logging)  canvas_draw_str(c, 68, 9, "[LOG]");
    if(app->auto_hop) canvas_draw_str(c, 100, 9, "[HOP]");

    /* freq row */
    uint32_t hz  = FREQS[app->freq_idx].hz;
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    snprintf(buf, sizeof(buf), "%lu.%03lu MHz  %s",
             (unsigned long)mhz, (unsigned long)khz,
             FREQS[app->freq_idx].label);
    canvas_draw_str(c, 0, 18, buf);
    snprintf(buf, sizeof(buf), "%d/%d", app->freq_idx + 1, FREQ_COUNT);
    canvas_draw_str(c, 104, 18, buf);

    /* RSSI + mod */
    snprintf(buf, sizeof(buf), "RSSI: %d dBm  [%s]",
             app->rssi, MODS[app->mod_idx].name);
    canvas_draw_str(c, 0, 27, buf);

    /* threshold + peak */
    snprintf(buf, sizeof(buf), "T: %d  Peak: %d  Det: %lu",
             app->threshold, app->peak, (unsigned long)app->detections);
    canvas_draw_str(c, 0, 35, buf);

    /* waveform box */
    const int WY0 = 37, WY1 = 56, WH = WY1 - WY0;
    canvas_draw_frame(c, 0, WY0, HISTORY_LEN, WH + 1);

    /* threshold dashed line */
    int ty = WY1 - rssi_bar(app->threshold, WH);
    if(ty < WY0) ty = WY0;
        if(ty > WY1) ty = WY1;
    for(int x = 1; x < HISTORY_LEN - 1; x += 4) {
        canvas_draw_dot(c, x, ty); canvas_draw_dot(c, x+1, ty);
    }

    /* peak hold line */
    int py = WY1 - rssi_bar(app->peak, WH);
    if(py < WY0) py = WY0;
        if(py > WY1) py = WY1;
    canvas_draw_line(c, 1, py, HISTORY_LEN - 2, py);

    /* bars */
    for(int i = 0; i < HISTORY_LEN; i++) {
        int idx = (app->hist_pos + i) % HISTORY_LEN;
        int px = rssi_bar((int)app->history[idx], WH);
        if(px < 0) {
            px = 0;
        }
        if(px > WH) {
            px = WH;
        }
        if(px > 0) canvas_draw_line(c, i, WY1 - px, i, WY1);
    }

    /* status bar */
    snprintf(buf, sizeof(buf), "Log: %lu", (unsigned long)app->log_count);
    canvas_draw_str(c, 0, 64, buf);
    canvas_draw_str(c, 80, 64, app->logging ? "OK=StopLog" : "OK=Log");
}

/* ── DRAW: SPECTRUM ─────────────────────────────────────────────────────── */
static void draw_spectrum(Canvas* c, App* app) {
    /* title */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "Spectrum");
    canvas_set_font(c, FontSecondary);

    /* display current frequency and modulation */
    uint32_t hz = FREQS[app->freq_idx].hz;
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%lu.%03lu MHz  %s", 
             (unsigned long)mhz, (unsigned long)khz, MODS[app->mod_idx].name);
    canvas_draw_str(c, 0, 20, freq_str);
    
    /* bar area: centered single bar */
    const int BY0 = 30, BY1 = 55, BH = BY1 - BY0;
    const int bar_x = 50, bar_w = 30;

    /* draw bar for current frequency */
    int rssi = app->spec_rssi[app->freq_idx];
    int peak = app->spec_peak[app->freq_idx];

    /* bar */
    int bh = rssi_bar(rssi, BH);
    if(bh < 0) {
        bh = 0;
    }
    if(bh > BH) {
        bh = BH;
    }
    if(bh > 0) canvas_draw_box(c, bar_x, BY1 - bh, bar_w, bh);

    /* peak line */
    int ph = rssi_bar(peak, BH);
    if(ph < 0) {
        ph = 0;
    }
    if(ph > BH) {
        ph = BH;
    }
    if(ph > 0) canvas_draw_line(c, bar_x, BY1 - ph, bar_x + bar_w, BY1 - ph);

    /* bar border */
    canvas_draw_frame(c, bar_x - 1, BY0 - 1, bar_w + 2, BH + 2);

    /* RSSI value display */
    char rssi_str[16];
    snprintf(rssi_str, sizeof(rssi_str), "RSSI: %d dBm", rssi);
    canvas_draw_str(c, 0, 64, rssi_str);
}

/* ── DRAW: DECODER ──────────────────────────────────────────────────────── */
static void draw_decoder(Canvas* c, App* app) {
    char buf[48];

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "Decoder");
    canvas_set_font(c, FontSecondary);

    uint32_t hz  = FREQS[app->freq_idx].hz;
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    snprintf(buf, sizeof(buf), "%lu.%03lu  %s  %s",
             (unsigned long)mhz, (unsigned long)khz,
             FREQS[app->freq_idx].label, MODS[app->mod_idx].name);
    canvas_draw_str(c, 0, 18, buf);
    canvas_draw_line(c, 0, 20, 127, 20);

    if(app->dec_state == DecCapturing) {
        /* listening UI */
        canvas_draw_str(c, 0, 30, "Listening...");
        snprintf(buf, sizeof(buf), "Edges: %d", app->pulse_count);
        canvas_draw_str(c, 0, 39, buf);
        /* progress bar */
        int prog = app->pulse_count * 110 / CAPTURE_MAX;
        canvas_draw_frame(c, 0, 42, 110, 6);
        if(prog > 0) canvas_draw_box(c, 1, 43, prog, 4);
        canvas_draw_str(c, 0, 55, "Aim transmitter at Flipper");
        canvas_draw_str(c, 0, 63, "OK=Reset");

    } else if(app->dec_state == DecDone) {
        /* raw signal display (ProtoView style) */
        if(app->sigmap_len > 0) {
            const int SY = 30, SH = 8;
            canvas_draw_frame(c, 0, SY - 1, 110, SH + 2);
            bool in_hi = false;
            int run_start = 0;
            for(int x = 0; x <= app->sigmap_len; x++) {
                uint8_t cur = (x < app->sigmap_len) ? app->sigmap[x] : 0;
                if(x == 0) { in_hi = (cur == 1); run_start = 0; continue; }
                bool new_hi = (cur == 1);
                if(new_hi != in_hi || x == app->sigmap_len) {
                    if(in_hi)
                        canvas_draw_box(c, run_start, SY, x - run_start, SH);
                    in_hi = new_hi;
                    run_start = x;
                }
            }
        }

        /* results */
        snprintf(buf, sizeof(buf), "%d pulses  Te:%luus",
                 app->pulse_count, (unsigned long)app->te_us);
        canvas_draw_str(c, 0, 42, buf);

        snprintf(buf, sizeof(buf), "%s", app->proto_name);
        canvas_draw_str(c, 0, 51, buf);

        if(app->bit_count > 0) {
            char row[22]; strncpy(row, app->bits, 21); row[21] = '\0';
            canvas_draw_str(c, 0, 60, row);
        } else {
            canvas_draw_str(c, 0, 60, "No bits decoded");
        }
        canvas_draw_str(c, 80, 64, "OK=Clear");
    }
}

static const char* SET_LABELS[] = {
    "  Sound",
    "  Modulation",
    "  Frequency",
    "  Auto-Hop",
    "  Bin Raw",
};
/* ── DRAW: SETTINGS ─────────────────────────────────────────────────────── */
static void draw_settings(Canvas* c, App* app) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 9, "Settings");
    canvas_draw_line(c, 0, 11, 127, 11);
    canvas_set_font(c, FontSecondary);

    const char* set_vals[SET_N];
    char freq_buf[16];
    uint32_t hz = FREQS[app->freq_idx].hz;
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    snprintf(freq_buf, sizeof(freq_buf), "%lu.%03lu MHz", (unsigned long)mhz, (unsigned long)khz);
    set_vals[0] = app->sound_on ? "ON " : "OFF";
    set_vals[1] = MODS[app->mod_idx].name;
    set_vals[2] = freq_buf;
    set_vals[3] = app->auto_hop ? "ON " : "OFF";
    set_vals[4] = app->bin_raw ? "ON " : "OFF";

    for(int i = 0; i < SET_N; i++) {
        int y = 20 + i * 9;
        if(i == app->set_cur) {
            canvas_draw_box(c, 0, y - 8, 128, 10);
            canvas_set_color(c, ColorWhite);
        }
        canvas_draw_str(c, 2, y, SET_LABELS[i]);
        canvas_draw_str(c, 90, y, set_vals[i]);
        canvas_set_color(c, ColorBlack);
    }
}

/* ── DRAW: ABOUT ────────────────────────────────────────────────────────── */
static void draw_about(Canvas* c) {
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 22, 10, "FreqHunter v4.0");
    canvas_draw_line(c, 0, 12, 127, 12);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 22, "By: Smoodiehacking");
    canvas_draw_str(c, 0, 31, "Scanner + Spectrum + Decoder");
    canvas_draw_line(c, 0, 33, 127, 33);
    canvas_draw_str(c, 0, 42, "GitHub:");
    canvas_draw_str(c, 0, 50, "OscarLauen/FreqHunter");
    canvas_draw_str(c, 0, 58, "https://github.com");
    canvas_draw_line(c, 0, 60, 127, 60);
}

/* ── main draw ──────────────────────────────────────────────────────────── */
static void draw_cb(Canvas* c, void* ctx) {
    App* app = (App*)ctx;
    canvas_clear(c);
    switch(app->page) {
    case PageHome:     draw_home(c, app);     break;
    case PageScanner:  draw_scanner(c, app);  break;
    case PageSpectrum: draw_spectrum(c, app); break;
    case PageDecoder:  draw_decoder(c, app);  break;
    case PageSettings: draw_settings(c, app); break;
    case PageAbout:    draw_about(c);         break;
    }
}

/* ── event callbacks ────────────────────────────────────────────────────── */
static void input_cb(InputEvent* ev, void* ctx) {
    App* app = (App*)ctx;
    AppEvent e = { .kind = EvInput, .input = *ev };
    furi_message_queue_put(app->queue, &e, FuriWaitForever);
}
static void timer_cb(void* ctx) {
    App* app = (App*)ctx;
    AppEvent e = { .kind = EvTimer };
    furi_message_queue_put(app->queue, &e, 0);
}

/* ── navigation helper ──────────────────────────────────────────────────── */
static void go_home(App* app) {
    if(app->page == PageScanner)  radio_stop_plain();
    if(app->page == PageSpectrum) radio_stop_plain();
    if(app->page == PageDecoder)  radio_stop_async();
    if(app->logging) { log_close(app); app->logging = false; }
    app->page = PageHome;
}

/* ── entry point ────────────────────────────────────────────────────────── */
int32_t freqhunter_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "FreqHunter v4.0 start");

    App* app = malloc(sizeof(App));
    furi_check(app);
    memset(app, 0, sizeof(App));
    g_app = app;

    /* defaults */
    app->page       = PageHome;
    app->home_cur   = 0;
    app->freq_idx   = 5;    /* 433.92 MHz */
    app->threshold  = THR_DEFAULT;
    app->rssi       = -100;
    app->peak       = -110;
    app->sound_on   = true;
    app->mod_idx    = 0;
    app->auto_hop   = false;
    app->bin_raw    = false;
    app->hop_speed  = 2;
    app->dec_state  = DecIdle;
    for(int i = 0; i < HISTORY_LEN; i++) app->history[i] = -110.0f;
    for(int i = 0; i < FREQ_COUNT;  i++) {
        app->spec_rssi[i] = -110;
        app->spec_peak[i] = -110;
    }

    app->queue   = furi_message_queue_alloc(16, sizeof(AppEvent));
    app->vp      = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    app->gui     = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);
    app->notif   = furi_record_open(RECORD_NOTIFICATION);
    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(app->storage, LOG_DIR);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(DWELL_MS));

    bool run = true;
    AppEvent ev;

    while(run) {
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk)
            continue;

        /* ── TIMER ──────────────────────────────────────────────────── */
        if(ev.kind == EvTimer) {
            app->home_anim++;

            /* back hold to exit */
            if(app->back_held) {
                app->back_ticks++;
                if(app->back_ticks >= HOLD_2S) { run = false; continue; }
            }

            /* hold right: scanner → decoder */
            if(app->right_held && app->page == PageScanner) {
                app->right_ticks++;
                if(app->right_ticks >= HOLD_3S) {
                    app->right_held = false; app->right_ticks = 0;
                    radio_stop_plain();
                    app->page = PageDecoder;
                    decoder_reset(app);
                    if(app->sound_on)
                        notification_message(app->notif, &sequence_blink_blue_10);
                }
            }

            /* hold left: decoder → scanner */
            if(app->left_held && app->page == PageDecoder) {
                app->left_ticks++;
                if(app->left_ticks >= HOLD_3S) {
                    app->left_held = false; app->left_ticks = 0;
                    radio_stop_async();
                    app->page = PageScanner;
                    radio_start(app);
                    if(app->sound_on)
                        notification_message(app->notif, &sequence_blink_blue_10);
                }
            }

            /* scanner tick */
            if(app->page == PageScanner) {
                float rssi_f = furi_hal_subghz_get_rssi();
                app->rssi = (int)rssi_f;
                app->history[app->hist_pos] = rssi_f;
                app->hist_pos = (app->hist_pos + 1) % HISTORY_LEN;

                if(app->rssi > app->peak) app->peak = app->rssi;
                else if(app->peak > -110) app->peak--;

                if(app->rssi > app->threshold) {
                    app->detections++;
                    app->det_per_freq[app->freq_idx]++;
                    if(app->sound_on)
                        notification_message(app->notif, &sequence_success);
                    if(app->logging) {
                        uint32_t ts = furi_get_tick() /
                            (furi_kernel_get_tick_frequency() / 1000);
                        log_write(app, ts, app->freq_idx, app->rssi);
                    }
                }

                /* auto hop */
                if(app->auto_hop) {
                    int hop_ticks = (app->hop_speed == 1) ? 8 :
                                    (app->hop_speed == 3) ? 2 : 4;
                    app->hop_tick++;
                    if(app->hop_tick >= hop_ticks) {
                        app->hop_tick = 0;
                        app->freq_idx = (app->freq_idx + 1) % FREQ_COUNT;
                        app->peak = -110;
                        radio_stop_plain();
                        radio_start(app);
                    }
                }
            }

            /* spectrum tick - scan one freq per tick */
            if(app->page == PageSpectrum) {
                float rssi_f = furi_hal_subghz_get_rssi();
                int rssi = (int)rssi_f;
                app->spec_accum += rssi;
                app->spec_sample_count++;

                if(app->spec_sample_count >= SPEC_SAMPLES) {
                    int avg = app->spec_accum / app->spec_sample_count;
                    app->spec_rssi[app->spec_scan_idx] = avg;
                    if(avg > app->spec_peak[app->spec_scan_idx])
                        app->spec_peak[app->spec_scan_idx] = avg;
                    app->spec_accum = 0;
                    app->spec_sample_count = 0;
                    app->spec_scan_idx = (app->spec_scan_idx + 1) % FREQ_COUNT;
                    /* tune to next freq */
                    radio_stop_plain();
                    furi_hal_subghz_reset();
                    furi_hal_subghz_load_custom_preset(MODS[app->mod_idx].regs);
                    furi_hal_subghz_set_frequency_and_path(
                        FREQS[app->spec_scan_idx].hz);
                    furi_hal_subghz_flush_rx();
                    furi_hal_subghz_rx();
                }
            }

            /* decoder tick */
            if(app->page == PageDecoder) {
                if(app->capture_ready && app->dec_state == DecCapturing) {
                    furi_hal_subghz_stop_async_rx();
                    app->dec_state = DecDone;
                    decode_protocol(app);
                    if(app->sound_on)
                        notification_message(app->notif, &sequence_success);
                }
            }

            view_port_update(app->vp);
        }

        /* ── INPUT ──────────────────────────────────────────────────── */
        else if(ev.kind == EvInput) {

            /* track hold keys */
            if(ev.input.key == InputKeyBack) {
                if(ev.input.type == InputTypePress)
                    { app->back_held = true;  app->back_ticks = 0; }
                else if(ev.input.type == InputTypeRelease)
                    { app->back_held = false; app->back_ticks = 0; }
            }
            if(ev.input.key == InputKeyRight) {
                if(ev.input.type == InputTypePress)
                    { app->right_held = true;  app->right_ticks = 0; }
                else if(ev.input.type == InputTypeRelease)
                    { app->right_held = false; app->right_ticks = 0; }
            }
            if(ev.input.key == InputKeyLeft) {
                if(ev.input.type == InputTypePress)
                    { app->left_held = true;  app->left_ticks = 0; }
                else if(ev.input.type == InputTypeRelease)
                    { app->left_held = false; app->left_ticks = 0; }
            }

            /* short press: back = home */
            if(ev.input.type == InputTypeShort &&
               ev.input.key == InputKeyBack) {
                if(app->page != PageHome) go_home(app);
            }

            /* short press: page-specific */
            if(ev.input.type == InputTypeShort &&
               ev.input.key != InputKeyBack) {

                /* HOME */
                if(app->page == PageHome) {
                    if(ev.input.key == InputKeyUp)
                        app->home_cur = (app->home_cur - 1 + HOME_N) % HOME_N;
                    else if(ev.input.key == InputKeyDown)
                        app->home_cur = (app->home_cur + 1) % HOME_N;
                    else if(ev.input.key == InputKeyOk) {
                        switch(app->home_cur) {
                        case 0: /* Scanner */
                            app->page = PageScanner;
                            radio_start(app);
                            break;
                        case 1: /* Spectrum */
                            app->page = PageSpectrum;
                            app->spec_scan_idx = 0;
                            app->spec_sample_count = 0;
                            app->spec_accum = 0;
                            /* reset peaks */
                            for(int i = 0; i < FREQ_COUNT; i++)
                                app->spec_peak[i] = -110;
                            /* tune to first freq */
                            furi_hal_subghz_reset();
                            furi_hal_subghz_load_custom_preset(
                                MODS[app->mod_idx].regs);
                            furi_hal_subghz_set_frequency_and_path(
                                FREQS[0].hz);
                            furi_hal_subghz_flush_rx();
                            furi_hal_subghz_rx();
                            break;
                        case 2: /* Decoder */
                            app->page = PageDecoder;
                            decoder_reset(app);
                            break;
                        case 3: /* Settings */
                            app->page = PageSettings;
                            app->set_cur = 0;
                            break;
                        case 4: /* About */
                            app->page = PageAbout;
                            break;
                        }
                    }
                }

                /* SCANNER */
                else if(app->page == PageScanner) {
                    switch(ev.input.key) {
                    case InputKeyUp:
                        app->freq_idx = (app->freq_idx-1+FREQ_COUNT) % FREQ_COUNT;
                        app->peak = -110;
                        radio_stop_plain(); radio_start(app);
                        break;
                    case InputKeyDown:
                        app->freq_idx = (app->freq_idx+1) % FREQ_COUNT;
                        app->peak = -110;
                        radio_stop_plain(); radio_start(app);
                        break;
                    case InputKeyLeft:
                        if(app->threshold > THR_MIN) app->threshold -= THR_STEP;
                        break;
                    case InputKeyRight:
                        if(app->threshold < THR_MAX) app->threshold += THR_STEP;
                        break;
                    case InputKeyOk:
                        if(!app->logging) {
                            if(log_open(app)) {
                                app->logging = true;
                                if(app->sound_on)
                                    notification_message(app->notif,
                                        &sequence_blink_blue_10);
                            }
                        } else {
                            log_close(app); app->logging = false;
                            if(app->sound_on)
                                notification_message(app->notif,
                                    &sequence_blink_red_10);
                        }
                        break;
                    default: break;
                    }
                }

                /* SPECTRUM */
                else if(app->page == PageSpectrum) {
                    if(ev.input.key == InputKeyOk) {
                        /* reset all peaks */
                        for(int i = 0; i < FREQ_COUNT; i++)
                            app->spec_peak[i] = -110;
                        if(app->sound_on)
                            notification_message(app->notif, &sequence_success);
                    }
                }

                /* DECODER */
                else if(app->page == PageDecoder) {
                    if(ev.input.key == InputKeyOk) decoder_reset(app);
                    else if(ev.input.key == InputKeyUp || ev.input.key == InputKeyDown) {
                        /* change freq in decoder */
                        if(ev.input.key == InputKeyUp)
                            app->freq_idx = (app->freq_idx-1+FREQ_COUNT) % FREQ_COUNT;
                        else
                            app->freq_idx = (app->freq_idx+1) % FREQ_COUNT;
                        decoder_reset(app);
                    }
                }

                /* SETTINGS */
                else if(app->page == PageSettings) {
                    switch(ev.input.key) {
                    case InputKeyUp:
                        app->set_cur = (app->set_cur - 1 + SET_N) % SET_N;
                        break;
                    case InputKeyDown:
                        app->set_cur = (app->set_cur + 1) % SET_N;
                        break;
                    case InputKeyOk:
                        if(app->set_cur == 0) app->sound_on = !app->sound_on;
                        if(app->set_cur == 1) app->mod_idx = (app->mod_idx+1) % MOD_COUNT;
                        if(app->set_cur == 2) app->freq_idx = (app->freq_idx+1) % FREQ_COUNT;
                        if(app->set_cur == 3) app->auto_hop = !app->auto_hop;
                        if(app->set_cur == 4) app->bin_raw = !app->bin_raw;
                        break;
                    case InputKeyLeft:
                        if(app->set_cur == 1)
                            app->mod_idx = (app->mod_idx-1+MOD_COUNT) % MOD_COUNT;
                        if(app->set_cur == 2)
                            app->freq_idx = (app->freq_idx-1+FREQ_COUNT) % FREQ_COUNT;
                        break;
                    case InputKeyRight:
                        if(app->set_cur == 1)
                            app->mod_idx = (app->mod_idx+1) % MOD_COUNT;
                        if(app->set_cur == 2)
                            app->freq_idx = (app->freq_idx+1) % FREQ_COUNT;
                        break;
                    default: break;
                    }
                }
            }

            /* long OK on scanner = toggle auto-hop */
            if(ev.input.type == InputTypeLong &&
               ev.input.key == InputKeyOk &&
               app->page == PageScanner) {
                app->auto_hop = !app->auto_hop;
                app->hop_tick = 0; app->peak = -110;
                if(app->sound_on)
                    notification_message(app->notif, &sequence_blink_blue_10);
            }
        }
    }

    /* cleanup */
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    if(app->page == PageDecoder) radio_stop_async();
    else radio_stop_plain();
    if(app->logging) log_close(app);
    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_message_queue_free(app->queue);
    g_app = NULL;
    free(app);
    FURI_LOG_I(TAG, "FreqHunter v4.0 exit");
    return 0;
}
