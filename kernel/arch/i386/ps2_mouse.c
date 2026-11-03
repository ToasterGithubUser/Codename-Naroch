#include <kernel/io.h>
#include <kernel/ps2_mouse.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_COMMAND 0x64
#define PS2_INPUT_FULL 0x02
#define PS2_OUTPUT_FULL 0x01
#define PS2_AUXILIARY_DATA 0x20
#define PS2_WAIT_LIMIT 100000U
#define MOUSE_PACKET_SIZE 3
#define MOUSE_MAX_X 639
#define MOUSE_MAX_Y 399

static ps2_mouse_state_t mouse_state;
static uint8_t packet[MOUSE_PACKET_SIZE];
static uint32_t packet_index;
static int available;

static int wait_input_empty(void) {
    for (uint32_t wait = 0; wait < PS2_WAIT_LIMIT; wait++) {
        if (!(inb(PS2_STATUS) & PS2_INPUT_FULL)) {
            return 1;
        }
    }
    return 0;
}

static int wait_output_full(void) {
    for (uint32_t wait = 0; wait < PS2_WAIT_LIMIT; wait++) {
        if (inb(PS2_STATUS) & PS2_OUTPUT_FULL) {
            return 1;
        }
    }
    return 0;
}

static int read_controller_byte(uint8_t* value) {
    if (!value || !wait_output_full()) {
        return 0;
    }
    *value = inb(PS2_DATA);
    return 1;
}

static int send_mouse_byte(uint8_t value) {
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND, 0xD4);
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_DATA, value);

    for (uint32_t wait = 0; wait < PS2_WAIT_LIMIT; wait++) {
        if (inb(PS2_STATUS) & PS2_OUTPUT_FULL) {
            uint8_t status = inb(PS2_STATUS);
            uint8_t response = inb(PS2_DATA);
            if ((status & PS2_AUXILIARY_DATA) && response == 0xFA) {
                return 1;
            }
        }
    }
    return 0;
}

int ps2_mouse_init(void) {
    available = 0;
    packet_index = 0;
    mouse_state.x = MOUSE_MAX_X / 2;
    mouse_state.y = MOUSE_MAX_Y / 2;
    mouse_state.buttons = 0;
    mouse_state.packets = 0;

    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND, 0xA8);

    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND, 0x20);
    uint8_t config;
    if (!read_controller_byte(&config)) {
        return 0;
    }

    config &= (uint8_t)~((1U << 5) | (1U << 1));
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND, 0x60);
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_DATA, config);

    if (!send_mouse_byte(0xF6) || !send_mouse_byte(0xF4)) {
        return 0;
    }
    available = 1;
    return 1;
}

int ps2_mouse_is_available(void) {
    return available;
}

static void decode_packet(void) {
    uint8_t flags = packet[0];
    if (flags & 0xC0) {
        return;
    }

    int16_t delta_x = packet[1];
    int16_t delta_y = packet[2];
    if (flags & 0x10) {
        delta_x |= (int16_t)0xFF00;
    }
    if (flags & 0x20) {
        delta_y |= (int16_t)0xFF00;
    }

    int32_t x = mouse_state.x + delta_x;
    int32_t y = mouse_state.y - delta_y;
    if (x < 0) {
        x = 0;
    } else if (x > MOUSE_MAX_X) {
        x = MOUSE_MAX_X;
    }
    if (y < 0) {
        y = 0;
    } else if (y > MOUSE_MAX_Y) {
        y = MOUSE_MAX_Y;
    }
    mouse_state.x = (uint16_t)x;
    mouse_state.y = (uint16_t)y;
    mouse_state.buttons = flags & 0x07;
    mouse_state.packets++;
}

void ps2_mouse_poll(void) {
    if (!available) {
        return;
    }
    for (uint32_t count = 0; count < 16; count++) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & PS2_OUTPUT_FULL) ||
            !(status & PS2_AUXILIARY_DATA)) {
            break;
        }
        uint8_t value = inb(PS2_DATA);
        if (!packet_index && !(value & 0x08)) {
            continue;
        }
        packet[packet_index++] = value;
        if (packet_index == MOUSE_PACKET_SIZE) {
            packet_index = 0;
            decode_packet();
        }
    }
}

void ps2_mouse_get_state(ps2_mouse_state_t* state) {
    if (!state) {
        return;
    }
    *state = mouse_state;
}
