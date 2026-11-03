#ifndef _KERNEL_PS2_MOUSE_H
#define _KERNEL_PS2_MOUSE_H

#include <stdint.h>

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t buttons;
    uint32_t packets;
} ps2_mouse_state_t;

int ps2_mouse_init(void);
void ps2_mouse_poll(void);
int ps2_mouse_is_available(void);
void ps2_mouse_get_state(ps2_mouse_state_t* state);

#endif
