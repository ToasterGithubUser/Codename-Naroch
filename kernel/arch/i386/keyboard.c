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

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    if (scancode & 0x80) {
        return; 
    }
    // --- ПЕРЕХВАТ ГОРЯЧИХ КЛАВИШ F1 И F2 ---
    if (scancode == 0x3B) { // Скан-код F1
        tty_switch(0);      // Переключаемся на Терминал 0
        return;
    }
    if (scancode == 0x3C) { // Скан-код F2
        tty_switch(1);      // Переключаемся на Терминал 1
        return;
    }
    if (scancode < 128) {
        char c = kbdus[scancode];
        if (c != 0) {
            // ВМЕСТО terminal_putchar(c) вызываем наш обработчик Shell:
            shell_input_char(c); 
        }
    }
}