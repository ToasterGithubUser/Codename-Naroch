#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/idt.h>
#include <kernel/conversions.h>
#include <kernel/tty.h>
#include <stdio.h>
#include <stddef.h>

static uint32_t* page_directory = 0;

void paging_install(void) {
     page_directory = (uint32_t*)pmm_alloc_block();
    
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; 
    }

    // Identity Mapping для первых 16 МБ памяти (4 таблицы страниц)
    for (uint32_t t = 0; t < 4; t++) {
        uint32_t* page_table = (uint32_t*)pmm_alloc_block();
        
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t physical_address = (t * 4 * 1024 * 1024) + (i * 4096);
            page_table[i] = physical_address | 3; 
        }
        page_directory[t] = ((uint32_t)page_table) | 3;
    }

    // НАДЁЖНОЕ ВКЛЮЧЕНИЕ ПЕЙДЖИНГА С ОЧИСТКОЙ КОНВЕЙЕРА x86
    asm volatile(
        "mov %0, %%cr3\n\t"         // Загружаем адрес каталога в CR3
        "mov %%cr0, %%eax\n\t"
        "or $0x80000000, %%eax\n\t" // Взводим 31-й бит (PG)
        "mov %%eax, %%cr0\n\t"
        "jmp 1f\n\t"                // Локальный прыжок для очистки очереди инструкций CPU
        "1:\n\t"
        :
        : "r"(page_directory)
        : "eax", "memory"
    );

}

// Обработчик вынесен в отдельную функцию. 
// Замените `registers_t`, если в вашем idt.h структура называется иначе.
void page_fault_handler(regs_t* r) {
    uint32_t faulting_address;
    asm volatile("mov %%cr2, %0" : "=r" (faulting_address));

    int present = (r->err_code & 0x1) ? 1 : 0; 
    int write   = (r->err_code & 0x2) ? 1 : 0; 
    int user    = (r->err_code & 0x4) ? 1 : 0; 
    char charbuf[32];
    inttostr(faulting_address,charbuf);
    printf("\n--- PAGE FAULT at 0x"); printf(charbuf); printf(" --- \n");
    printf("Cause: ");
    if (!present) printf("[Page Not Present] ");
    if (write)    printf("[Write Operation] ");
    if (user)     printf("[User-Mode Privilege Violation] ");
    //do not halt system, as it is idt's panic handler's duty
}
