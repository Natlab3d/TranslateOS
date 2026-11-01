// Network task: Gemini translation and Google TTS over a small keep-alive HTTPS client.
// Why not HTTPClient: we need upload progress, cancel between blocks, a "slow" notice, and to play TTS
// while it downloads; HTTPClient's blocking POST/getString() gives none of that and needs big Strings.
#include "tos_net.h"
#include "tos_config.h"
#include "secrets.h"
#include "app_api.h"
#include "tos_audio.h"
#include "tos_settings.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"

// ================================================================= buffers (allocated once)
static char*    s_req     = NULL;  static size_t s_reqCap  = 0;    // request body (JSON + base64 audio)
static char*    s_resp    = NULL;  static size_t s_respCap = 0;    // small JSON responses
static int16_t* s_tts     = NULL;  static size_t s_ttsCap  = 0;    // last spoken audio (16 kHz mono)
static size_t   s_ttsLen  = 0;     static int    s_ttsPair = -1;

#define HIST_MAX 20
#define TXT_MAX  400
typedef struct { char orig[TXT_MAX + 1]; char trans[TXT_MAX + 1]; int dst; } Pair;
static Pair* s_hist = NULL; static int s_histCount = 0, s_histHead = 0;   // ring

typedef enum { JOB_TRANSLATE, JOB_SPEAK } JobType;
typedef struct { JobType type; const uint8_t* wav; size_t len; int src, dst, pair; } Job;
static QueueHandle_t s_jobs = NULL;
static volatile bool s_busy = false, s_cancel = false;

// ================================================================= errors
typedef enum { E_OK, E_NOWIFI, E_CONNECT, E_TIMEOUT, E_IO, E_CANCEL, E_HTTP, E_PARSE, E_JUNK, E_CONFIG } NetErr;
typedef struct { NetErr err; int code; char msg[96]; } NetResult;

static bool retryable(const NetResult& r) {
  return r.err == E_CONNECT || r.err == E_TIMEOUT || r.err == E_IO || (r.err == E_HTTP && r.code >= 500);
}

static void toast_for(const NetResult& r, const char* what) {
  char t[128];
  switch (r.err) {
    case E_NOWIFI:  ui_post_toast(TOAST_ERROR, "No WiFi - check hotspot / WiFi", 5000); return;
    case E_CONNECT: ui_post_toast(TOAST_ERROR, "No internet connection", 5000); return;
    case E_TIMEOUT: ui_post_toast(TOAST_ERROR, "Network too slow - try a shorter phrase", 5000); return;
    case E_IO:      ui_post_toast(TOAST_ERROR, "Connection dropped - try again", 5000); return;
    case E_CANCEL:  ui_post_toast(TOAST_INFO,  "Cancelled", 1500); return;
    case E_PARSE:   ui_post_toast(TOAST_ERROR, "Bad reply from server - try again", 5000); return;
    case E_JUNK:    ui_post_toast(TOAST_INFO,  "Didn't understand - speak again", 3000); return;
    case E_CONFIG:  ui_post_toast(TOAST_ERROR, "Set GEMINI_MODEL in secrets.h", 5000); return;
    case E_HTTP:
      if (r.code == 402)                    snprintf(t, sizeof t, "Gemini credit used up - top up in AI Studio");
      else if (r.code == 401 || r.code == 403) snprintf(t, sizeof t, "API key rejected (%d) - check the key", r.code);
      else if (r.code == 404)               snprintf(t, sizeof t, "Model not found - check GEMINI_MODEL");
      else if (r.code == 429)               snprintf(t, sizeof t, "Too many requests - wait a moment");
      else if (r.code >= 500)               snprintf(t, sizeof t, "Google %s busy (%d) - try again", what, r.code);
      else                                  snprintf(t, sizeof t, "%s error %d: %.60s", what, r.code, r.msg);
      ui_post_toast(TOAST_ERROR, t, 5000); return;
    default: return;
  }
}

