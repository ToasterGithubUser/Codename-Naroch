#include <kernel/heap.h>
#include <stdio.h>

static heap_header_t* heap_start = 0;

void kheap_install(uint32_t start_address, uint32_t end_address) {
    // Выравниваем начальный адрес по границе 4 байт для x86
    start_address = (start_address + 3) & ~3;
    
    heap_start = (heap_header_t*)start_address;
    heap_start->size = (end_address - start_address) - sizeof(heap_header_t);
    heap_start->is_free = 1;
    heap_start->next = 0;
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    
    // Выравниваем запрашиваемый размер по границе 4 байт
    size = (size + 3) & ~3;

    heap_header_t* current = heap_start;

    // Ищем первый подходящий свободный блок (First Fit)
    while (current) {
        if (current->is_free && current->size >= size) {
            // Проверяем, можно ли разделить блок на два (текущий занятый + новый свободный остаток)
            // Минимальный остаток должен вмещать заголовок + хотя бы 4 байта данных
            if (current->size >= size + sizeof(heap_header_t) + 4) {
                heap_header_t* new_next = (heap_header_t*)((uint32_t)current + sizeof(heap_header_t) + size);
                new_next->size = current->size - size - sizeof(heap_header_t);
                new_next->is_free = 1;
                new_next->next = current->next;

                current->size = size;
                current->next = new_next;
            }

            current->is_free = 0; // Маркируем блок как занятый
            
            // Возвращаем указатель на память СРАЗУ за заголовочным блоком
            return (void*)((uint32_t)current + sizeof(heap_header_t));
        }
        current = current->next;
    }

    printf("Kernel Panic: Out of Kernel Heap Memory!\n");
    return 0; // Куча переполнена
}

void kfree(void* ptr) {
    if (!ptr) return;

    // Сдвигаем указатель назад, чтобы получить доступ к служебному заголовку блока
    heap_header_t* header = (heap_header_t*)((uint32_t)ptr - sizeof(heap_header_t));
    header->is_free = 1;

    // Дефрагментация (Слияние смежных свободных блоков), чтобы избежать фрагментации памяти кучи
    heap_header_t* current = heap_start;
    while (current && current->next) {
        if (current->is_free && current->next->is_free) {
            current->size += sizeof(heap_header_t) + current->next->size;
            current->next = current->next->next;
            continue; // Проверяем склейку со следующим блоком повторно
        }
        current = current->next;
    }
}
