#ifndef LVGL_PORT_H
#define LVGL_PORT_H
#include <stdbool.h>



#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
void lvgl_port_set_flipped(bool flipped);   // TranslateOS: LVGL task only


#ifdef __cplusplus
}
#endif



#endif










