#include "tos_audio.h"
#include "tos_config.h"
#include "app_api.h"
#include <Arduino.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"

// ------------------------------------------------------------------ state
static esp_codec_dev_handle_t s_rec  = NULL;
static esp_codec_dev_handle_t s_play = NULL;

// Buffers are allocated once at boot (PSRAM) and reused: no per-recording malloc/free.
static int16_t* s_raw    = NULL;           // codec stream, interleaved stereo 24 kHz
static size_t   s_rawCap = 0;              // in stereo frames
static uint8_t* s_wav    = NULL;           // 16 kHz mono WAV for upload
static size_t   s_wavCap = 0;              // bytes

static TaskHandle_t s_recTask = NULL;
static volatile bool s_recActive = false;
static volatile bool s_stopReq   = false;
static volatile uint32_t s_recStartMs = 0;
static volatile bool s_resultReady = false;
static RecResult s_result;

static SemaphoreHandle_t s_codecMux = NULL; // record and playback never overlap
static volatile bool s_playActive = false;
static volatile bool s_playCancel = false;
static int16_t s_playPrev = 0;              // upsampler state
static int     s_playPhase = 0;

// ------------------------------------------------------------------ helpers
static void write_wav_header(uint8_t* p, uint32_t dataBytes, uint32_t rate, uint16_t ch) {
  uint32_t byteRate = rate * ch * 2;
  memcpy(p, "RIFF", 4);  uint32_t v = 36 + dataBytes; memcpy(p + 4, &v, 4);
  memcpy(p + 8, "WAVEfmt ", 8);
  v = 16; memcpy(p + 16, &v, 4);
  uint16_t s = 1; memcpy(p + 20, &s, 2); memcpy(p + 22, &ch, 2);
  memcpy(p + 24, &rate, 4); memcpy(p + 28, &byteRate, 4);
  s = ch * 2; memcpy(p + 32, &s, 2); s = 16; memcpy(p + 34, &s, 2);
  memcpy(p + 36, "data", 4); memcpy(p + 40, &dataBytes, 4);
}

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

// Convert the raw stereo 24 kHz capture into trimmed 16 kHz mono. Returns sample count.
static size_t process_recording(size_t frames, RecResult* r) {
  // 1. per-channel RMS -> pick the channel that carries the voice (measured, not assumed)
  double sl = 0, sr = 0;
  for (size_t i = 0; i < frames; i++) { int32_t a = s_raw[2*i], b = s_raw[2*i+1]; sl += (double)a*a; sr += (double)b*b; }
  r->rmsL = frames ? (int)sqrt(sl / frames) : 0;
  r->rmsR = frames ? (int)sqrt(sr / frames) : 0;
  if (r->rmsL > r->rmsR * 3 / 2) r->chUsed = 0;
  else if (r->rmsR > r->rmsL * 3 / 2) r->chUsed = 1;
  else r->chUsed = 2;

  // 2. mono + DC/rumble high-pass (stronger with the noise filter) + 24->16 kHz (3 in -> 2 out)
  int16_t* out = (int16_t*)(s_wav + 44);
  size_t outCap = (s_wavCap - 44) / 2, n = 0;
  float hpA = g_settings.noise ? 0.96f : 0.995f;     // ~150 Hz vs ~20 Hz at 24 kHz
  float hpPrevIn = 0, hpPrevOut = 0;
  float x[3]; int k = 0;
  float lpPrev = 0;
  for (size_t i = 0; i < frames && n + 2 <= outCap; i++) {
    float m = r->chUsed == 0 ? s_raw[2*i] : (r->chUsed == 1 ? s_raw[2*i+1] : 0.5f * (s_raw[2*i] + s_raw[2*i+1]));
    float hp = hpA * (hpPrevOut + m - hpPrevIn); hpPrevIn = m; hpPrevOut = hp;
    float lp = 0.5f * hp + 0.5f * lpPrev; lpPrev = hp;           // gentle anti-alias before decimation
    x[k++] = lp;
    if (k == 3) { out[n++] = sat16((int32_t)x[0]); out[n++] = sat16((int32_t)(0.5f * (x[1] + x[2]))); k = 0; }
  }

  // 3. find speech: 20 ms frames, adaptive noise floor
  const size_t F = UPLOAD_RATE / 50;
  size_t nf = n / F;
  int first = -1, last = -1;
  if (nf > 0) {
    static uint16_t frms[MAX_RECORD_MS / 20 + 8];
    uint16_t sorted[MAX_RECORD_MS / 20 + 8];
    for (size_t f = 0; f < nf; f++) {
      double e = 0; for (size_t j = 0; j < F; j++) { int32_t v = out[f*F + j]; e += (double)v*v; }
      frms[f] = (uint16_t)fmin(65535.0, sqrt(e / F)); sorted[f] = frms[f];
    }
    // 20th percentile as noise floor (partial selection sort is fine for <=500 frames)
    size_t q = nf / 5;
    for (size_t a = 0; a <= q; a++) for (size_t b = a + 1; b < nf; b++) if (sorted[b] < sorted[a]) { uint16_t t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t; }
    int floorRms = sorted[q];
    int thr = floorRms * 3; if (thr < 300) thr = 300;
    if (g_settings.rmsThreshold > 0 && thr < g_settings.rmsThreshold) thr = g_settings.rmsThreshold;
    for (size_t f = 0; f < nf; f++) if (frms[f] > thr) { if (first < 0) first = f; last = f; }
  }
  r->silent = first < 0;
  size_t s0 = 0, s1 = n;
  if (!r->silent) {
    long a = (long)first * F - (long)(UPLOAD_RATE * 150 / 1000);
    long b = (long)(last + 1) * F + (long)(UPLOAD_RATE * 250 / 1000);
    s0 = a < 0 ? 0 : a; s1 = (size_t)b > n ? n : (size_t)b;
  }
  size_t len = s1 - s0;
  if (s0) memmove(out, out + s0, len * 2);

  // 4. optional AGC: bring the peak to about -3 dBFS, at most x8
  int peak = 1; for (size_t i = 0; i < len; i++) { int v = abs(out[i]); if (v > peak) peak = v; }
  if (g_settings.micAGC && !r->silent) {
    float g = 23000.0f / peak; if (g > 8.0f) g = 8.0f;
    if (g > 1.05f) { for (size_t i = 0; i < len; i++) out[i] = sat16((int32_t)(out[i] * g)); peak = (int)(peak * g); }
  }
  r->peak = peak;
  r->speechMs = len * 1000 / UPLOAD_RATE;
  write_wav_header(s_wav, len * 2, UPLOAD_RATE, 1);
  r->wav = s_wav; r->wavLen = 44 + len * 2;
  return len;
}

