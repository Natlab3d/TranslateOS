# TranslateOS

A push-to-talk pocket translator for the **Waveshare ESP32-S3-Touch-LCD-3.49 (V1 board)**.
Tap REC, speak, tap REC again: Gemini transcribes and translates, Google Text-to-Speech speaks the result.
10 languages (English, Italian, French, Spanish, German, Portuguese, Japanese, Chinese, Ukrainian, Arabic).

Status: work in progress. Verified on hardware: boot, display, WiFi, screensaver, flip, settings.
Not yet verified on hardware: recording, translation, TTS playback, 3G use.

## Build
1. Install `arduino-cli` and the ESP32 core **3.3.0**: `arduino-cli core install esp32:esp32@3.3.0`
   (additional URL `https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
2. Install the libraries (see **Libraries** below).
3. In `firmware/translator_fw_v2/` copy `secrets.h.example` to `secrets.h` and put your own Gemini key
   (the key's project needs Gemini, Cloud Text-to-Speech and Geolocation enabled and must have credit).
   **Never commit `secrets.h`.**
4. Compile (from this folder):

       arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,USBMode=hwcdc,LoopCore=1,EventsCore=1 firmware/translator_fw_v2

   The first build compiles all of LVGL and takes several minutes.
5. Flash over USB (data cable). Back up the device first: `esptool read-flash 0x0 0x1000000 backup.bin`.
   The app goes to `0x10000`; the partition table is the standard "16M Flash (3MB APP/9.9MB FATFS)" scheme.

## Libraries
Not included in this repository. Install exactly these versions (the ones the firmware was built and tested with):

| Library | Version | Licence | Get it |
|---|---|---|---|
| ESP32 Arduino core | 3.3.0 | LGPL-2.1 | `arduino-cli core install esp32:esp32@3.3.0` |
| LVGL | 9.3.0 | MIT | `arduino-cli lib install lvgl@9.3.0` (or https://github.com/lvgl/lvgl/releases/tag/v9.3.0) |
| ArduinoJson | 6.21.5 | MIT | `arduino-cli lib install ArduinoJson@6.21.5` |
| WiFiManager (tzapu) | 2.0.17 | MIT | `arduino-cli lib install WiFiManager@2.0.17` |

The ESP32 core also provides `WiFi`, `WiFiClientSecure`, `HTTPClient`, `ArduinoOTA` and `Preferences`.

**LVGL needs two project files** from `firmware/translator_fw_v2/vendor_patches/` (LVGL heap in PSRAM, assert hook,
extra fonts, TinyTTF for the large clock):
1. Copy `lv_conf.h` into your Arduino `libraries/` folder, next to (not inside) the `lvgl` folder.
2. Copy `lvgl_assert_hook.h` into `libraries/lvgl/src/`.

## First run
Switch on. If no WiFi is saved, join the network **Translator** from a phone or laptop and pick your WiFi
(or use Settings, Connect, Reset WiFi). Settings are kept in flash. Popups explain every failure
(no WiFi, no internet, key rejected, credit used up, too short, ...). Low-battery popups appear at 20 % and 10 %.

## Notes
- Source layout: `tos_*.cpp` are the TranslateOS modules (settings, audio, network, WiFi, power), `ui.cpp` holds all screens,
  `app_api.h` is the contract between them. Board drivers are in `src/`.
- Hardware code follows Waveshare's own V1 examples. Do NOT use the V2-board code: the pins differ.
- Audio uses the vendor setting (24 kHz stereo); the upload is reduced in software after recording.
- Recorded audio is sent to Google (Gemini and Cloud Text-to-Speech) with your own API key; usage cost and privacy are yours.
- Certificate checking is still off for the Google calls (`setInsecure()`).
- The project's own code is MIT licensed (`LICENSE`). Third-party licences: `NOTICE.md`.
