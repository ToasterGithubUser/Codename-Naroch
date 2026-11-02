#ifndef _KERNEL_IDT_H
#define _KERNEL_IDT_H

#include <stdint.h>

// Структура записи IDT
struct idt_entry_struct {
    uint16_t base_low;   // Младшие 16 бит адреса обработчика
    uint16_t sel;        // Селектор сегмента кода в GDT (обычно 0x08)
    uint8_t  always0;    // Всегда 0
    uint8_t  flags;      // Флаги доступа (тип шлюза, кольцо защиты)
    uint16_t base_high;  // Старшие 16 бит адреса обработчика
} __attribute__((packed));
typedef struct idt_entry_struct idt_entry_t;

// Указатель для регистра IDTR
struct idt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
typedef struct idt_ptr_struct idt_ptr_t;

// Структура состояния процессора, передаваемая из ассемблера в C
struct registers {
    uint32_t ds;                                      // Селектор данных
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  // Pushed by pusha
    uint32_t int_no, err_code;                        // Номер прерывания и код ошибки
    uint32_t eip, cs, eflags, useresp, ss;            // ESP/SS are present on ring transitions
};
typedef struct registers regs_t;

void idt_install(void);

#endif
    