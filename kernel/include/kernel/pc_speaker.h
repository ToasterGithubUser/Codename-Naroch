#ifndef _KERNEL_PC_SPEAKER_H
#define _KERNEL_PC_SPEAKER_H

#include <stdint.h>

void pc_speaker_beep(uint32_t frequency, uint32_t duration_ms);

#endif
