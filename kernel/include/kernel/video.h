#ifndef _KERNEL_VIDEO_H
#define _KERNEL_VIDEO_H

#include <kernel/multiboot.h>
#include <stddef.h>
#include <stdint.h>

void video_init(const multiboot_info_t* boot_info);
int video_is_framebuffer(void);
uint32_t video_width(void);
uint32_t video_height(void);
uint32_t video_bpp(void);
uint32_t video_pitch(void);
void video_clear(uint8_t color);
void video_draw_cell(uint32_t column, uint32_t row, uint8_t character,
                     uint8_t color);

#endif
