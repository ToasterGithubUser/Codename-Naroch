#include <kernel/io.h>
#include <kernel/pc_speaker.h>
#include <kernel/timer.h>

#define PIT_CHANNEL_2 0x42
#define PIT_COMMAND 0x43
#define SPEAKER_CONTROL 0x61
#define PIT_FREQUENCY 1193182U

void pc_speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    if (frequency < 20 || frequency > 20000 || !duration_ms) {
        return;
    }

    uint32_t divisor = PIT_FREQUENCY / frequency;
    if (!divisor || divisor > 0xFFFF) {
        return;
    }

    outb(PIT_COMMAND, 0xB6);
    outb(PIT_CHANNEL_2, (uint8_t)divisor);
    outb(PIT_CHANNEL_2, (uint8_t)(divisor >> 8));
    uint8_t control = inb(SPEAKER_CONTROL);
    if (!(control & 3)) {
        outb(SPEAKER_CONTROL, control | 3);
    }

    sleep(duration_ms);
    outb(SPEAKER_CONTROL, inb(SPEAKER_CONTROL) & (uint8_t)~3);
}
