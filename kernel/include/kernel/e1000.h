#ifndef _KERNEL_E1000_H
#define _KERNEL_E1000_H

#include <stdint.h>

int e1000_init(void);
int e1000_is_available(void);
const uint8_t* e1000_mac_address(void);
int e1000_send(const void* frame, uint16_t length);
int e1000_receive(void* frame, uint16_t capacity);

#endif
