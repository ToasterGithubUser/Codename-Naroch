#include <stdio.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/tty.h>
#include <kernel/shell.h>
#include <kernel/build_no.h> 
#include <kernel/conversions.h>
#include <kernel/timer.h>
#include <kernel/pmm.h>
#include <kernel/multiboot.h>
#include <kernel/paging.h>
#include <kernel/heap.h>
#include <kernel/task.h>
#include <kernel/fat16.h>
#include <kernel/ring3.h>
#include <kernel/serial.h>
#include <kernel/rtc.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/pci.h>
#include <kernel/net.h>
#include <kernel/ps2_mouse.h>
#include <kernel/video.h>
#include <string.h>
// Объявляем возможные альтернативные имена, которые генерируют разные версии linker.ld
extern char _kernel_start    __attribute__((weak));
extern char _kernel_end      __attribute__((weak));
extern char kernel_start     __attribute__((weak));
extern char kernel_end       __attribute__((weak));
extern char start            __attribute__((weak));
extern char end              __attribute__((weak));
extern char user_mode_demo_start[];
extern char user_mode_demo_end[];

volatile ring3_test_state_t* ring3_test_state;

static void idle_task(void) {
    shell_prompt();
    for (;;) {
        shell_process_pending();
        net_poll();
        ps2_mouse_poll();
        uint8_t character;
        while (serial_read_char(&character)) {
            if (character == '\r') {
                character = '\n';
            } else if (character == 0x7F) {
                character = '\b';
            }
            if (!syscall_console_input((char)character)) {
                shell_input_char((char)character);
            }
        }
        asm volatile("sti; hlt");
    }
}

static inline uint32_t get_kernel_start(void) {
    if (&_kernel_start != NULL) return (uint32_t)&_kernel_start;
    if (&kernel_start != NULL)  return (uint32_t)&kernel_start;
    if (&start != NULL)         return (uint32_t)&start;
    return 0x100000; // Дефолтный физический адрес загрузки ядра (1 МБ)
}

static inline uint32_t get_kernel_end(void) {
    if (&_kernel_end != NULL) return (uint32_t)&_kernel_end;
    if (&kernel_end != NULL)  return (uint32_t)&kernel_end;
    if (&end != NULL)         return (uint32_t)&end;
    return 0x400000; // Фоллбэк: если метка не найдена, берем безопасный лимит в 4 МБ
}

extern uint32_t pmm_max_blocks;
void dummy_task_A(void) {
    while (1) {
        printf("[A]");
        sleep(500); // Спим 500 мс (внутри вызывается hlt, отдавая квант времени)
    }
}

// Тестовый поток ядра №2
void dummy_task_B(void) {
    while (1) {
        printf("[B]");
        sleep(300); // Спим 300 мс
    }
}
void kernel_main(uint32_t magic, multiboot_info_t* mboot_ptr) {
    gdt_install();
    idt_install();
    if (magic != 0x2BADB002) while(1);

    uint32_t total_memory = 0;
    if (mboot_ptr->flags & (1 << 0)) {
        total_memory = (mboot_ptr->mem_upper * 1024) + 0x100000;
    } else {
        total_memory = 64 * 1024 * 1024; 
    }
    
    uint32_t pmm_bitmap_start = (get_kernel_end() + 4095) & ~4095; 
    pmm_init(total_memory, pmm_bitmap_start);

    if (mboot_ptr->flags & (1 << 6)) {
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mboot_ptr->mmap_addr;
        uint32_t mmap_end = mboot_ptr->mmap_addr + mboot_ptr->mmap_length;

        while ((uint32_t)mmap < mmap_end) {
            if (mmap->type == 1) {
                uint32_t base_addr = (uint32_t)mmap->addr;
                uint32_t region_size = (uint32_t)mmap->len;
                
                if (base_addr < 0x100000) {
                    if (base_addr + region_size > 0x100000) {
                        region_size = (base_addr + region_size) - 0x100000;
                        base_addr = 0x100000;
                    } else {
                        goto next_region;
                    }
                }
                pmm_deinit_region(base_addr, region_size);
            }
        next_region:
            mmap = (multiboot_memory_map_t*)((uint32_t)mmap + mmap->size + sizeof(mmap->size));
        }
    } else {
        pmm_deinit_region(0x800000, total_memory - 0x800000);
    }

    pmm_init_region(0, 0x100000);
    uint32_t kernel_size = pmm_bitmap_start - get_kernel_start();
    uint32_t bitmap_size = pmm_max_blocks / 8;
    pmm_init_region(get_kernel_start(), kernel_size + bitmap_size);

    // 1. ВКЛЮЧАЕМ АППАРАТНЫЙ ПЕЙДЖИНГ СНАЧАЛА
    paging_install();

    // 2. ИНИЦИАЛИЗИРУЕМ КУЧУ ЯДРА ВНУТРИ ЗАМАПЛЕННОЙ ЗОНЫ
    kheap_install(0xC00000, 0x1000000); 
    tasking_install();
    //task_create(dummy_task_A);
    //task_create(dummy_task_B);
    // 3. ИНИЦИАЛИЗИРУЕМ ТЕРМИНАЛ VGA (он безопасно заберет выровненную память)
    video_init(mboot_ptr);
    terminal_initialize();
    serial_init();
    pci_init();
    net_init();
    ps2_mouse_init();
    fat16_init();
    vfs_init();
    // Вывод логов
    terminal_writestring("Hardware Paging enabled successfully.\n");
    terminal_writestring("NaroSH Build ");
    char buf[16];
    inttostr(KERNEL_BUILD_NUMBER, buf);
    terminal_writestring(buf);

    timer_install(100); 
    char mem_print_buf[32];
    inttostr(total_memory / (1024 ^ 2), mem_print_buf);
    printf("\nDetected Real Physical RAM: "); printf(mem_print_buf); printf(" MB.\n");
    terminal_writestring("Type help for help.\n");
    
    

    terminal_writestring("Jumping to User Mode (Ring 3)... Goodbye Ring 0!\n");
    
    uint32_t user_code_size =
        (uint32_t)user_mode_demo_end - (uint32_t)user_mode_demo_start;

    task_t* first_user_task =
        task_create_user(user_mode_demo_start, user_code_size, 1);
    if (!first_user_task ||
        !task_create_user(user_mode_demo_start, user_code_size, 2) ||
        !task_create_kernel(idle_task)) {
        printf("Unable to initialize user processes.\n");
        while (1) {
            asm volatile("cli; hlt");
        }
    }

    ring3_test_state = first_user_task->user_test_state;
    task_start();
}