// ================================================================= tiny HTTPS client
struct Conn {
  const char* host;
  WiFiClientSecure cli;
  uint32_t lastUse = 0;
};
static Conn s_gem{ "generativelanguage.googleapis.com" };
static Conn s_ttsConn{ "texttospeech.googleapis.com" };

typedef struct { int code; bool chunked; long contentLength; bool close; } Resp;

static NetErr conn_ensure(Conn& c) {
  if (WiFi.status() != WL_CONNECTED) return E_NOWIFI;
  // Google closes idle connections after a while; don't trust a socket idle for > 50 s.
  if (c.cli.connected() && millis() - c.lastUse < 50000) return E_OK;
  c.cli.stop();
  c.cli.setInsecure();                       // TODO: CA bundle (core 3.3.0 needs setCACertBundle)
  c.cli.setHandshakeTimeout(NET_CONNECT_TIMEOUT_MS / 1000);
  uint32_t t0 = millis();
  if (!c.cli.connect(c.host, 443, NET_CONNECT_TIMEOUT_MS)) {
    Serial.printf("[NET] connect %s failed after %u ms\n", c.host, (unsigned)(millis() - t0));
    return E_CONNECT;
  }
  Serial.printf("[NET] connected %s in %u ms\n", c.host, (unsigned)(millis() - t0));
  return E_OK;
}

