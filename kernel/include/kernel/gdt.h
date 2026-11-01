#ifndef _KERNEL_GDT_H
#define _KERNEL_GDT_H

#include <stdint.h>

// Defines a GDT entry. Packed attribute prevents the compiler from optimizing padding.
struct gdt_entry_struct {
    uint16_t limit_low;           // The lower 16 bits of the limit
    uint16_t base_low;            // The lower 16 bits of the base
    uint8_t  base_middle;         // The next 8 bits of the base
    uint8_t  access;              // Access flags (determines Ring 0 vs Ring 3, Code vs Data)
    uint8_t  granularity;
    uint8_t  base_high;           // The last 8 bits of the base
} __attribute__((packed));
typedef struct gdt_entry_struct gdt_entry_t;

// Special pointer structure passed directly to the GDTR register via assembly
struct gdt_ptr_struct {
    uint16_t limit;               // The upper 16 bits of all selector limits
    uint32_t base;                // The address of the first gdt_entry_t struct
} __attribute__((packed));
typedef struct gdt_ptr_struct gdt_ptr_t;

// Function to initialize the GDT
void gdt_install(void);
// Структура TSS (Task State Segment) для x86
struct tss_entry_struct {
    uint32_t prev_tss;   // Старый TSS, если используется аппаратное переключение задач
    uint32_t esp0;       // Указатель на стек ядра (Ring 0) — САМОЕ ВАЖНОЕ ПОЛЕ!
    uint32_t ss0;        // Сегмент стека ядра (обычно 0x10)
    uint32_t esp1;       // Не используются в плоской модели памяти
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;        // Регистр CR3 (каталог страниц)
    uint32_t eip;        // Состояние регистров для аппаратного переключения
    uint32_t eflags;
    uint32_t eax; uint32_t ecx; uint32_t edx; uint32_t ebx;
    uint32_t esp; uint32_t ebp; uint32_t esi; uint32_t edi;
    uint32_t es;  uint32_t cs;  uint32_t ss;  uint32_t ds; uint32_t fs; uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base; // База карты ввода-вывода (IO Map)
} __attribute__((packed));
typedef struct tss_entry_struct tss_entry_t;

#endif
