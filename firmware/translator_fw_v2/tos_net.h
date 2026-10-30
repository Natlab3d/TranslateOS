// Network jobs (Gemini translation + Google TTS) on their own task, so the UI and buttons never wait
// for the network. Results go to the UI through ui_post_*; the app is told through app_net_done().
#ifndef TRANSLATEOS_NET_H_
#define TRANSLATEOS_NET_H_
#include <stdint.h>
#include <stddef.h>

void net_init();
bool net_busy();
void net_cancel();                                   // any task; aborts the current job quickly

// wav stays owned by the audio module; it is not touched until the next recording, and the app
// never starts a recording while net_busy().
bool net_submit_translate(const uint8_t* wav, size_t len, int srcLang, int dstLang);
bool net_submit_speak_pair(int pairIndex);           // replay a history pair (cached audio if latest)

// History of pairs shown in the chat (app side copy, for replay and saving).
int  net_history_count();
void net_history_clear();

// Implemented by the app (.ino): called from the net task when a job ends.
void app_net_done(bool ok);
#endif
