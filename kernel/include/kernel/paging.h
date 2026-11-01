#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

// Инициализация структур страничной адресации и включение Paging в CPU
void paging_install(void);

// Обработчик исключения Page Fault (INT 14)
struct registers; // Опережающее объявление regs_t структуры из idt.h
void page_fault_handler(struct registers *r);

#endif
