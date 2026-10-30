// Audio: vendor codec setup (24 kHz stereo TDM, never changed), recording task, and the software
// conversion to a small upload (16 kHz mono, trimmed) plus streamed playback of 16 kHz TTS audio.
#ifndef TRANSLATEOS_AUDIO_H_
#define TRANSLATEOS_AUDIO_H_
#include <stdint.h>
#include <stddef.h>

typedef struct {
  const uint8_t* wav;     // 16 kHz mono 16-bit WAV (header included), valid until the next recording
  size_t   wavLen;
  uint32_t rawMs;         // what was recorded
  uint32_t speechMs;      // after trimming silence
  int      rmsL, rmsR;    // per-channel level of the raw recording (channel selection evidence)
  int      chUsed;        // 0 = left, 1 = right, 2 = average
  int      peak;          // peak after processing
  bool     silent;        // no frame above the speech threshold
} RecResult;

bool audio_init();                       // after the IO expander has enabled the PA
void audio_set_volume(int pct);          // 0..100
void audio_set_gain_db(float db);

bool audio_rec_start();                  // false if already recording or playing
void audio_rec_stop();                   // async: the recording task finishes and processes
bool audio_rec_active();
uint32_t audio_rec_elapsed_ms();
bool audio_rec_take_result(RecResult* out);   // true once per finished recording

// Playback of 16 kHz mono PCM (any task; one stream at a time)
bool audio_play_begin();
bool audio_play_pcm16(const int16_t* samples, size_t n);   // false once cancelled
void audio_play_end();
void audio_play_cancel();                // any task
bool audio_play_active();
#endif
