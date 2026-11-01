#ifndef _KERNEL_HEAP_H
#define _KERNEL_HEAP_H

#include <stdint.h>
#include <stddef.h>

// Заголовок каждого блока памяти в куче
struct heap_header {
    size_t size;                 // Размер блока данных (БЕЗ учета размера самого заголовка)
    uint8_t is_free;             // 1 — блок свободен, 0 — занят
    struct heap_header* next;    // Указатель на следующий блок в куче
} __attribute__((packed));
typedef struct heap_header heap_header_t;

// Инициализация кучи ядра
void kheap_install(uint32_t start_address, uint32_t end_address);

// Выделение памяти произвольного размера
void* kmalloc(size_t size);

// Освобождение выделенной памяти
void kfree(void* ptr);

#endif
