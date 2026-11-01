#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_BLOCK_SIZE 4096 // Размер страницы в x86 (4 КБ)

// Инициализация PMM: задает адрес битовой карты и размер доступной RAM
void pmm_init(uint32_t mem_size, uint32_t bitmap_address);

// Маркирует регион памяти как занятый (например, там, где лежит код самого ядра)
void pmm_init_region(uint32_t base, size_t size);

// Маркирует регион памяти как свободный
void pmm_deinit_region(uint32_t base, size_t size);

// Выделяет одну свободную физическую страницу 4 КБ и возвращает ее адрес
void* pmm_alloc_block(void);

// Освобождает физическую страницу по ее адресу
void pmm_free_block(void* p);

#endif
