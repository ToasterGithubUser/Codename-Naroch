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
// Объявляем возможные альтернативные имена, которые генерируют разные версии linker.ld
extern char _kernel_start    __attribute__((weak));
extern char _kernel_end      __attribute__((weak));
extern char kernel_start     __attribute__((weak));
extern char kernel_end       __attribute__((weak));
extern char start            __attribute__((weak));
extern char end              __attribute__((weak));
extern void switch_to_user(void (*entry_point)(void));

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
void my_user_program(void) {
    // ВНИМАНИЕ: Здесь пока нельзя вызывать printf или работать с диском напрямую,
    // так как процессор в Ring 3 моментально выкинет General Protection Fault!
    // Мы просто проверим выполнение в бесконечном цикле.
    while (1) {
        asm volatile("nop"); 
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
    terminal_initialize();
    fat16_init();
    // Вывод логов
    terminal_writestring("Hardware Paging enabled successfully.\n");
    terminal_writestring("NaroSH Build ");
    char buf[16];
    inttostr(KERNEL_BUILD_NUMBER, buf);
    terminal_writestring(buf);

    timer_install(100); 
    char mem_print_buf[32];
    inttostr(total_memory / (1024 * 1024), mem_print_buf);
    printf("\nDetected Real Physical RAM: "); printf(mem_print_buf); printf(" MB.\n");
    terminal_writestring("Type help for help.\n");
    
    shell_prompt();

    asm volatile("sti");
    terminal_writestring("Jumping to User Mode (Ring 3)... Goodbye Ring 0!\n");
    
    switch_to_user(my_user_program); // Процессор улетает в Ring 3!
    
    while(1) {
        asm volatile("hlt");
    }
        
}
