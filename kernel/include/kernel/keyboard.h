#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>

// Функция, которую будет вызывать IDT при прерывании IRQ 1
void keyboard_handler(void);

#endif
