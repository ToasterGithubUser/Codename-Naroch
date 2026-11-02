#include <kernel/gdt.h>
#include <string.h>
// Our table contains 3 entries: Null, Kernel Code, Kernel Data
// Теперь у нас 4 записи в GDT
gdt_entry_t gdt_entries[6]; 
gdt_ptr_t   gdt_ptr;
tss_entry_t tss_entry; 

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

void gdt_install(void) {
    // Размер GDT теперь под 4 элемента
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // 1. Null segment
    gdt_set_gate(0, 0, 0, 0, 0);

    // 2. Kernel Code Segment (0x08)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // 3. Kernel Data Segment (0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // 4. User Code Segment (Смещение: 3 * 8 = 24 = 0x18 | 3 для RPL = 0x1B)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // 5. User Data Segment (Смещение: 4 * 8 = 32 = 0x20 | 3 для RPL = 0x23)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    // --- НАСТРОЙКА СТРУКТУРЫ TSS ---
    memset(&tss_entry, 0, sizeof(tss_entry_t));
    tss_entry.ss0  = 0x10;  
    tss_entry.iomap_base = sizeof(tss_entry_t);
    uint32_t esp;
    asm volatile("mov %%esp, %0" : "=r"(esp));
    tss_entry.esp0 = esp;   

    uint32_t tss_base = (uint32_t)&tss_entry;
    uint32_t tss_limit = sizeof(tss_entry_t) - 1;
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00);

    // Загружаем GDT и сообщаем процессору о TSS (теперь индекс ворот 0x28!)
    asm volatile(
        "lgdt %0\n\t"            
        "ljmp $0x08, $1f\n\t"   
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"    // 0x28 указывает на нашу 5-ю запись в GDT (TSS)
        "ltr %%ax\n\t"           
        : : "m"(gdt_ptr) : "ax"
    );
}