static void rec_task(void*) {
  const size_t CHUNK_FRAMES = 512;                       // 21 ms at 24 kHz
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(s_codecMux, pdMS_TO_TICKS(200)) != pdTRUE) { s_recActive = false; continue; }
    size_t frames = 0;
    uint32_t lastProg = 0;
    const size_t maxFrames = s_rawCap;
    ui_post_state(UI_RECORDING);
    Serial.println("[REC] start");
    while (!s_stopReq && frames + CHUNK_FRAMES <= maxFrames) {
      int ret = esp_codec_dev_read(s_rec, s_raw + frames * 2, CHUNK_FRAMES * 4);
      if (ret != ESP_CODEC_DEV_OK) { Serial.printf("[REC] read error %d\n", ret); vTaskDelay(1); continue; }
      frames += CHUNK_FRAMES;
      uint32_t el = millis() - s_recStartMs;
      if (el - lastProg >= 200) { lastProg = el; ui_post_rec_progress(el, MAX_RECORD_MS); }
    }
    xSemaphoreGive(s_codecMux);
    bool hitCap = frames + CHUNK_FRAMES > maxFrames;
    RecResult r = {};
    r.rawMs = frames * 1000 / CODEC_RATE;
    uint32_t t0 = millis();
    process_recording(frames, &r);
    Serial.printf("[REC] done raw=%ums speech=%ums wav=%u B rmsL=%d rmsR=%d ch=%d peak=%d silent=%d cap=%d proc=%ums\n",
                  r.rawMs, r.speechMs, (unsigned)r.wavLen, r.rmsL, r.rmsR, r.chUsed, r.peak, r.silent, hitCap,
                  (unsigned)(millis() - t0));
    s_result = r;
    s_recActive = false;
    s_resultReady = true;
  }
}

