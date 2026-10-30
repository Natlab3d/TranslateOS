# Third-party components

Libraries below are NOT redistributed here; install them as described in `README.md` ("Libraries").

- **LVGL 9.3.0**: MIT. `vendor_patches/lv_conf.h` and `lvgl_assert_hook.h` are project edits for it.
- **ArduinoJson 6.21.5**: MIT.
- **WiFiManager 2.0.17** (tzapu): MIT.

Included in this repository:

- **Espressif `codec_board`**: Espressif Modified MIT (`firmware/translator_fw_v2/src/codec_board/LICENSE`).
- **Espressif `esp_codec_dev`**: Apache 2.0 (`firmware/translator_fw_v2/src/esp_codec_dev/LICENSE`).
- **Montserrat** typeface: SIL Open Font License 1.1, embedded in `firmware/translator_fw_v2/ttf_montserrat.c`
  (licence text: https://openfontlicense.org).
- **Espressif board components** (`axs15231b`, `touch`, `tca9554`): Apache 2.0, stated in each file header (SPDX).
- **MultiButton** (`button_bsp/multi_button.*`): by Zibin Zheng; the file header carries no licence line.
- **Waveshare board setup** (`adc_bsp`, `lcd_bl_bsp`, `i2c_bsp`, `lvgl_port`, `user_config.h`): based on Waveshare's example
  code for this board, which states no licence.
- **QMI8658 / PCF85063 drivers**: project code written from the vendor examples.

The project's own code is under the MIT licence (`LICENSE`).
