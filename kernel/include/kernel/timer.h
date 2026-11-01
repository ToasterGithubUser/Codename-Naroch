#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <stdint.h>

// Инициализация таймера PIT с заданной частотой (в Гц)
void timer_install(uint32_t frequency);

// Обработчик прерывания таймера, который будет вызываться из idt.c
void timer_handler(void);

// Функция точной задержки в миллисекундах
void sleep(uint32_t ms);

// Возвращает текущее количество тиков с момента старта ОС
uint32_t timer_get_ticks(void);

#endif
