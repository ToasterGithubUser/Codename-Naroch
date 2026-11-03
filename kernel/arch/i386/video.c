#include <kernel/paging.h>
#include <kernel/video.h>
#include <string.h>

#define MULTIBOOT_INFO_FRAMEBUFFER (1U << 12)
#define VIDEO_FRAMEBUFFER_VIRTUAL 0x3E000000U
#define VIDEO_MAX_FRAMEBUFFER_SIZE (16U * 1024U * 1024U)
#define VIDEO_GLYPH_WIDTH 8
#define VIDEO_GLYPH_HEIGHT 16

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
    uint8_t red_position;
    uint8_t red_size;
    uint8_t green_position;
    uint8_t green_size;
    uint8_t blue_position;
    uint8_t blue_size;
} video_mode_t;

static volatile uint8_t* framebuffer;
static uint8_t font[256][VIDEO_GLYPH_HEIGHT];
static uint32_t framebuffer_pitch;
static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint8_t framebuffer_bpp;
static uint8_t red_position;
static uint8_t red_size;
static uint8_t green_position;
static uint8_t green_size;
static uint8_t blue_position;
static uint8_t blue_size;

static int video_load_bios_font(void) {
    uint32_t vector;
    asm volatile("movl 0x10c, %0" : "=r"(vector));
    uint16_t offset = (uint16_t)vector;
    uint16_t segment = (uint16_t)(vector >> 16);
    uint32_t address = (uint32_t)segment * 16 + offset;
    if (address < 0x10000 || address >= 0x100000) {
        return 0;
    }

    uint32_t nonzero_glyphs = 0;
    int space_is_blank = 1;
    int capital_a_is_visible = 0;
    for (uint32_t character = 0; character < 256; character++) {
        int nonzero = 0;
        for (uint32_t row = 0; row < VIDEO_GLYPH_HEIGHT; row++) {
            uint32_t font_address =
                (address + character * VIDEO_GLYPH_HEIGHT + row) & 0xFFFFFU;
            uint8_t bits = *(const volatile uint8_t*)font_address;
            font[character][row] = bits;
            nonzero |= bits != 0;
            if (character == ' ' && bits) {
                space_is_blank = 0;
            }
            if (character == 'A' && bits) {
                capital_a_is_visible = 1;
            }
        }
        if (character >= 33 && character <= 126) {
            nonzero_glyphs += nonzero;
        }
    }
    return space_is_blank && capital_a_is_visible && nonzero_glyphs > 80;
}

static uint32_t channel_value(uint8_t value, uint8_t size) {
    if (!size) {
        return 0;
    }
    uint32_t maximum = (1U << size) - 1;
    return ((uint32_t)value * maximum + 127) / 255;
}

static uint32_t pack_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return (channel_value(red, red_size) << red_position) |
        (channel_value(green, green_size) << green_position) |
        (channel_value(blue, blue_size) << blue_position);
}

