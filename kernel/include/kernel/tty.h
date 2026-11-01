#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#include <stddef.h>
#include <stdint.h>



// Структура виртуального терминала
typedef struct {
    uint16_t* buffer;       // Скрытый буфер экрана в куче (80x25 слов)
    size_t column;          // Текущий X
    size_t row;             // Текущий Y
    uint8_t color;          // Цвет текста фона
    char* shell_buf;        // Указатель на динамический буфер командной строки
    size_t shell_idx;       // Текущая длина строки (индекс ввода)
    size_t shell_capacity;  // ВАЖНО: Текущая емкость буфера в байтах
} virtual_tty_t;


void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcolor(uint8_t color);
void terminal_update_cursor(void);
void terminal_setcolor(uint8_t color);

// Функции многооконности
void tty_create_vterms(void);
void tty_switch(int vterm_id);
virtual_tty_t* tty_get_current(void);

#endif
