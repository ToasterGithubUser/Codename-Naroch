#include <kernel/io.h>
#include <kernel/serial.h>

#define SERIAL_COM1 0x3F8
#define SERIAL_DATA (SERIAL_COM1 + 0)
#define SERIAL_INTERRUPT_ENABLE (SERIAL_COM1 + 1)
#define SERIAL_FIFO_CONTROL (SERIAL_COM1 + 2)
#define SERIAL_LINE_CONTROL (SERIAL_COM1 + 3)
#define SERIAL_MODEM_CONTROL (SERIAL_COM1 + 4)
#define SERIAL_LINE_STATUS (SERIAL_COM1 + 5)
#define SERIAL_WAIT_LIMIT 100000U

static int serial_available;

void serial_init(void) {
    serial_available = 0;

    outb(SERIAL_INTERRUPT_ENABLE, 0x00);
    outb(SERIAL_LINE_CONTROL, 0x80);
    outb(SERIAL_DATA, 0x01);
    outb(SERIAL_INTERRUPT_ENABLE, 0x00);
    outb(SERIAL_LINE_CONTROL, 0x03);
    outb(SERIAL_FIFO_CONTROL, 0xC7);
    outb(SERIAL_MODEM_CONTROL, 0x1E);
    outb(SERIAL_DATA, 0xAE);

    if (inb(SERIAL_DATA) != 0xAE) {
        outb(SERIAL_MODEM_CONTROL, 0x0B);
        return;
    }

    outb(SERIAL_MODEM_CONTROL, 0x0B);
    serial_available = 1;
}

int serial_is_available(void) {
    return serial_available;
}

static int serial_wait_transmit(void) {
    for (uint32_t wait = 0; wait < SERIAL_WAIT_LIMIT; wait++) {
        if (inb(SERIAL_LINE_STATUS) & 0x20) {
            return 1;
        }
    }
    serial_available = 0;
    return 0;
}

static int serial_transmit_byte(uint8_t byte) {
    if (!serial_wait_transmit()) {
        return 0;
    }
    outb(SERIAL_DATA, byte);
    return 1;
}

int serial_write_char(char character) {
    if (!serial_available) {
        return 0;
    }
    if (character == '\n') {
        if (!serial_transmit_byte('\r')) {
            return 0;
        }
        return serial_transmit_byte('\n');
    }
    if (character == '\b') {
        return serial_transmit_byte('\b') &&
            serial_transmit_byte(' ') &&
            serial_transmit_byte('\b');
    }
    return serial_transmit_byte((uint8_t)character);
}

size_t serial_write(const char* data, size_t size) {
    if (!data || !serial_available) {
        return 0;
    }
    size_t written = 0;
    while (written < size && serial_write_char(data[written])) {
        written++;
    }
    return written;
}

int serial_read_char(uint8_t* character) {
    if (!character || !serial_available ||
        !(inb(SERIAL_LINE_STATUS) & 0x01)) {
        return 0;
    }
    *character = inb(SERIAL_DATA);
    return 1;
}