static uint32_t color_for_attribute(uint8_t attribute, int foreground) {
    static const uint8_t palette[16][3] = {
        {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
        {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
        {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
        {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}
    };
    uint8_t index = foreground ? attribute & 0x0F : (attribute >> 4) & 0x0F;
    return pack_rgb(palette[index][0], palette[index][1], palette[index][2]);
}

void video_init(const multiboot_info_t* boot_info) {
    framebuffer = 0;
    framebuffer_pitch = framebuffer_width = framebuffer_height = 0;
    if (!boot_info || !(boot_info->flags & MULTIBOOT_INFO_FRAMEBUFFER) ||
        (uint32_t)(boot_info->framebuffer_addr >> 32) ||
        boot_info->framebuffer_type != 1) {
        return;
    }
    if (boot_info->framebuffer_bpp != 24 &&
        boot_info->framebuffer_bpp != 32) {
        return;
    }
    if (!boot_info->framebuffer_width || !boot_info->framebuffer_height ||
        boot_info->framebuffer_width > 4096 ||
        boot_info->framebuffer_height > 2160) {
        return;
    }

    uint32_t physical_address = (uint32_t)boot_info->framebuffer_addr;
    uint32_t physical_page = physical_address & ~0xFFFU;
    uint32_t page_offset = physical_address & 0xFFFU;
    uint32_t bytes_per_pixel = boot_info->framebuffer_bpp / 8;
    uint64_t size = (uint64_t)boot_info->framebuffer_pitch *
        boot_info->framebuffer_height;
    if (boot_info->framebuffer_pitch <
            boot_info->framebuffer_width * bytes_per_pixel ||
        !size || size > VIDEO_MAX_FRAMEBUFFER_SIZE ||
        size + page_offset > 0xFFFFFFFFULL ||
        (uint64_t)physical_address + size > 0x100000000ULL ||
        !paging_map_kernel_mmio(VIDEO_FRAMEBUFFER_VIRTUAL,
            physical_page, (uint32_t)size + page_offset)) {
        return;
    }

    red_position = boot_info->framebuffer_color_info.rgb.red_field_position;
    red_size = boot_info->framebuffer_color_info.rgb.red_mask_size;
    green_position = boot_info->framebuffer_color_info.rgb.green_field_position;
    green_size = boot_info->framebuffer_color_info.rgb.green_mask_size;
    blue_position = boot_info->framebuffer_color_info.rgb.blue_field_position;
    blue_size = boot_info->framebuffer_color_info.rgb.blue_mask_size;
    if (!red_size && red_position + 8 <= boot_info->framebuffer_bpp) {
        red_size = 8;
    }
    if (!green_size && green_position + 8 <= boot_info->framebuffer_bpp) {
        green_size = 8;
    }
    if (!blue_size && blue_position + 8 <= boot_info->framebuffer_bpp) {
        blue_size = 8;
    }
    if (red_position + red_size > boot_info->framebuffer_bpp ||
        green_position + green_size > boot_info->framebuffer_bpp ||
        blue_position + blue_size > boot_info->framebuffer_bpp) {
        red_position = 16;
        red_size = 8;
        green_position = 8;
        green_size = 8;
        blue_position = 0;
        blue_size = 8;
    }
    if (!video_load_bios_font()) {
        return;
    }

    framebuffer = (volatile uint8_t*)(VIDEO_FRAMEBUFFER_VIRTUAL + page_offset);
    framebuffer_pitch = boot_info->framebuffer_pitch;
    framebuffer_width = boot_info->framebuffer_width;
    framebuffer_height = boot_info->framebuffer_height;
    framebuffer_bpp = boot_info->framebuffer_bpp;
}

int video_is_framebuffer(void) {
    return framebuffer != 0;
}

uint32_t video_width(void) {
    return framebuffer_width;
}

uint32_t video_height(void) {
    return framebuffer_height;
}

uint32_t video_bpp(void) {
    return framebuffer_bpp;
}

uint32_t video_pitch(void) {
    return framebuffer_pitch;
}

static void video_store_pixel(volatile uint8_t* destination, uint32_t pixel) {
    uint32_t bytes_per_pixel = framebuffer_bpp / 8;
    for (uint32_t byte = 0; byte < bytes_per_pixel; byte++) {
        destination[byte] = (uint8_t)(pixel >> (byte * 8));
    }
}

void video_clear(uint8_t color) {
    if (!framebuffer) {
        return;
    }
    uint32_t background = color_for_attribute(color, 0);
    for (uint32_t y = 0; y < framebuffer_height; y++) {
        volatile uint8_t* row = framebuffer + y * framebuffer_pitch;
        for (uint32_t x = 0; x < framebuffer_width; x++) {
            video_store_pixel(row + x * (framebuffer_bpp / 8), background);
        }
    }
}

void video_draw_cell(uint32_t column, uint32_t row, uint8_t character,
                     uint8_t color) {
    if (!framebuffer || column >= framebuffer_width / VIDEO_GLYPH_WIDTH ||
        row >= framebuffer_height / VIDEO_GLYPH_HEIGHT) {
        return;
    }

    uint32_t foreground = color_for_attribute(color, 1);
    uint32_t background = color_for_attribute(color, 0);
    uint32_t x_start = column * VIDEO_GLYPH_WIDTH;
    uint32_t y_start = row * VIDEO_GLYPH_HEIGHT;
    for (uint32_t y = 0; y < VIDEO_GLYPH_HEIGHT; y++) {
        volatile uint8_t* pixels =
            framebuffer + (y_start + y) * framebuffer_pitch +
            x_start * (framebuffer_bpp / 8);
        uint8_t bits = font[character][y];
        for (uint32_t x = 0; x < VIDEO_GLYPH_WIDTH; x++) {
            video_store_pixel(pixels + x * (framebuffer_bpp / 8),
                              bits & (0x80 >> x) ? foreground : background);
        }
    }
}
