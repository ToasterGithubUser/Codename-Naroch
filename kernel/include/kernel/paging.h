#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

// Инициализация структур страничной адресации и включение Paging в CPU
void paging_install(void);
int paging_map_user_page(uint32_t virtual_address, uint32_t physical_address,
                         int writable);
uint32_t* paging_create_address_space(void);
void paging_destroy_address_space(uint32_t* address_space);
uint32_t* paging_current_address_space(void);
uint32_t paging_user_page_physical_in(uint32_t* address_space,
                                      uint32_t virtual_address);
int paging_map_user_page_in(uint32_t* address_space,
                            uint32_t virtual_address,
                            uint32_t physical_address,
                            int writable);
void paging_activate_address_space(uint32_t* address_space);
int paging_user_range_valid(uint32_t address, uint32_t length);
int paging_user_range_writable(uint32_t address, uint32_t length);
int paging_unmap_user_page_in(uint32_t* address_space,
                              uint32_t virtual_address);
int paging_copy_to_user_in(uint32_t* address_space, uint32_t destination,
                           const void* source, uint32_t length);
int paging_map_kernel_mmio(uint32_t virtual_address,
                           uint32_t physical_address, uint32_t length);

// Обработчик исключения Page Fault (INT 14)
struct registers; // Опережающее объявление regs_t структуры из idt.h
void page_fault_handler(struct registers *r);

#endif
