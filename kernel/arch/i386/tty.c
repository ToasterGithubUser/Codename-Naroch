#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include "vga.h"
#include <kernel/mutex.h>
#include <kernel/serial.h>
#include <kernel/video.h>

// Мьютекс для защиты физического экрана VGA и каретки TTY
mutex_t tty_mutex; 
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;
static virtual_tty_t* vterms[2];
static int active_vterm = 0;
static size_t terminal_width = TTY_DEFAULT_WIDTH;
static size_t terminal_height = TTY_DEFAULT_HEIGHT;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void tty_create_vterms(void) {
    terminal_width = video_is_framebuffer()
        ? video_width() / 8 : TTY_DEFAULT_WIDTH;
    terminal_height = video_is_framebuffer()
        ? video_height() / 16 : TTY_DEFAULT_HEIGHT;
    if (!terminal_width || !terminal_height) {
        terminal_width = TTY_DEFAULT_WIDTH;
        terminal_height = TTY_DEFAULT_HEIGHT;
    }

    for (int i = 0; i < 2; i++) {
        vterms[i] = (virtual_tty_t*)kmalloc(sizeof(virtual_tty_t));
        vterms[i]->buffer = (uint16_t*)kmalloc(
            terminal_width * terminal_height * sizeof(uint16_t));
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
        vterms[i]->prompt_pending = 0;
        vterms[i]->prompt_initialized = 0;
        for (size_t j = 0; j < TTY_COMMAND_HISTORY_SIZE; j++) {
            vterms[i]->command_history[j] = 0;
        }

        for (size_t y = 0; y < terminal_height; y++) {
            for (size_t x = 0; x < terminal_width; x++) {
                vterms[i]->buffer[y * terminal_width + x] =
                    vga_entry(' ', vterms[i]->color);
            }
        }
    }
    active_vterm = 0;
}

void tty_switch(int vterm_id) {
    if (vterm_id < 0 || vterm_id >= 2) return;
    
    if (!video_is_framebuffer()) {
        for (size_t i = 0; i < terminal_width * terminal_height; i++) {
            vterms[active_vterm]->buffer[i] = VGA_MEMORY[i];
        }
    }
    
    // 2. Переключаем индекс
    active_vterm = vterm_id;
    if (!vterms[active_vterm]->prompt_initialized) {
        vterms[active_vterm]->prompt_pending = 1;
    }
    
    // 3. Выводим данные нового TTY на экран
    for (size_t i = 0; i < terminal_width * terminal_height; i++) {
        uint16_t cell = vterms[active_vterm]->buffer[i];
        if (video_is_framebuffer()) {
            video_draw_cell((uint32_t)(i % terminal_width),
                            (uint32_t)(i / terminal_width),
                            (uint8_t)cell, (uint8_t)(cell >> 8));
        } else {
            VGA_MEMORY[i] = cell;
        }
    }
    
    terminal_update_cursor();
}

