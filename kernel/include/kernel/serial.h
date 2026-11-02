#ifndef _KERNEL_SERIAL_H
#define _KERNEL_SERIAL_H

#include <stddef.h>
#include <stdint.h>

void serial_init(void);
int serial_is_available(void);
int serial_write_char(char character);
size_t serial_write(const char* data, size_t size);
int serial_read_char(uint8_t* character);

#endif