// ------------------------------------------------------------------ public
bool audio_init() {
  set_codec_board_type("S3_LCD_3_49");
  codec_init_cfg_t cfg = {};
  cfg.in_mode = CODEC_I2S_MODE_TDM;
  cfg.out_mode = CODEC_I2S_MODE_TDM;
  cfg.in_use_tdm = false;
  cfg.reuse_dev = false;
  if (init_codec(&cfg) != 0) { Serial.println("[AUDIO] init_codec failed"); return false; }
  s_play = get_playback_handle();
  s_rec  = get_record_handle();
  if (!s_play || !s_rec) { Serial.println("[AUDIO] codec handles missing"); return false; }
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = CODEC_RATE; fs.channel = CODEC_CHANNELS; fs.bits_per_sample = CODEC_BITS;
  int a = esp_codec_dev_open(s_play, &fs);
  int b = esp_codec_dev_open(s_rec, &fs);
  audio_set_volume(g_settings.volume * 10);
  audio_set_gain_db(g_settings.micGain * 3 + 9);
  Serial.printf("[AUDIO] codec open play=%d rec=%d (%u Hz %u ch)\n", a, b, (unsigned)CODEC_RATE, CODEC_CHANNELS);

  s_rawCap = CODEC_RATE * MAX_RECORD_MS / 1000;
  s_raw = (int16_t*)heap_caps_malloc(s_rawCap * 4, MALLOC_CAP_SPIRAM);
  s_wavCap = 44 + UPLOAD_RATE * 2 * MAX_RECORD_MS / 1000;
  s_wav = (uint8_t*)heap_caps_malloc(s_wavCap, MALLOC_CAP_SPIRAM);
  s_codecMux = xSemaphoreCreateMutex();
  if (!s_raw || !s_wav || !s_codecMux) { Serial.println("[AUDIO] buffer alloc failed"); return false; }
  xTaskCreatePinnedToCoreWithCaps(rec_task, "rec_task", 8 * 1024, NULL, 5, &s_recTask, 1, MALLOC_CAP_SPIRAM);
  Serial.printf("[AUDIO] buffers raw=%u KB wav=%u KB (PSRAM, allocated once)\n",
                (unsigned)(s_rawCap * 4 / 1024), (unsigned)(s_wavCap / 1024));
  return a == 0 && b == 0;
}

void audio_set_volume(int pct)   { if (s_play) esp_codec_dev_set_out_vol(s_play, (float)pct); }
void audio_set_gain_db(float db) { if (s_rec)  esp_codec_dev_set_in_gain(s_rec, db); }

bool audio_rec_start() {
  if (s_recActive || s_playActive || !s_recTask) return false;
  s_stopReq = false; s_resultReady = false; s_recActive = true;
  s_recStartMs = millis();
  xTaskNotifyGive(s_recTask);
  return true;
}
void audio_rec_stop()          { if (s_recActive) s_stopReq = true; }
bool audio_rec_active()        { return s_recActive; }
uint32_t audio_rec_elapsed_ms(){ return s_recActive ? millis() - s_recStartMs : 0; }
bool audio_rec_take_result(RecResult* out) {
  if (!s_resultReady) return false;
  s_resultReady = false; *out = s_result; return true;
}

bool audio_play_begin() {
  if (s_recActive || !s_codecMux) return false;
  if (xSemaphoreTake(s_codecMux, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  s_playCancel = false; s_playActive = true; s_playPrev = 0; s_playPhase = 0;
  return true;
}

// 16 kHz mono -> 24 kHz stereo by linear interpolation (2 in -> 3 out), written in small blocks.
bool audio_play_pcm16(const int16_t* in, size_t n) {
  if (!s_playActive) return false;
  static int16_t blk[2 * 768];
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    int16_t cur = in[i];
    // phase 0: output prev; phase 1: output 2/3 between prev and cur ... keeps 3:2 ratio over pairs
    if (s_playPhase == 0) {
      int16_t a = s_playPrev, b = (int16_t)((s_playPrev + 2 * (int32_t)cur) / 3);
      blk[o++] = a; blk[o++] = a; blk[o++] = b; blk[o++] = b;
      s_playPhase = 1;
    } else {
      int16_t c = (int16_t)((2 * (int32_t)s_playPrev + cur) / 3);
      blk[o++] = c; blk[o++] = c;
      s_playPhase = 0;
    }
    s_playPrev = cur;
    if (o >= 2 * 760) {
      if (s_playCancel) return false;
      esp_codec_dev_write(s_play, blk, o * 2); o = 0;
    }
  }
  if (o && !s_playCancel) esp_codec_dev_write(s_play, blk, o * 2);
  return !s_playCancel;
}

void audio_play_end() {
  if (!s_playActive) return;
  static int16_t zeros[2 * 480] = {0};             // 20 ms of silence flushes the DMA tail
  if (!s_playCancel) esp_codec_dev_write(s_play, zeros, sizeof(zeros));
  s_playActive = false;
  xSemaphoreGive(s_codecMux);
}
void audio_play_cancel() { s_playCancel = true; }
bool audio_play_active() { return s_playActive; }
