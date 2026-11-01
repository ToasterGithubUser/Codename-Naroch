#include <kernel/idt.h>
#include <kernel/tty.h>
#include <string.h>
#include <kernel/keyboard.h> 
#include <kernel/conversions.h>
#include <kernel/io.h>
#include <stdio.h>
#include <kernel/timer.h>
#include <kernel/paging.h>
#include "vga.h"
// Добавьте в начало вашего idt.c
#include <kernel/task.h>

idt_entry_t idt_entries[256]; 
idt_ptr_t   idt_ptr;

// Внешние ассемблерные функции
extern void idt_load(void);
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void);

extern void isr32(void);
extern void isr33(void); // Наш обработчик клавиатуры IRQ 1


static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low  = (base & 0xFFFF);
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
    idt_entries[num].sel       = sel;
    idt_entries[num].always0   = 0;
    idt_entries[num].flags     = flags; // 0x8E - Ring 0, 32-bit Interrupt Gate
}

// Перепрограммирование PIC, чтобы сдвинуть IRQ с 0..7 на 32..39
static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20); // Мастер PIC начинает с 32 (0x20)
    outb(0xA1, 0x28); // Слейв PIC начинает с 40 (0x28)
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x0);  // Разрешить все прерывания
    outb(0xA1, 0x0);
}

void idt_install(void) {
    idt_ptr.limit = (sizeof(idt_entry_t) * 256) - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    memset(&idt_entries, 0, sizeof(idt_entry_t) * 256);

    pic_remap();
    //timer_install(100);
    // Регистрируем заглушки обработчиков
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // GPF
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); // Page Fault
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(32, (uint32_t)isr32, 0x08, 0x8E); // timer
    idt_set_gate(33, (uint32_t)isr33, 0x08, 0x8E); // Keyboard (IRQ 1 -> 32 + 1 = 33)

    idt_load();
}
static const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "Reserved"
};
// Главный распределитель прерываний на Си, вызываемый из interrupt.S
void irq_handler(regs_t *r) {

    if (r->int_no < 32) {
        // Окрашиваем экран в синий, текст в белый
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE)); // 0x1 - синий фон, 0xF - белый текст
        terminal_initialize();  // Очищаем экран синим цветом

        printf("==================================================================\n");
        printf("                  KERNEL PANIC: CPU EXCEPTION                     \n");
        printf("==================================================================\n\n");
        char num_buf[16]; // Буфер под номер прерывания
        inttostr(r->int_no, num_buf);

        // Если есть код ошибки процессора (например, у Page Fault)
        char err_buf[16];
        inttostr(r->err_code, err_buf);
        if (r->int_no < 20) {
            printf("Exception: ");
            printf(exception_messages[r->int_no]);
            printf(" (INT ");
            printf(num_buf);
            printf(")\n");
        }
       
        else {
            printf("Exception: Reserved Interrupt (INT ");
            printf(num_buf);
            printf(")\n");
        }
        if (r->int_no == 14) {
        page_fault_handler(r);
    } 
        printf("PROCESSOR REGISTERS DUMP:\n");
        printf("------------------------------------------------------------------\n");
        
        char buf[16];
        inttostr(r->eax, buf); printf("EAX: "); printf(buf); printf("  ");
        inttostr(r->ebx, buf); printf("EBX: "); printf(buf); printf("  ");
        inttostr(r->ecx, buf); printf("ECX: "); printf(buf); printf("\n");
        inttostr(r->edx, buf); printf("EDX: "); printf(buf); printf("  ");
        inttostr(r->esi, buf); printf("ESI: "); printf(buf); printf("  ");
        inttostr(r->edi, buf); printf("EDI: "); printf(buf); printf("\n");
        inttostr(r->esp, buf); printf("ESP: "); printf(buf); printf("  ");
        inttostr(r->ebp, buf); printf("EBP: "); printf(buf); printf("  ");
        inttostr(r->eip, buf); printf("EIP: "); printf(buf); printf("\n");
        inttostr(r->cs,  buf); printf("CS:  "); printf(buf); printf("  ");
        inttostr(r->ds,  buf); printf("DS:  "); printf(buf); printf("  ");
        inttostr(r->eflags, buf); printf("EFLAGS: "); printf(buf); printf("\n");
        
        printf("------------------------------------------------------------------\n");
        printf("System halted. Please restart your virtual machine.");

        // Намертво останавливаем процессор x86
        while (1) {
            asm volatile("cli; hlt");
        }
    }
    
    // Обработка таймера
    if (r->int_no == 32) {
        timer_handler(); 
        
        // ВАЖНО: Если многозадачность активна, сохраняем текущий ESP прерванного процессора в PCB
        if (current_task != NULL) {
            current_task->esp = (uint32_t)r; // Запоминаем точку сохранения регистров x86 на стеке
        }
    }

    // Обработка клавиатуры
    if (r->int_no == 33) {
        keyboard_handler();
    }

    // СИГНАЛ EOI ДЛЯ АППАРАТНЫХ ПРЕРЫВАНИЙ (ОБЯЗАТЕЛЕН СТРОГО В КОНЦЕ!)
    if (r->int_no >= 32) {
        if (r->int_no >= 40) {
            outb(0xA0, 0x20); // Направляем Slave PIC
        }
        outb(0x20, 0x20);     // Направляем Master PIC
    }
    
}
