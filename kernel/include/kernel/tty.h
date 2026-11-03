#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H
#define TTY_DEFAULT_WIDTH 80
#define TTY_DEFAULT_HEIGHT 25
#define TTY_COMMAND_HISTORY_SIZE 8
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
    char* command_history[TTY_COMMAND_HISTORY_SIZE];
    size_t history_count;
    size_t history_next;
    size_t history_position;
    char* history_draft;
    size_t history_draft_length;
    volatile uint8_t command_pending;
    volatile uint8_t command_active;
    volatile uint8_t prompt_pending;
    uint8_t prompt_initialized;
} virtual_tty_t;


void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_clear(void);
void terminal_setcolor(uint8_t color);
void terminal_update_cursor(void);
void terminal_setcolor(uint8_t color);
size_t terminal_columns(void);
size_t terminal_rows(void);

// Функции многооконности
void tty_create_vterms(void);
void tty_switch(int vterm_id);
virtual_tty_t* tty_get_current(void);

#endif
