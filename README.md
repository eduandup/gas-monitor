# Gas Monitor

Shows the gas level of two 48 kg LPG cylinders from anywhere.

```
EyeGas Pro / Mopeka sensors --Bluetooth--> ESP32-S3 --HTTPS every 5 min--> Supabase database
                                              |                                  |
                                   http://gas.local (backup,             GitHub Pages website
                                   works without internet)               (reads the database)
```

- **Sensors:** EyeGas Pro (a rebranded Mopeka Pro Check LPG). They only send Bluetooth advertisements.
- **ESP32-S3:** listens to both sensors. It serves a local page at `http://gas.local` and sends a reading per tank to Supabase every 5 minutes. If the internet is down it keeps up to 24 h of readings in memory and sends them when the connection comes back.
- **Supabase:** a free online Postgres database that stores the readings.
- **Website:** `web/` is published by GitHub Pages. It reads the database and works out %, kg, usage rate and the empty date in the browser.

## Folders

| Path | What it is |
|---|---|
| `firmware/gas_monitor/` | The Arduino sketch. Open `gas_monitor.ino` in the Arduino IDE. |
| `firmware/gas_monitor/secrets.h` | Your Wi-Fi password and keys. **Not in git.** Copy it from `secrets.example.h`. |
| `firmware/local_page.html` | The page served at `http://gas.local`. Run `python tools/build_page.py` after editing it. |
| `firmware/mopeka_test/` | A simple serial scanner, used for the first sensor test. |
| `web/index.html`, `web/config.js` | The online dashboard and its settings (tank names, calibration, Supabase key). |
| `supabase/schema.sql` | Everything needed to create the database from scratch. |
| `tools/` | Helper scripts (see below). |

## Flashing the ESP32

Arduino IDE, board **ESP32S3 Dev Module**, port **COM5**, and in the Tools menu:

- **USB CDC On Boot: Enabled**. Without it the Serial Monitor stays empty.
- **Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**. It keeps room for a second copy of the program, which updates over Wi-Fi need.

Open `firmware/gas_monitor/gas_monitor.ino` and click Upload. Set the Serial Monitor to 115200 baud. At start-up it prints why it last restarted. Every 30 s it prints the sensor readings, the Wi-Fi signal, the free memory ("heap"), the chip temperature, the uptime, and how many rows are waiting to upload. Every 5 minutes it also prints `uploaded N rows`.

### Updating over Wi-Fi (once it's mounted outside)

The laptop must be on the same Wi-Fi as the hub. In the Arduino IDE pick **Tools → Port → gas at 192.168.x.x** (a network port instead of COM5) and click Upload. It asks for the password: `OTA_PASSWORD` in `secrets.h`. It takes about a minute, then the hub restarts by itself. The Serial Monitor only works over USB.

### When it's left alone

The hub repairs itself: it reconnects Wi-Fi, a watchdog resets it if anything hangs for 2 minutes, it restarts when Wi-Fi is gone for 30 minutes, Bluetooth goes silent for 30 minutes, nothing uploads for 3 hours or memory runs low, and it restarts every night at 03:00. Unsent readings are saved to flash first and sent after the restart. The website header shows its Wi-Fi signal (hover for uptime and chip temperature) and warns when the hub runs hot.

From the command line, the Arduino IDE ships its own `arduino-cli`:

```bash
"$LOCALAPPDATA/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" compile --upload -p COM5 -b "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=min_spiffs" firmware/gas_monitor
```

## Setting up Supabase (once)

1. Create a project at [supabase.com](https://supabase.com) (sign in with GitHub). Pick the region closest to South Africa.
2. Go to **SQL Editor → New query**, paste all of `supabase/schema.sql`, and click **Run**.
3. Then run `supabase/add_device_key.local.sql` the same way. It registers the ESP32's secret key (only a fingerprint of it is stored). This file is not in git. If it's missing, make a new key as described in `secrets.example.h` and insert its SHA-256 hash.
4. Copy the **Project URL** and the **anon** (or **publishable**) key from **Project Settings → API**:
   - into `firmware/gas_monitor/secrets.h` (`SUPABASE_URL`, `SUPABASE_ANON_KEY`), then re-flash the board
   - into `web/config.js` (`supabaseUrl`, `supabaseKey`), then commit and push
5. Check it: `python tools/check_supabase.py`. It tests that the website can read, that nobody can write without the device key, and that the ESP32's built-in certificates match Supabase's.

**Never** put the `service_role` / secret key anywhere in this repo. The anon/publishable key is fine to publish, because the database only lets it read.

## Everyday changes

### Recalibrating a tank

The sensor measures liquid height in mm, and the database stores millimetres, so calibration only changes how that height is turned into %.

1. Right after a refill, read the **Liquid height** on the website (or `http://gas.local`).
2. Put that number in `fullMm` for that tank in **`web/config.js`**. Commit and push. The website and all of its history update within a minute.
3. Optionally put the same number in `TANKS[]` in `gas_monitor.ino` and re-flash, so the local page agrees.

`emptyMm` is the height when a cylinder runs out. 20 mm is a guess. Adjust it the next time a cylinder runs dry.

### Changing tank names

Edit `name` in `web/config.js` (website) and in `TANKS[]` in the sketch (local page).

### Adding or swapping a sensor

Put the sensor's MAC in `TANKS[]` in the sketch and re-flash. The website doesn't care about MACs; it only shows them.

## If something goes wrong

- **The website says "Hub offline since …".** The ESP32 hasn't uploaded for 20+ minutes. Check its power and Wi-Fi. Readings it queued while offline (up to 24 h) appear once it reconnects.
- **The website says "Demo data: couldn't reach the database".** Supabase is down or **paused**. Free projects pause after a week without activity (the ESP32's uploads should prevent that). To fix it, log in to supabase.com, open the project and click **Restore**. Your data is kept.
- **The Serial Monitor shows `upload: Supabase replied 403 … unauthorized`.** The `DEVICE_KEY` in `secrets.h` doesn't match the one registered in `device_keys`. Run `add_device_key.local.sql` again.
- **The Serial Monitor shows an SSL/handshake error.** Supabase may have changed certificate authority. Run `python tools/check_supabase.py`. If the TLS check fails, add the new root to `ROOTS` in `tools/make_ca_certs.py`, run it, and re-flash.
- **No internet at all:** `http://gas.local` still works on your home Wi-Fi.

## Tools

| Script | Use |
|---|---|
| `tools/build_page.py` | Rebuilds `firmware/gas_monitor/page.h` from `firmware/local_page.html`. |
| `tools/make_ca_certs.py` | Rebuilds `ca_certs.h` (the root certificates the ESP32 trusts). Needs `pip install certifi`. |
| `tools/check_supabase.py` | Tests the Supabase setup using the values in `secrets.h`. |