virtual_tty_t* tty_get_current(void) {
    return vterms[active_vterm];
}
// Внутренняя функция скроллинга конкретного терминала
static void terminal_scroll(virtual_tty_t* tty) {
    // 1. Вычисляем размер одной строки в байтах: 80 символов * 2 байта на символ (символ + атрибут цвета)
    // 2. Сдвигаем строки с 1 по 24 на место строк с 0 по 23 в локальном буфере окна
    // Мы копируем (25 - 1) строк, пропуская самую первую верхнюю строку
    for (size_t y = 0; y < terminal_height - 1; y++) {
        for (size_t x = 0; x < terminal_width; x++) {
            tty->buffer[y * terminal_width + x] =
                tty->buffer[(y + 1) * terminal_width + x];
        }
    }

    // 3. Очищаем самую нижнюю (24-ю) строку пробелами, подготавливая её для нового ввода
    for (size_t x = 0; x < terminal_width; x++) {
        tty->buffer[(terminal_height - 1) * terminal_width + x] =
            vga_entry(' ', tty->color);
    }

    // 4. Переносим каретку ввода строго на начало последней строки
    tty->row = terminal_height - 1;

    // 5. Обновляем физический экран VGA (0xB8000) из буфера TTY, чтобы пользователь увидел сдвиг
    // (Делаем это только если данный терминал сейчас активен на экране)
    virtual_tty_t* active_tty = tty_get_current();
    if (tty == active_tty) {
        for (size_t i = 0; i < terminal_width * terminal_height; i++) {
            uint16_t cell = tty->buffer[i];
            if (video_is_framebuffer()) {
                video_draw_cell((uint32_t)(i % terminal_width),
                                (uint32_t)(i / terminal_width),
                                (uint8_t)cell, (uint8_t)(cell >> 8));
            } else {
                VGA_MEMORY[i] = cell;
            }
        }
    }
}
void terminal_initialize(void) {
    mutex_init(&tty_mutex); // Инициализация!
	tty_create_vterms();
    if (video_is_framebuffer()) {
        video_clear(vterms[0]->color);
    } else {
        for (size_t y = 0; y < terminal_height; y++) {
            for (size_t x = 0; x < terminal_width; x++) {
                VGA_MEMORY[y * terminal_width + x] =
                    vga_entry(' ', vterms[0]->color);
            }
        }
    }
}

void terminal_update_cursor(void) {
    if (video_is_framebuffer()) {
        return;
    }
    virtual_tty_t* tty = vterms[active_vterm];
    uint16_t pos = tty->row * terminal_width + tty->column;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t) (pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t) ((pos >> 8) & 0xFF));
}

void terminal_setcolor(uint8_t color) {
	vterms[active_vterm]->color = color;
}

size_t terminal_columns(void) {
    return terminal_width;
}

size_t terminal_rows(void) {
    return terminal_height;
}

void terminal_clear(void) {
    mutex_lock(&tty_mutex);
    virtual_tty_t* tty = vterms[active_vterm];
    tty->row = 0;
    tty->column = 0;
    for (size_t i = 0; i < terminal_width * terminal_height; i++) {
        tty->buffer[i] = vga_entry(' ', tty->color);
    }
    if (video_is_framebuffer()) {
        video_clear(tty->color);
    } else {
        for (size_t i = 0; i < terminal_width * terminal_height; i++) {
            VGA_MEMORY[i] = tty->buffer[i];
        }
    }
    terminal_update_cursor();
    mutex_unlock(&tty_mutex);
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
            if (++tty->row >= terminal_height) {
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
            if (tty->column >= terminal_width) {
                tty->column = 0;
                // ИСПРАВЛЕНО: Табуляция тоже может переполнить строку
                if (++tty->row >= terminal_height) {
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
                tty->column = terminal_width - 1;
            } else {
                mutex_unlock(&tty_mutex);
                return;
            }
            // Стираем символ как в локальном буфере кучи, так и на физическом экране
            tty->buffer[tty->row * terminal_width + tty->column] =
                vga_entry(' ', tty->color);
            if (video_is_framebuffer()) {
                video_draw_cell((uint32_t)tty->column, (uint32_t)tty->row,
                                ' ', tty->color);
            } else {
                VGA_MEMORY[tty->row * terminal_width + tty->column] =
                    vga_entry(' ', tty->color);
            }
            
            terminal_update_cursor();
            mutex_unlock(&tty_mutex);
            return;

        default:
            // Пишем символ сначала в локальный бэкап-буфер виртуального TTY
            tty->buffer[tty->row * terminal_width + tty->column] =
                vga_entry(uc, tty->color);
            
            // Затем дублируем на физический VGA экран
            if (video_is_framebuffer()) {
                video_draw_cell((uint32_t)tty->column, (uint32_t)tty->row,
                                uc, tty->color);
            } else {
                VGA_MEMORY[tty->row * terminal_width + tty->column] =
                    vga_entry(uc, tty->color);
            }
            
            if (++tty->column >= terminal_width) {
                tty->column = 0;
                // ИСПРАВЛЕНО: Обычный текст переходит на новую строку со скроллингом
                if (++tty->row >= terminal_height) {
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
