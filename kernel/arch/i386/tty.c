#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include "vga.h"
#include <kernel/mutex.h>
#include <kernel/serial.h>

// Мьютекс для защиты физического экрана VGA и каретки TTY
mutex_t tty_mutex; 
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;
static virtual_tty_t* vterms[2];
static int active_vterm = 0;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void tty_create_vterms(void) {
    for (int i = 0; i < 2; i++) {
        vterms[i] = (virtual_tty_t*)kmalloc(sizeof(virtual_tty_t));
        vterms[i]->buffer = (uint16_t*)kmalloc(VGA_WIDTH * VGA_HEIGHT * sizeof(uint16_t));
        vterms[i]->column = 0;
        vterms[i]->row = 0;
        vterms[i]->color = vga_entry_color(15, 0); // Белый на черном
        
        vterms[i]->shell_capacity = 32; // Стартовая емкость
        vterms[i]->shell_buf = (char*)kmalloc(vterms[i]->shell_capacity);
        vterms[i]->shell_idx = 0;
        vterms[i]->history_count = 0;
        vterms[i]->history_next = 0;
        vterms[i]->history_position = 0;
        vterms[i]->history_draft = 0;
        vterms[i]->history_draft_length = 0;
        vterms[i]->command_pending = 0;
        vterms[i]->command_active = 0;
        for (size_t j = 0; j < TTY_COMMAND_HISTORY_SIZE; j++) {
            vterms[i]->command_history[j] = 0;
        }

        for (size_t y = 0; y < VGA_HEIGHT; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                vterms[i]->buffer[y * VGA_WIDTH + x] = vga_entry(' ', vterms[i]->color);
            }
        }
    }
    active_vterm = 0;
}

void tty_switch(int vterm_id) {
    if (vterm_id < 0 || vterm_id >= 2) return;
    
    // Preserve writes made directly to the VGA buffer by legacy code.
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vterms[active_vterm]->buffer[i] = VGA_MEMORY[i];
    }
    
    // 2. Переключаем индекс
    active_vterm = vterm_id;
    
    // 3. Выводим данные нового TTY на экран
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEMORY[i] = vterms[active_vterm]->buffer[i];
    }
    
    terminal_update_cursor();
}

virtual_tty_t* tty_get_current(void) {
    return vterms[active_vterm];
}
// Внутренняя функция скроллинга конкретного терминала
static void terminal_scroll(virtual_tty_t* tty) {
    // 1. Вычисляем размер одной строки в байтах: 80 символов * 2 байта на символ (символ + атрибут цвета)
    size_t line_bytes = VGA_WIDTH * sizeof(uint16_t);
    
    // 2. Сдвигаем строки с 1 по 24 на место строк с 0 по 23 в локальном буфере окна
    // Мы копируем (25 - 1) строк, пропуская самую первую верхнюю строку
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            tty->buffer[y * VGA_WIDTH + x] = tty->buffer[(y + 1) * VGA_WIDTH + x];
        }
    }

    // 3. Очищаем самую нижнюю (24-ю) строку пробелами, подготавливая её для нового ввода
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        tty->buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', tty->color);
    }

    // 4. Переносим каретку ввода строго на начало последней строки
    tty->row = VGA_HEIGHT - 1;

    // 5. Обновляем физический экран VGA (0xB8000) из буфера TTY, чтобы пользователь увидел сдвиг
    // (Делаем это только если данный терминал сейчас активен на экране)
    virtual_tty_t* active_tty = tty_get_current();
    if (tty == active_tty) {
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            VGA_MEMORY[i] = tty->buffer[i];
        }
    }
}
void terminal_initialize(void) {
    mutex_init(&tty_mutex); // Инициализация!
	tty_create_vterms();
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', vterms[0]->color);
        }
    }
}

void terminal_update_cursor(void) {
    virtual_tty_t* tty = vterms[active_vterm];
    uint16_t pos = tty->row * VGA_WIDTH + tty->column;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t) (pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t) ((pos >> 8) & 0xFF));
}

void terminal_setcolor(uint8_t color) {
	vterms[active_vterm]->color = color;
}

void terminal_putchar(char c) {
    serial_write_char(c);
    mutex_lock(&tty_mutex); // Защищаем вывод мьютексом от Race Condition!

    virtual_tty_t* tty = vterms[active_vterm];
    unsigned char uc = c;

    switch (uc) {
        case '\n':
            tty->column = 0;
            // ИСПРАВЛЕНО: Вместо сброса в 0 вызываем скроллинг!
            if (++tty->row >= VGA_HEIGHT) {
                terminal_scroll(tty);
            }
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;

        case '\r':
            tty->column = 0;
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;

        case '\t':
            tty->column = ((tty->column + 4) & ~3);
            if (tty->column >= VGA_WIDTH) {
                tty->column = 0;
                // ИСПРАВЛЕНО: Табуляция тоже может переполнить строку
                if (++tty->row >= VGA_HEIGHT) {
                    terminal_scroll(tty);
                }
            }
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;

        case '\b':
            if (tty->column > 0) {
                tty->column--;
            } else if (tty->row > 0) {
                tty->row--;
                tty->column = VGA_WIDTH - 1;
            } else {
                mutex_unlock(&tty_mutex);
                return;
            }
            // Стираем символ как в локальном буфере кучи, так и на физическом экране
            tty->buffer[tty->row * VGA_WIDTH + tty->column] = vga_entry(' ', tty->color);
            VGA_MEMORY[tty->row * VGA_WIDTH + tty->column] = vga_entry(' ', tty->color);
            
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;

        default:
            // Пишем символ сначала в локальный бэкап-буфер виртуального TTY
            tty->buffer[tty->row * VGA_WIDTH + tty->column] = vga_entry(uc, tty->color);
            
            // Затем дублируем на физический VGA экран
            VGA_MEMORY[tty->row * VGA_WIDTH + tty->column] = vga_entry(uc, tty->color);
            
            if (++tty->column >= VGA_WIDTH) {
                tty->column = 0;
                // ИСПРАВЛЕНО: Обычный текст переходит на новую строку со скроллингом
                if (++tty->row >= VGA_HEIGHT) {
                    terminal_scroll(tty);
                }
            }
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;
    }
}

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++) {
        terminal_putchar(data[i]);
    }
}

void terminal_writestring(const char* data) {
	terminal_write(data, strlen(data));
}
