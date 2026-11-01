#include <kernel/timer.h>
#include <kernel/io.h>

// Переменная должна быть глобальной (без static!)
volatile uint32_t timer_ticks = 0;
static uint32_t sys_frequency = 100;

void timer_install(uint32_t frequency) {
    sys_frequency = frequency;
    uint32_t divisor = 1193182 / frequency;

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_handler(void) {
    timer_ticks++;
}

uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

void sleep(uint32_t ms) {
    uint32_t ticks_to_wait = (ms * sys_frequency) / 1000;
    if (ticks_to_wait == 0) ticks_to_wait = 1;

    uint32_t start_ticks = timer_ticks;
    uint32_t eticks = start_ticks + ticks_to_wait;

    // Жесткий цикл ожидания (Busy Loop) без ухода процессора в сон
    while (timer_ticks < eticks) {
        // 1. Принудительно разрешаем прерывания на каждой итерации, 
        // на случай, если какая-то подсистема случайно выполнила cli
        asm volatile("sti"); 
        
        // 2. Барьер памяти запрещает GCC кэшировать timer_ticks в регистрах
        asm volatile("" : : : "memory"); 
    }
}
