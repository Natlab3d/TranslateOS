// TranslateOS — constants. The API key is NOT here: see secrets.h (gitignored).
#ifndef TRANSLATEOS_CONFIG_H_
#define TRANSLATEOS_CONFIG_H_
#include <stdint.h>

#define FW_VERSION "2.0.0-dev"

// ---------------- audio (vendor V1 08_Audio_Test: never change these) ----------------
constexpr uint32_t CODEC_RATE      = 24000;   // Hz
constexpr uint8_t  CODEC_CHANNELS  = 2;
constexpr uint8_t  CODEC_BITS      = 16;

// Upload format produced in software from the codec stream
constexpr uint32_t UPLOAD_RATE     = 16000;   // Hz, mono 16-bit
constexpr uint32_t MAX_RECORD_MS   = 10000;   // hard cap of one utterance
constexpr uint32_t MIN_SPEECH_MS   = 400;     // shorter than this after trimming = "too short"

// TTS is requested at 16 kHz (1/3 less download than 24 kHz) and upsampled for the codec.
constexpr uint32_t TTS_RATE        = 16000;

// ---------------- Gemini ----------------
static const char* const GEMINI_MODEL = "gemini-2.5-flash-lite";
constexpr int GEMINI_MAX_TOKENS = 400;

// Translation prompt. Arguments: source, target, source, target.
static const char* const SYSTEM_PROMPT_TRANSLATION =
  "You are a translation engine. Translate speech ONLY. No other text.\n"
  "SOURCE LANGUAGE: %s\n"
  "TARGET LANGUAGE: %s\n"
  "STRICT RULES:\n"
  "- ALWAYS translate FROM %s INTO %s. NEVER output in the source language.\n"
  "- Output the natural idiomatic translation as if you are the speaker. Never translate word-for-word.\n"
  "- ALWAYS begin your response with [HEARD: original spoken words]. Example: [HEARD: what is your name]\n"
  "- TAG OUTPUT: Wrap the ENTIRE translated output in the ISO language code tag of the TARGET language. Example: If target is Italian, use [it]...[/it].\n"
  "- ASCII ONLY: For non-Latin scripts output in ASCII form: Japanese=romaji, Chinese=pinyin, Korean=romanization, Arabic=transliteration, Cyrillic=Latin transliteration, Ukrainian=Latin transliteration. Do NOT use native characters. This includes ALL languages using non-Latin alphabets.\n"
  "- NO ASSISTANT BEHAVIOR: Output ONLY the translation and [HEARD: transcription]. No greetings, questions, or commentary.\n"
  "- NO TIMESTAMPS: Ignore all 00:00 timestamps.\n"
  "- NO SPEECH RULE: If the audio contains silence, background noise, or no intelligible human speech, output ONLY the exact string: No speech detected - NOTHING ELSE. No translation, no explanation, no repetition, no filler. ANY other output for silent/noisy audio is a critical error.";

// ---------------- Google TTS voices (same order as LANG_CODES) ----------------
static const char* const TTS_LANG[10]  = {"en-GB","it-IT","fr-FR","es-ES","de-DE","pt-PT","ja-JP","cmn-CN","uk-UA","ar-XA"};
static const char* const TTS_VOICE[10] = {"en-GB-Neural2-A","it-IT-Neural2-A","fr-FR-Neural2-A","es-ES-Neural2-A",
                                          "de-DE-Neural2-A","pt-PT-Wavenet-A","ja-JP-Wavenet-B","cmn-CN-Wavenet-A",
                                          "uk-UA-Wavenet-A","ar-XA-Wavenet-A"};

// ---------------- network ----------------
constexpr uint32_t NET_CONNECT_TIMEOUT_MS = 8000;     // TCP + TLS handshake
constexpr uint32_t NET_BASE_TIMEOUT_MS    = 12000;    // server think time + small transfers
constexpr uint32_t NET_MIN_RATE_BPS       = 4000;     // budget assumes at least 4 KB/s (bad 3G)
constexpr uint32_t NET_SLOW_NOTICE_MS     = 6000;     // show "slow connection" after this
constexpr uint8_t  NET_RETRIES            = 1;        // network errors / 5xx only, never 4xx

// ---------------- WiFi ----------------
constexpr uint32_t WIFI_PORTAL_TIMEOUT_S  = 180;      // only when the user asked for setup
constexpr uint8_t  WIFI_MAX_KNOWN         = 3;        // remembered networks (home + phone hotspot ...)
#endif
