#include <kernel/keyboard.h>
#include <kernel/tty.h>
#include <kernel/shell.h>
// Вспомогательная функция чтения из I/O порта
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Карта скан-кодов для стандартной американской раскладки (US Layout Set 1)
// Превращает индекс скан-кода в символ ASCII
static const char kbdus[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',  
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',    
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,     
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' '
    // Остальные клавиши (F1-F12, Keypad) при необходимости можно дописать ниже
};

static int left_shift;
static int right_shift;
static int extended_scancode;

static char shifted_character(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A');
    }

    switch (c) {
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case ';': return ':';
        case '\'': return '"';
        case '`': return '~';
        case '\\': return '|';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        default: return c;
    }
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    int extended = extended_scancode;
    extended_scancode = 0;
    uint8_t keycode = scancode & 0x7F;
    int released = (scancode & 0x80) != 0;

    if (!extended && keycode == 0x2A) {
        left_shift = !released;
        return;
    }
    if (!extended && keycode == 0x36) {
        right_shift = !released;
        return;
    }
    if (released) {
        return;
    }

    if (extended) {
        if (keycode == 0x48) {
            shell_history_up();
        } else if (keycode == 0x50) {
            shell_history_down();
        }
        return;
    }

    // --- ПЕРЕХВАТ ГОРЯЧИХ КЛАВИШ F1 И F2 ---
    if (keycode == 0x3B) { // Скан-код F1
        tty_switch(0);      // Переключаемся на Терминал 0
        return;
    }
    if (keycode == 0x3C) { // Скан-код F2
        tty_switch(1);      // Переключаемся на Терминал 1
        return;
    }

    char c = kbdus[keycode];
    if ((left_shift || right_shift) && c) {
        c = shifted_character(c);
    }
    if (c != 0) {
        shell_input_char(c);
    }
}