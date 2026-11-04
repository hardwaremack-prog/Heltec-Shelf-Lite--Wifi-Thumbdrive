<p align="center"><img src="docs/icon.png" width="96" alt=""></p>

# Heltec Shelf Lite

**The Shelf, shrunk onto a Heltec WiFi LoRa 32 V3 or V4.** Plug a microSD card into a $3 module and the board becomes a little Wi-Fi file hub for your home. Open it in a browser on any phone or PC to upload, browse and download files, play your music, and pin notes and links for the family.

- 📁 **Files**: upload from an iPhone or PC, make folders, rename, delete and download. Photos, videos and text files open right in the page.
- 🎵 **Music**: folders become albums. Stream to your phone with lock-screen controls, or play out a MAX98357A speaker wired to the board.
- 📌 **Board**: a shared wall of notes and links.
- 📟 **OLED**: shows the board's address, free space and the song that's playing.
- 📱 **iPhone-friendly**: Add to Home Screen and it opens like an app.

### ➡️ Install from your browser: **https://hardwaremack-prog.github.io/Heltec-Shelf-Lite/**

> Installing Shelf Lite replaces Meshtastic on that board. You can put Meshtastic back any time from [flasher.meshtastic.org](https://flasher.meshtastic.org).

---

## What you need

| Part | Notes |
|---|---|
| Heltec WiFi LoRa 32 **V3** or **V4** | Both V4 versions (2 MB and 8 MB PSRAM) work |
| 3.3 V SPI microSD module | e.g. the 6-pin "Micro SD TF Card Adapter Mini Reader Module 3.3V". **Skip the 5 V ones with a regulator chip** |
| microSD card | **32 GB recommended**: it comes FAT32, ready to use. Bigger cards work after reformatting to FAT32 |
| 6 jumper wires | Female-female if your board has header pins |
| *Optional:* MAX98357A I2S amp + 4–8 Ω 3 W speaker | For playing music out loud from the board |

No card? Shelf Lite still runs and keeps files in the board's own flash: about **5.9 MB on a V3** and **13.9 MB on a V4**.

## Wiring

One wiring plan works on the V3 and the V4. The pins avoid the LoRa radio (GPIO 8–14), the OLED (17, 18, 21), USB (19, 20), the V4's high-power LoRa amplifier controls (2, 5, 7), the V4-R8's PSRAM (33–37) and the boot pins.

### microSD module

| Module pin | Heltec pin |
|---|---|
| VCC | **3V3** (not Ve, which the firmware can switch off) |
| GND | GND |
| CS | GPIO 4 |
| MISO | GPIO 6 |
| SCK | GPIO 47 |
| MOSI | GPIO 48 |

### MAX98357A speaker amp (optional)

| Amp pin | Heltec pin |
|---|---|
| VIN | 3V3, or 5V for louder (when on USB power) |
| GND | GND |
| BCLK | GPIO 39 |
| LRC | GPIO 41 |
| DIN | GPIO 38 |
| + / − | speaker |

**Go by the labels printed on the board.** GPIO 47 and 48 are on the *other* header row from 4, 6, 38, 39 and 41. Keep the SD wires short (under 10 cm). If the card isn't found, the board retries at a slower speed, then falls back to its own flash. The screen shows **SD** or **FLASH** in the top-right corner.

> **V4 note:** GPIO 38, 39 and 41 are also wired to the V4's GNSS (GPS) connector. That's fine as long as no GPS module is plugged in there.

## First-time setup

1. **Install** with the [web installer](https://hardwaremack-prog.github.io/Heltec-Shelf-Lite/) (Chrome or Edge on a PC, Mac or Pi).
2. Power the board. The screen shows **Join Shelf-Lite-XXXX**.
3. On your phone, join that network. The password is **`shelflite`**. A setup page pops up; if it doesn't, open `http://192.168.4.1`.
4. Go to **Device → Find networks**, tap your home Wi-Fi and enter its password. The board restarts.
5. Put your phone back on your home Wi-Fi. Open the address shown on the board's screen, or **`http://shelf-xxxx.local`**.
6. iPhone: Share → **Add to Home Screen**.

You can rename the board under **Device → Name on the network**, for example `shelf-kitchen`. Give each board its own name if you run more than one.

### The PRG button

| Press | What happens |
|---|---|
| Short press | Next song on the speaker, or wakes the screen |
| Hold 6 seconds | Forgets the Wi-Fi and restarts in setup mode |

## Music

- Put songs in folders (`/Music/Road Trip/…`). Each folder shows up as an album.
- **This device** streams to the phone or PC you're using. MP3, M4A/AAC, WAV and FLAC work in Safari and Chrome. Skipping around in a song works too, because the board supports range requests.
- **Shelf speaker** appears once you turn on *Device → Speaker*. It plays **MP3 and WAV** through the MAX98357A and then carries on through the rest of the folder. Volume is in the player bar.
- New uploads are picked up automatically a few seconds after they finish. *Device → Rescan music* forces a rescan.

## Speeds and limits

- Expect roughly **0.5–1 MB/s** for uploads and downloads (SPI card plus Wi-Fi). Documents, photos, songs and app zips are quick; big videos are slow.
- The board does one heavy job at a time. Browsing while the speaker plays is fine, but a big upload during speaker playback may cause a stutter.
- **There's no login.** Anyone on your home Wi-Fi can open it, the same as The Shelf. Don't keep anything private on it, and don't expose it to the internet. Use Tailscale on a Pi if you want access from outside the house.
- The setup hotspot password `shelflite` is the same on every board. The hotspot only runs until the board joins your Wi-Fi.

## Building from source

The web installer has ready-made firmware, so you only need this to change the code.

Arduino IDE / arduino-cli with:

- **esp32 core 2.0.17** (Espressif)
- Libraries: **ESPAsyncWebServer 3.3.23** + **AsyncTCP 3.2.14** (mathieucarbou forks), **U8g2**, **ESP8266Audio 1.9.7**

```bash
./build.sh            # builds V3 + V4 and writes docs/firmware/*.bin
```

`build.sh` runs `tools/build_web.py` first, which gzips `web/index.html` and the icon into `HeltecShelfLite/web_assets.h`. In the Arduino IDE, choose **ESP32S3 Dev Module**, Flash Size 8 MB (V3) or 16 MB (V4), PSRAM *disabled*, and copy `partitions/v3_8mb.csv` or `partitions/v4_16mb.csv` to `HeltecShelfLite/partitions.csv`. For the V4, add `-DSHELF_BOARD_V4`.

### Working on the web page without a board

```bash
python3 tools/mock_server.py ~/some-folder     # then open http://localhost:8080
```

It serves the same page and copies the board's API, so you can change `web/index.html` and see it in a browser right away.

## Project layout

```
HeltecShelfLite/
  shelf_lite.cpp     firmware: Wi-Fi, storage, web API, speaker, OLED, button
  board.h            pin map (one wiring for V3 and V4)
  web_assets.h       generated: gzipped web app + icon
web/index.html       the web app the board serves
partitions/          flash layouts (V3 8 MB, V4 16 MB)
docs/                web installer (GitHub Pages) + firmware .bin files
tools/               web packer and the PC preview server
```

### Web API (for tinkering)

| Method | Path | What it does |
|---|---|---|
| GET | `/api/info` | board, storage, Wi-Fi, song count |
| GET | `/api/list?path=/Music` | folder listing |
| GET | `/f?p=/Music/a.mp3` | file download/stream (Range supported, `&dl=1` to download) |
| POST | `/api/upload?dir=/Music` | multipart upload |
| POST | `/api/mkdir`, `/api/rename`, `/api/delete` | `path`, `from`/`to` |
| GET | `/api/music` | every song as JSON |
| GET/POST | `/api/speaker` | `cmd=play&path=…`, `pause`, `resume`, `next`, `prev`, `stop`, `vol=0-100` |
| GET/POST | `/api/board` | notes and links (JSON list) |

Part of the Heltec family alongside [Heltec-C64](https://github.com/hardwaremack-prog/Heltec-C64).
