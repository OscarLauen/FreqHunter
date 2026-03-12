# FreqHunter 🎯
### A Sub-GHz RF Signal Logger for Flipper Zero

FreqHunter scans Sub-GHz frequencies (300–928 MHz), displays a **live RSSI
waveform** on the Flipper's 128×64 screen, and logs every signal that exceeds
your threshold to a **CSV file** on the SD card.

---

## Features

| Feature | Detail |
|---|---|
| Frequency list | 10 ISM-band presets (315, 345, 390, 418, 433.92, 434.42, 868.35, 915 MHz…) |
| Live waveform | 110-sample scrolling RSSI graph on the display |
| Threshold line | Dashed line on the graph, adjustable in 5 dBm steps |
| CSV logging | `timestamp_ms, frequency_hz, rssi_dbm` — appended across sessions |
| LED feedback | Green blink on detection · Blue = logging started · Red = logging stopped |
| Counters | Live detection count and log entry count on screen |

---

## Screen Layout

```
FreqHunter                    [REC]
433.920 MHz                   6/10
RSSI: -72.5 dBm   Thr: -85 dBm
┌──────────────────────────────────┐
│     ▄▄  ▄                        │  ← live waveform
│  - - - - - - - - - - - - - - - - │  ← threshold line
│▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄│
└──────────────────────────────────┘
Log:47  Det:12              OK=Stop
```

---

## Controls

| Button | Action |
|---|---|
| **Up / Down** | Change frequency |
| **Left** | Lower threshold by 5 dBm |
| **Right** | Raise threshold by 5 dBm |
| **OK** | Start / stop logging to SD card |
| **Back** | Exit |

---

## Log File

```
/ext/apps_data/freqhunter/log.csv
```

CSV format:
```
timestamp_ms,frequency_hz,rssi_dbm
12345,433920000,-72.5
12427,433920000,-68.1
```

Open in Excel, Python/pandas, or any CSV viewer.

---

## Build & Install

### Prerequisites
```bash
brew install pipx
pipx install ufbt
pipx ensurepath
```

### Build
```bash
cd FreqHunter
ufbt
```
Output: `dist/freqhunter.fap`

### Deploy to Flipper (USB)
Close qFlipper first, then:
```bash
ufbt launch
```

### Manual install
Copy `dist/freqhunter.fap` to your Flipper SD card:
```
/ext/apps/Tools/freqhunter.fap
```
Find it under **Applications → Tools → FreqHunter**.

---

## Frequency Hopping

By default FreqHunter stays on one frequency so you can focus.
To enable automatic sweeping, uncomment this block in `freqhunter.c`:

```c
app->freq_index = (app->freq_index + 1) % FREQ_COUNT;
radio_start_rx(app);
```

---

## Adding Custom Frequencies

Edit `FREQ_LIST` in `freqhunter.c`:

```c
static const uint32_t FREQ_LIST[] = {
    300000000,   /* 300.0 MHz */
    433920000,   /* 433.92 MHz */
    // add yours here in Hz
};
```

Valid range: **300 000 000 – 928 000 000 Hz**

---

## Author

Made by oscarlauenbach
