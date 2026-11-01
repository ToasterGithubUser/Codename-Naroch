#ifndef _KERNEL_IO_H
#define _KERNEL_IO_H

#include <stdint.h>

// Отправка 1 байта в порт
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Чтение 1 байта из порта
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif
