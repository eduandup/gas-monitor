# Gas Monitor: notes for Claude

Hobby project: two Mopeka Pro Check LPG sensors (sold as EyeGas Pro) → ESP32-S3 over BLE → Supabase every 5 min → GitHub Pages dashboard. The ESP32 also serves a local backup page at `http://gas.local`. See README.md for setup and everyday changes.

The owner is comfortable with hardware but new to Supabase and GitHub, so explain those steps in plain words. Timezone is Africa/Johannesburg (UTC+2).

## Build

- Arduino IDE, board "ESP32S3 Dev Module", COM5, arduino-esp32 core 3.3.x, no PSRAM, 4 MB flash.
- **USB CDC On Boot: Enabled** and **Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)** are both required. The OTA scheme is what lets the outdoor hub be updated over Wi-Fi (ArduinoOTA, password `OTA_PASSWORD` in secrets.h). Keep the sketch under 1.9 MB.
- There's no arduino-cli on PATH. Use the one bundled with the IDE:
  `"$LOCALAPPDATA/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" compile -b "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" firmware/gas_monitor`
- `page.h` is generated from `firmware/local_page.html` (`python tools/build_page.py`), and `ca_certs.h` by `tools/make_ca_certs.py`. Commit both generated files.
- `secrets.h` is git-ignored. Never print its contents or commit it. The repo is public.

## Rules learned the hard way

- The Arduino IDE auto-inserts function prototypes above the first function. Any struct used in a function signature must be defined before the first function (see `Stats`, `Row`).
- Core 3.x: `getManufacturerData()` returns `String`. Use `auto` and `.c_str()`.
- Wi-Fi and BLE share the radio. Keep the scan window around 50% (interval 160, window 80).
- `String += int8_t` appends a character, not a number. Wrap it in `String((int)x)`.
- iCloud Drive sometimes renames files to `name 2.ext` (it hit `.gitignore` and `web/index.html`). Check `git status` for a deleted file plus a `… 2.…` copy before committing.

## Mopeka decoding (works, don't change without reason)

Manufacturer data is 12 bytes: `0x59 0x00` followed by `d[0..9]`.

| Field | Decoding |
|---|---|
| Hardware ID | `d[0]&0x7F` = 3 |
| Battery (V) | `(d[1]&0x7F)/32` |
| Temperature (°C) | `rawTemp = d[2]&0x7F`, °C = rawTemp − 40 |
| Raw level | `((d[4]<<8)\|d[3]) & 0x3FFF` |
| Quality | `d[4]>>6` |

Level in mm: `raw*(0.573045 - 0.002822*rawTemp - 0.00000535*rawTemp²)`. Only readings with quality ≥ 2 are trusted.

## Data flow

- **Firmware:** every 5 minutes (`closeUploadWindow`) it takes the median of that window's quality ≥ 2 reads and puts one row per tank in a RAM queue (576 rows = 24 h). `uploadTask` (its own FreeRTOS task, 12 KB stack) POSTs batches of 36 to `/rest/v1/rpc/ingest` with the device key and validates TLS against `ca_certs.h` (ISRG X1/X2, GTS R1/R4). It backs off from 30 s up to 10 min. A separate 15-minute median goes into the 21-day LittleFS history used by the local page. `healthCheck()` makes it self-healing (Wi-Fi redial, task watchdog, planned restarts, nightly restart at 03:00); unsent rows are saved to `/queue.bin` before restarts. Each row also carries `wifi_rssi`, `hub_temp_c` and `uptime_min`.
- **Database (`supabase/schema.sql`):** `readings` is unique on (tank, ts), so retries don't duplicate. anon gets SELECT only. Writes go through the `ingest(p_key, p_rows)` security-definer function, which checks the key's sha256 against `device_keys`. Reads use the `latest` view (newest row per tank plus the last good level) and `history(p_hours, p_bucket_min)`.
- **Website (`web/`):** plain JS, no build step. The calibration in `config.js` turns mm into %, so recalibrating updates the history too. Stats are a port of the sketch's `computeStats()` (72 h regression, refill = rise of more than 8 pp, rate ≥ 0.1 kg/day counts as "In use"). It shows "Hub offline" after 20 minutes without an upload, and demo data only when the database can't be reached.

## Tanks

| Tank | MAC | Notes |
|---|---|---|
| 1 | `f1:81:73:69:2c:5b` | on a cylinder, weak signal (about −84 dBm) |
| 2 | `e9:e1:cf:08:46:7c` | not mounted yet |

`fullMm = 900`, `emptyMm = 20` and `diameterMm = 365` are uncalibrated estimates.