static NetErr write_all(Conn& c, const uint8_t* p, size_t n, uint32_t deadline, uint32_t* slowAt) {
  size_t off = 0;
  while (off < n) {
    if (s_cancel) return E_CANCEL;
    if ((int32_t)(millis() - deadline) > 0) return E_TIMEOUT;
    if (slowAt && *slowAt && millis() > *slowAt) { ui_post_toast(TOAST_WARN, "Slow connection - still sending...", 4000); *slowAt = 0; }
    size_t k = n - off > 4096 ? 4096 : n - off;
    size_t w = c.cli.write(p + off, k);
    if (w == 0) { if (!c.cli.connected()) return E_IO; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    off += w;
  }
  return E_OK;
}

static int read_byte(Conn& c, uint32_t deadline) {
  for (;;) {
    if (s_cancel) return -2;
    int v = c.cli.read();
    if (v >= 0) return v;
    if (!c.cli.connected() && !c.cli.available()) return -1;
    if ((int32_t)(millis() - deadline) > 0) return -3;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static NetErr read_line(Conn& c, char* buf, size_t cap, uint32_t deadline) {
  size_t n = 0;
  for (;;) {
    int v = read_byte(c, deadline);
    if (v == -2) return E_CANCEL;
    if (v == -3) return E_TIMEOUT;
    if (v < 0) return E_IO;
    if (v == '\n') break;
    if (v != '\r' && n + 1 < cap) buf[n++] = (char)v;
  }
  buf[n] = 0;
  return E_OK;
}

static NetErr read_headers(Conn& c, Resp& r, uint32_t deadline) {
  char line[256];
  NetErr e = read_line(c, line, sizeof line, deadline);
  if (e) return e;
  r.code = 0; r.chunked = false; r.contentLength = -1; r.close = false;
  if (sscanf(line, "HTTP/%*s %d", &r.code) != 1) return E_IO;
  for (;;) {
    e = read_line(c, line, sizeof line, deadline);
    if (e) return e;
    if (!line[0]) break;
    if (!strncasecmp(line, "Content-Length:", 15)) r.contentLength = atol(line + 15);
    else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line, "chunked")) r.chunked = true;
    else if (!strncasecmp(line, "Connection:", 11) && strcasestr(line, "close")) r.close = true;
  }
  return E_OK;
}

// Body reader handling Content-Length and chunked encoding. Returns bytes, 0 at end, <0 error.
struct Body {
  Conn* c; Resp* r; uint32_t deadline; long left; bool done = false;
  void begin(Conn& cc, Resp& rr, uint32_t dl) { c = &cc; r = &rr; deadline = dl; left = rr.chunked ? 0 : rr.contentLength; done = (!rr.chunked && rr.contentLength == 0); }
  int read(uint8_t* out, int cap) {
    if (done) return 0;
    if (r->chunked && left == 0) {
      char line[32];
      if (read_line(*c, line, sizeof line, deadline)) return -1;
      if (!line[0] && read_line(*c, line, sizeof line, deadline)) return -1;   // CRLF after previous chunk
      left = strtol(line, NULL, 16);
      if (left == 0) { read_line(*c, line, sizeof line, deadline); done = true; return 0; }
    }
    int n = 0;
    while (n < cap && (left > 0 || left < 0)) {
      int v = read_byte(*c, deadline);
      if (v == -1 && left < 0) { done = true; break; }       // no length: read until close
      if (v < 0) return v == -2 ? -2 : -1;
      out[n++] = (uint8_t)v; if (left > 0) left--;
      if (!c->cli.available() && n > 0) break;               // hand back what we have
    }
    if (!r->chunked && left == 0) done = true;
    return n;
  }
};

static NetErr send_request(Conn& c, const char* path, const char* body, size_t len, uint32_t deadline, uint32_t* slowAt) {
  char hdr[400];
  int h = snprintf(hdr, sizeof hdr,
    "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nx-goog-api-key: %s\r\n"
    "Content-Length: %u\r\nConnection: keep-alive\r\n\r\n", path, c.host, GEMINI_API_KEY, (unsigned)len);
  NetErr e = write_all(c, (const uint8_t*)hdr, h, deadline, NULL);
  if (e) return e;
  return write_all(c, (const uint8_t*)body, len, deadline, slowAt);
}

// Read a whole (small) body into s_resp.
static NetErr read_small_body(Conn& c, Resp& r, uint32_t deadline, size_t* outLen) {
  Body b; b.begin(c, r, deadline);
  size_t n = 0;
  for (;;) {
    int k = b.read((uint8_t*)s_resp + n, (int)(s_respCap - 1 - n));
    if (k == -2) return E_CANCEL;
    if (k < 0) return E_TIMEOUT;
    if (k == 0) break;
    n += k;
    if (n >= s_respCap - 1) break;
  }
  s_resp[n] = 0; *outLen = n;
  return E_OK;
}

// ================================================================= JSON / text helpers
static size_t json_escape(char* dst, size_t cap, const char* s) {
  size_t n = 0;
  for (; *s && n + 7 < cap; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '"' || ch == '\\') { dst[n++] = '\\'; dst[n++] = ch; }
    else if (ch == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
    else if (ch == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
    else if (ch == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
    else if (ch < 0x20) { n += snprintf(dst + n, cap - n, "\\u%04x", ch); }
    else dst[n++] = ch;
  }
  dst[n] = 0;
  return n;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t* src, size_t len, char* dst) {
  size_t o = 0, i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = (src[i] << 16) | (src[i+1] << 8) | src[i+2];
    dst[o++] = B64[v >> 18]; dst[o++] = B64[(v >> 12) & 63]; dst[o++] = B64[(v >> 6) & 63]; dst[o++] = B64[v & 63];
  }
  if (i < len) {
    uint32_t v = src[i] << 16; if (i + 1 < len) v |= src[i+1] << 8;
    dst[o++] = B64[v >> 18]; dst[o++] = B64[(v >> 12) & 63];
    dst[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '='; dst[o++] = '=';
  }
  return o;
}

// Length of an audio-position timestamp at p: "00:SS", "0:SS" or "00:00:SS" (clip is <= 10 s, so minutes/hours are 0).
static int ts_len(const char* p) {
  int i = 0;
  if (p[0] == '0' && p[1] == '0' && p[2] == ':' && p[3] == '0' && p[4] == '0' && p[5] == ':') i = 6;      // 00:00:SS
  else if (p[0] == '0' && p[1] == '0' && p[2] == ':') i = 3;                                              // 00:SS
  else if (p[0] == '0' && p[1] == ':') i = 2;                                                             // 0:SS
  else return 0;
  if (!(p[i] >= '0' && p[i] <= '5' && isdigit((unsigned char)p[i + 1]))) return 0;
  if (isdigit((unsigned char)p[i + 2]) || (p[i + 2] == ':' && isdigit((unsigned char)p[i + 3]))) return 0;                                      // part of a longer number
  return i + 2;
}

static void strip_inplace(char* s) {                 // trim + drop markdown and audio timestamps
  char* w = s;
  for (char* r = s; *r; ) {
    if (*r == '*' || *r == '#') { r++; continue; }
    const char* q = r;
    bool open = (*q == '[' || *q == '(');
    if (open) q++;
    int n = (r == s || !(isdigit((unsigned char)r[-1]) || r[-1] == ':')) ? ts_len(q) : 0;
    if (n) {
      q += n;
      // range "00:00-00:03", "00:00 - 00:03", "00:00 to 00:03", en dash
      const char* t = q; while (*t == ' ') t++;
      int dash = (*t == '-') ? 1 : ((unsigned char)t[0] == 0xE2 && (unsigned char)t[1] == 0x80 && (unsigned char)t[2] == 0x93) ? 3 : (t[0] == 't' && t[1] == 'o') ? 2 : 0;
      if (dash) {
        const char* u = t + dash; while (*u == ' ') u++;
        int n2 = ts_len(u); if (n2) q = u + n2;
      }
      if (open && (*q == ']' || *q == ')')) q++; else if (open) n = 0;          // brackets must close
      if (n) { if (*q == ':') q++; while (*q == ' ') q++; r = (char*)q; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
  for (char* p = s; (p = strstr(p, "[]")) != NULL; ) memmove(p, p + 2, strlen(p + 2) + 1);
  char* a = s; while (*a == ' ' || *a == '\n') a++;
  memmove(s, a, strlen(a) + 1);
  size_t len = strlen(s); while (len && (s[len-1] == ' ' || s[len-1] == '\n')) s[--len] = 0;
}

// Pull [HEARD: ...] and [xx]...[/xx] out of the model text.
static void parse_answer(const char* text, int dst, char* heard, char* trans) {
  heard[0] = trans[0] = 0;
  const char* h = strstr(text, "[HEARD:");
  if (h) { const char* e = strchr(h, ']'); if (e) { size_t n = e - (h + 7); if (n > TXT_MAX) n = TXT_MAX; memcpy(heard, h + 7, n); heard[n] = 0; } }
  char open[8], close[8];
  snprintf(open, sizeof open, "[%s]", LANG_CODES[dst]); snprintf(close, sizeof close, "[/%s]", LANG_CODES[dst]);
  const char* o = strstr(text, open);
  if (o) {
    o += strlen(open);
    const char* e = strstr(o, close); size_t n = e ? (size_t)(e - o) : strlen(o);
    if (n > TXT_MAX) n = TXT_MAX; memcpy(trans, o, n); trans[n] = 0;
  } else {                                           // fallback: everything outside brackets
    size_t n = 0; int depth = 0;
    for (const char* p = text; *p && n < TXT_MAX; p++) { if (*p == '[') depth++; else if (*p == ']') { if (depth) depth--; } else if (!depth) trans[n++] = *p; }
    trans[n] = 0;
  }
  strip_inplace(heard); strip_inplace(trans);
}

// ================================================================= Gemini
static NetResult gemini_translate(const uint8_t* wav, size_t len, int src, int dst, char* heard, char* trans) {
  NetResult res = { E_OK, 0, "" };
  if (!GEMINI_MODEL[0] || !strcmp(GEMINI_MODEL, "MODEL NAME HERE")) { res.err = E_CONFIG; return res; }
  // ---- build body once (reused by the retry)
  static char prompt[1600], esc[2400];
  snprintf(prompt, sizeof prompt, SYSTEM_PROMPT_TRANSLATION, LANG_NAMES[src], LANG_NAMES[dst], LANG_NAMES[src], LANG_NAMES[dst]);
  json_escape(esc, sizeof esc, prompt);
  size_t n = snprintf(s_req, s_reqCap, "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},"
                      "\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"", esc);
  if (n + ((len + 2) / 3) * 4 + 256 > s_reqCap) { res.err = E_PARSE; snprintf(res.msg, sizeof res.msg, "request too big"); return res; }
  n += b64_encode(wav, len, s_req + n);
  // thinkingConfig is only accepted by the 2.5 family; other models answer HTTP 400.
  const char* thinkOff = strstr(GEMINI_MODEL, "2.5") ? ",\"thinkingConfig\":{\"thinkingBudget\":0}" : "";
  n += snprintf(s_req + n, s_reqCap - n, "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":%d,\"temperature\":%.2f%s}}",
                GEMINI_MAX_TOKENS, settings_temperature(), thinkOff);
  char path[128];
  snprintf(path, sizeof path, "/v1beta/models/%s:generateContent", GEMINI_MODEL);

  for (int attempt = 0; attempt <= NET_RETRIES; attempt++) {
    if (attempt) {
      char t[64]; snprintf(t, sizeof t, "Slow connection - retrying (%d/%d)", attempt, NET_RETRIES);
      ui_post_toast(TOAST_WARN, t, 3000);
      s_gem.cli.stop(); vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (s_cancel) { res.err = E_CANCEL; return res; }
    uint32_t t0 = millis();
    res.err = conn_ensure(s_gem);
    if (res.err) { if (retryable(res)) continue; return res; }
    uint32_t tConn = millis();
    uint32_t budget = NET_BASE_TIMEOUT_MS + (uint32_t)((uint64_t)n * 1000 / NET_MIN_RATE_BPS);
    if (budget > 45000) budget = 45000;
    uint32_t deadline = millis() + budget, slowAt = millis() + NET_SLOW_NOTICE_MS;
    res.err = send_request(s_gem, path, s_req, n, deadline, &slowAt);
    uint32_t tSent = millis();
    Resp r = {};
    if (!res.err) res.err = read_headers(s_gem, r, deadline);
    uint32_t tHdr = millis();
    size_t bl = 0;
    if (!res.err) res.err = read_small_body(s_gem, r, deadline, &bl);
    uint32_t tEnd = millis();
    float kbps = (tSent > tConn) ? (n * 8.0f / (tSent - tConn)) : 0;
    Serial.printf("[NET] gem up=%u B conn=%u ms send=%u ms (%.0f kbit/s) wait=%u ms body=%u B total=%u ms code=%d err=%d try=%d\n",
                  (unsigned)n, (unsigned)(tConn - t0), (unsigned)(tSent - tConn), kbps, (unsigned)(tHdr - tSent),
                  (unsigned)bl, (unsigned)(tEnd - t0), r.code, res.err, attempt);
    if (res.err) { s_gem.cli.stop(); if (retryable(res) && res.err != E_CANCEL) continue; return res; }
    s_gem.lastUse = millis();
    if (r.close) s_gem.cli.stop();

    if (r.code != 200) {
      res.err = E_HTTP; res.code = r.code;
      StaticJsonDocument<256> f; f["error"]["message"] = true;
      DynamicJsonDocument d(2048);
      if (!deserializeJson(d, s_resp, DeserializationOption::Filter(f))) snprintf(res.msg, sizeof res.msg, "%s", (const char*)(d["error"]["message"] | ""));
      Serial.printf("[NET] gemini HTTP %d: %s\n", r.code, res.msg);
      if (retryable(res)) continue;
      return res;
    }
    StaticJsonDocument<256> f; f["candidates"][0]["content"]["parts"][0]["text"] = true;
    DynamicJsonDocument d(8192);
    if (deserializeJson(d, s_resp, DeserializationOption::Filter(f))) { res.err = E_PARSE; return res; }
    const char* text = d["candidates"][0]["content"]["parts"][0]["text"] | "";
    Serial.printf("[NET] gemini text: %s\n", text);
    if (strstr(text, "No speech detected") && strlen(text) < 40) { heard[0] = trans[0] = 0; res.err = E_OK; res.code = 204; return res; }
    parse_answer(text, dst, heard, trans);
    // A real answer carries the target-language tag. Apologies / refusals / chatter must never reach the chat.
    char tag[8]; snprintf(tag, sizeof tag, "[%s]", LANG_CODES[dst]);
    bool tagged = strstr(text, tag) != NULL;
    bool chatter = strcasestr(text, "i'm sorry") || strcasestr(text, "i cannot") || strcasestr(text, "i can't") ||
                   strcasestr(text, "as an ai") || strcasestr(text, "unable to");
    if (!tagged && (!heard[0] || chatter)) {
      Serial.printf("[NET] reply rejected (no target tag): %s\n", text);
      res.err = E_JUNK; return res;
    }
    if (!trans[0]) { res.err = E_PARSE; return res; }
    res.err = E_OK; res.code = 200;
    return res;
  }
  return res;
}

// ================================================================= TTS (streamed playback)
static int b64v(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1;
}

static NetResult tts_speak(const char* text, int lang, int pairIndex) {
  NetResult res = { E_OK, 0, "" };
  static char esc[TXT_MAX * 2 + 16];
  json_escape(esc, sizeof esc, text);
  size_t n = snprintf(s_req, s_reqCap,
    "{\"input\":{\"text\":\"%s\"},\"voice\":{\"languageCode\":\"%s\",\"name\":\"%s\"},"
    "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\",\"sampleRateHertz\":%u}}", esc, TTS_LANG[lang], TTS_VOICE[lang], (unsigned)TTS_RATE);

  for (int attempt = 0; attempt <= NET_RETRIES; attempt++) {
    if (attempt) { s_ttsConn.cli.stop(); vTaskDelay(pdMS_TO_TICKS(1000)); }
    if (s_cancel) { res.err = E_CANCEL; return res; }
    uint32_t t0 = millis();
    res.err = conn_ensure(s_ttsConn);
    if (res.err) { if (retryable(res)) continue; return res; }
    uint32_t deadline = millis() + NET_BASE_TIMEOUT_MS + 20000;
    res.err = send_request(s_ttsConn, "/v1/text:synthesize", s_req, n, deadline, NULL);
    Resp r = {};
    if (!res.err) res.err = read_headers(s_ttsConn, r, deadline);
    if (res.err) { s_ttsConn.cli.stop(); if (retryable(res) && res.err != E_CANCEL) continue; return res; }
    uint32_t tHdr = millis();
    if (r.code != 200) {
      size_t bl; read_small_body(s_ttsConn, r, deadline, &bl);
      res.err = E_HTTP; res.code = r.code;
      StaticJsonDocument<256> f; f["error"]["message"] = true;
      DynamicJsonDocument d(2048);
      if (!deserializeJson(d, s_resp, DeserializationOption::Filter(f))) snprintf(res.msg, sizeof res.msg, "%s", (const char*)(d["error"]["message"] | ""));
      Serial.printf("[NET] tts HTTP %d: %s\n", r.code, res.msg);
      s_ttsConn.lastUse = millis();
      if (retryable(res)) continue;
      return res;
    }
    // ---- stream: find "audioContent": "<base64>", decode, skip the 44-byte WAV header, play
    Body b; b.begin(s_ttsConn, r, deadline);
    static const char KEY[] = "\"audioContent\"";
    size_t keyPos = 0; int stage = 0;           // 0 find key, 1 find opening quote, 2 data, 3 done
    uint32_t acc = 0; int bits = 0; size_t outBytes = 0; uint8_t odd = 0; bool haveOdd = false;
    static int16_t pcm[512]; size_t pn = 0;
    bool playing = false; size_t total = 0;
    s_ttsLen = 0; s_ttsPair = -1;
    static uint8_t buf[1024];
    for (;;) {
      int k = b.read(buf, sizeof buf);
      if (k == -2) { res.err = E_CANCEL; break; }
      if (k < 0) { res.err = E_TIMEOUT; break; }
      if (k == 0) break;
      total += k;
      for (int i = 0; i < k && stage < 3; i++) {
        char ch = (char)buf[i];
        if (stage == 0) { keyPos = (ch == KEY[keyPos]) ? keyPos + 1 : (ch == KEY[0] ? 1 : 0); if (keyPos == sizeof(KEY) - 1) stage = 1; continue; }
        if (stage == 1) { if (ch == '"') stage = 2; continue; }
        if (ch == '"') { stage = 3; break; }
        int v = b64v(ch); if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) {
          bits -= 8; uint8_t byte = (acc >> bits) & 0xFF;
          if (outBytes++ < 44) continue;                       // WAV header from Google
          if (!haveOdd) { odd = byte; haveOdd = true; continue; }
          haveOdd = false;
          pcm[pn++] = (int16_t)(odd | (byte << 8));
          if (pn == 512) {
            if (!playing) { if (!audio_play_begin()) { res.err = E_IO; stage = 3; break; } playing = true; ui_post_state(UI_SPEAKING); }
            if (s_tts && s_ttsLen + pn <= s_ttsCap) { memcpy(s_tts + s_ttsLen, pcm, pn * 2); s_ttsLen += pn; }
            if (!audio_play_pcm16(pcm, pn)) { res.err = E_CANCEL; stage = 3; break; }
            pn = 0;
          }
        }
      }
      if (res.err) break;
    }
    if (pn && !res.err) {
      if (!playing && audio_play_begin()) { playing = true; ui_post_state(UI_SPEAKING); }
      if (s_tts && s_ttsLen + pn <= s_ttsCap) { memcpy(s_tts + s_ttsLen, pcm, pn * 2); s_ttsLen += pn; }
      if (playing) audio_play_pcm16(pcm, pn);
    }
    if (playing) audio_play_end();
    // drain the rest so the connection can be reused
    if (!res.err) { while (b.read(buf, sizeof buf) > 0) {} s_ttsConn.lastUse = millis(); if (r.close) s_ttsConn.cli.stop(); }
    else s_ttsConn.cli.stop();
    Serial.printf("[NET] tts dn=%u B first=%u ms total=%u ms audio=%u ms err=%d\n", (unsigned)total,
                  (unsigned)(tHdr - t0), (unsigned)(millis() - t0), (unsigned)(s_ttsLen * 1000 / TTS_RATE), res.err);
    if (!res.err && outBytes <= 44) { res.err = E_PARSE; return res; }
    if (!res.err) s_ttsPair = pairIndex;
    return res;
  }
  return res;
}

static void play_cache() {
  if (!s_ttsLen || !audio_play_begin()) return;
  ui_post_state(UI_SPEAKING);
  for (size_t i = 0; i < s_ttsLen; i += 512) {
    size_t k = s_ttsLen - i > 512 ? 512 : s_ttsLen - i;
    if (!audio_play_pcm16(s_tts + i, k)) break;
  }
  audio_play_end();
}

// ================================================================= history
static int s_pairTotal = 0;                    // absolute pair id, same counting as the UI (since boot / last clear)
static int hist_push(const char* orig, const char* trans, int dst) {
  int idx = (s_histHead + s_histCount) % HIST_MAX;
  if (s_histCount == HIST_MAX) { idx = s_histHead; s_histHead = (s_histHead + 1) % HIST_MAX; }
  else s_histCount++;
  snprintf(s_hist[idx].orig, sizeof s_hist[idx].orig, "%s", orig);
  snprintf(s_hist[idx].trans, sizeof s_hist[idx].trans, "%s", trans);
  s_hist[idx].dst = dst;
  return s_pairTotal++;                          // absolute id of this pair
}
static Pair* hist_at(int absId) {
  int rel = absId - (s_pairTotal - s_histCount);   // position in the ring, 0 = oldest kept
  if (rel < 0 || rel >= s_histCount) return NULL;
  return &s_hist[(s_histHead + rel) % HIST_MAX];
}
int  net_history_count() { return s_histCount; }
void net_history_clear() { s_histCount = 0; s_histHead = 0; s_pairTotal = 0; s_ttsPair = -1; s_ttsLen = 0; }

// ================================================================= task
static void net_task(void*) {
  Job j;
  static char heard[TXT_MAX + 1], trans[TXT_MAX + 1];
  for (;;) {
    if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
    s_cancel = false; s_busy = true;
    bool ok = false;
    if (j.type == JOB_TRANSLATE) {
      ui_post_state(UI_PROCESSING);
      NetResult r = gemini_translate(j.wav, j.len, j.src, j.dst, heard, trans);
      if (r.err) toast_for(r, "translation");
      else if (r.code == 204) ui_post_toast(TOAST_INFO, "Didn't catch that - speak again", 3000);
      else {
        int pair = hist_push(heard, trans, j.dst);
        ui_post_bubble(heard, trans);
        NetResult t = tts_speak(trans, j.dst, pair);
        if (t.err) toast_for(t, "speech");
        ok = true;
      }
    } else {
      Pair* p = hist_at(j.pair);
      if (p) {
        if (j.pair == s_ttsPair && s_ttsLen) play_cache();
        else { NetResult t = tts_speak(p->trans, p->dst, j.pair); if (t.err) toast_for(t, "speech"); }
        ok = true;
      }
    }
    ui_post_state(UI_IDLE);
    s_busy = false;
    app_net_done(ok);
  }
}

// ================================================================= public
void net_init() {
  s_reqCap  = 2600 + ((44 + UPLOAD_RATE * 2 * MAX_RECORD_MS / 1000 + 2) / 3) * 4 + 512;
  s_req     = (char*)heap_caps_malloc(s_reqCap, MALLOC_CAP_SPIRAM);
  s_respCap = 32 * 1024;
  s_resp    = (char*)heap_caps_malloc(s_respCap, MALLOC_CAP_SPIRAM);
  s_ttsCap  = TTS_RATE * 15;                      // 15 s of speech
  s_tts     = (int16_t*)heap_caps_malloc(s_ttsCap * 2, MALLOC_CAP_SPIRAM);
  s_hist    = (Pair*)heap_caps_calloc(HIST_MAX, sizeof(Pair), MALLOC_CAP_SPIRAM);
  s_jobs    = xQueueCreate(2, sizeof(Job));
  if (!s_req || !s_resp || !s_tts || !s_hist || !s_jobs) { Serial.println("[NET] buffer alloc FAILED"); return; }
  // stack must be in internal RAM: lwIP asserts if a task opens a TCP connection with its stack in PSRAM
  xTaskCreatePinnedToCore(net_task, "net_task", 12 * 1024, NULL, 3, NULL, 0);
  Serial.printf("[NET] buffers req=%u KB resp=%u KB tts=%u KB (PSRAM, allocated once)\n",
                (unsigned)(s_reqCap / 1024), (unsigned)(s_respCap / 1024), (unsigned)(s_ttsCap * 2 / 1024));
}

bool net_busy()   { return s_jobs && (s_busy || uxQueueMessagesWaiting(s_jobs) > 0); }   // safe before net_init()
void net_cancel() { s_cancel = true; audio_play_cancel(); }

bool net_submit_translate(const uint8_t* wav, size_t len, int src, int dst) {
  if (!s_jobs) return false;
  Job j = { JOB_TRANSLATE, wav, len, src, dst, -1 };
  return xQueueSend(s_jobs, &j, 0) == pdTRUE;
}
bool net_submit_speak_pair(int pairIndex) {
  if (!s_jobs) return false;
  Job j = { JOB_SPEAK, NULL, 0, 0, 0, pairIndex };
  return xQueueSend(s_jobs, &j, 0) == pdTRUE;
}
