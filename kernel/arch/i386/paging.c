#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/idt.h>
#include <kernel/conversions.h>
#include <kernel/tty.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define PAGE_SIZE 4096U
#define USER_VIRTUAL_BASE 0x40000000U
#define USER_DIRECTORY_INDEX (USER_VIRTUAL_BASE >> 22)
#define IDENTITY_MAP_SIZE 0x02000000U

static uint32_t* page_directory = 0;
static uint32_t* kernel_page_directory = 0;

void paging_install(void) {
     page_directory = (uint32_t*)pmm_alloc_block();
    kernel_page_directory = page_directory;
    
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002;
    }

    // Keep the kernel and heap identity-mapped, but supervisor-only.
    for (uint32_t t = 0; t < IDENTITY_MAP_SIZE / 0x400000U; t++) {
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

int paging_map_user_page(uint32_t virtual_address, uint32_t physical_address,
                         int writable) {
    return paging_map_user_page_in(page_directory, virtual_address,
                                   physical_address, writable);
}

uint32_t* paging_create_address_space(void) {
    if (!page_directory) {
        return 0;
    }

    uint32_t* address_space = (uint32_t*)pmm_alloc_block();
    if (!address_space) {
        return 0;
    }
    if ((uint32_t)address_space >= IDENTITY_MAP_SIZE) {
        pmm_free_block(address_space);
        return 0;
    }

    memcpy(address_space, page_directory, PAGE_SIZE);
    for (uint32_t i = USER_DIRECTORY_INDEX; i < 1024; i++) {
        address_space[i] = 2;
    }
    return address_space;
}

void paging_destroy_address_space(uint32_t* address_space) {
    if (!address_space || address_space == page_directory) {
        return;
    }

    for (uint32_t i = USER_DIRECTORY_INDEX; i < 1024; i++) {
        if (address_space[i] & 1) {
            uint32_t* page_table =
                (uint32_t*)(address_space[i] & 0xFFFFF000);
            for (uint32_t j = 0; j < 1024; j++) {
                if (page_table[j] & 1) {
                    pmm_free_block((void*)(page_table[j] & 0xFFFFF000));
                }
            }
            pmm_free_block(page_table);
        }
    }
    pmm_free_block(address_space);
}

uint32_t* paging_current_address_space(void) {
    return page_directory;
}

uint32_t paging_user_page_physical_in(uint32_t* address_space,
                                      uint32_t virtual_address) {
    if (!address_space || virtual_address < USER_VIRTUAL_BASE) {
        return 0;
    }

    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FF;
    uint32_t directory_entry = address_space[directory_index];
    if (!(directory_entry & 1)) {
        return 0;
    }

    uint32_t* page_table =
        (uint32_t*)(directory_entry & 0xFFFFF000);
    uint32_t page_entry = page_table[table_index];
    return (page_entry & 1) ? page_entry & 0xFFFFF000 : 0;
}

int paging_map_user_page_in(uint32_t* address_space,
                            uint32_t virtual_address,
                            uint32_t physical_address,
                            int writable) {
    if (!address_space ||
        (virtual_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & (PAGE_SIZE - 1)) != 0 ||
        virtual_address < USER_VIRTUAL_BASE ||
        physical_address >= IDENTITY_MAP_SIZE) {
        return 0;
    }

    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FF;
    uint32_t* page_table;

    if (address_space[directory_index] & 1) {
        page_table = (uint32_t*)(address_space[directory_index] & 0xFFFFF000);
        address_space[directory_index] |= 7;
    } else {
        page_table = (uint32_t*)pmm_alloc_block();
        if (!page_table) {
            return 0;
        }
        if ((uint32_t)page_table >= IDENTITY_MAP_SIZE) {
            pmm_free_block(page_table);
            return 0;
        }

        memset(page_table, 0, PAGE_SIZE);
        address_space[directory_index] = (uint32_t)page_table | 7;
    }

    if (page_table[table_index] & 1) {
        if (writable) {
            page_table[table_index] |= 2;
        }
        return 1;
    }

    page_table[table_index] = physical_address | 5 | (writable ? 2 : 0);
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return 1;
}

void paging_activate_address_space(uint32_t* address_space) {
    if (!address_space) {
        return;
    }

    page_directory = address_space;
    asm volatile("mov %0, %%cr3" : : "r"(address_space) : "memory");
}

int paging_user_range_valid(uint32_t address, uint32_t length) {
    if (!page_directory || !length || address + length < address) {
        return 0;
    }

    uint32_t end = address + length;
    for (uint32_t page = address & ~(PAGE_SIZE - 1); page < end;
         page += PAGE_SIZE) {
        uint32_t directory_index = page >> 22;
        uint32_t table_index = (page >> 12) & 0x3FF;
        uint32_t directory_entry = page_directory[directory_index];
        if ((directory_entry & 5) != 5) {
            return 0;
        }

        uint32_t* page_table =
            (uint32_t*)(directory_entry & 0xFFFFF000);
        if ((page_table[table_index] & 5) != 5) {
            return 0;
        }

        if (page > 0xFFFFEFFF) {
            break;
        }
    }

    return 1;
}

int paging_user_range_writable(uint32_t address, uint32_t length) {
    if (!page_directory || !length || address + length < address) {
        return 0;
    }

    uint32_t end = address + length;
    for (uint32_t page = address & ~(PAGE_SIZE - 1); page < end;
         page += PAGE_SIZE) {
        uint32_t directory_index = page >> 22;
        uint32_t table_index = (page >> 12) & 0x3FF;
        uint32_t directory_entry = page_directory[directory_index];
        if ((directory_entry & 7) != 7) {
            return 0;
        }

        uint32_t* page_table =
            (uint32_t*)(directory_entry & 0xFFFFF000);
        if ((page_table[table_index] & 7) != 7) {
            return 0;
        }

        if (page > 0xFFFFEFFF) {
            break;
        }
    }

    return 1;
}

int paging_unmap_user_page_in(uint32_t* address_space,
                              uint32_t virtual_address) {
    if (!address_space || (virtual_address & (PAGE_SIZE - 1)) ||
        virtual_address < USER_VIRTUAL_BASE) {
        return 0;
    }

    uint32_t directory_index = virtual_address >> 22;
    uint32_t table_index = (virtual_address >> 12) & 0x3FF;
    uint32_t directory_entry = address_space[directory_index];
    if (!(directory_entry & 1)) {
        return 0;
    }

    uint32_t* page_table =
        (uint32_t*)(directory_entry & 0xFFFFF000);
    uint32_t page_entry = page_table[table_index];
    if (!(page_entry & 1)) {
        return 0;
    }

    page_table[table_index] = 0;
    pmm_free_block((void*)(page_entry & 0xFFFFF000));
    if (address_space == page_directory) {
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    }
    return 1;
}

int paging_copy_to_user_in(uint32_t* address_space, uint32_t destination,
                           const void* source, uint32_t length) {
    if (!address_space || (!source && length) ||
        destination + length < destination) {
        return 0;
    }

    const uint8_t* input = (const uint8_t*)source;
    while (length) {
        uint32_t page = destination & ~(PAGE_SIZE - 1);
        uint32_t directory_index = page >> 22;
        uint32_t table_index = (page >> 12) & 0x3FF;
        uint32_t directory_entry = address_space[directory_index];
        if ((directory_entry & 7) != 7) {
            return 0;
        }
        uint32_t* page_table =
            (uint32_t*)(directory_entry & 0xFFFFF000);
        uint32_t page_entry = page_table[table_index];
        if ((page_entry & 7) != 7) {
            return 0;
        }

        uint32_t page_offset = destination & (PAGE_SIZE - 1);
        uint32_t chunk = PAGE_SIZE - page_offset;
        if (chunk > length) {
            chunk = length;
        }
        uint32_t physical = page_entry & 0xFFFFF000;
        if (physical >= IDENTITY_MAP_SIZE) {
            return 0;
        }
        memcpy((void*)(physical + page_offset), input, chunk);
        destination += chunk;
        input += chunk;
        length -= chunk;
    }
    return 1;
}

int paging_map_kernel_mmio(uint32_t virtual_address,
                           uint32_t physical_address, uint32_t length) {
    if (!kernel_page_directory || !length ||
        (virtual_address & (PAGE_SIZE - 1)) !=
            (physical_address & (PAGE_SIZE - 1)) ||
        virtual_address >= USER_VIRTUAL_BASE ||
        virtual_address + length < virtual_address ||
        physical_address + length < physical_address ||
        virtual_address + length > USER_VIRTUAL_BASE) {
        return 0;
    }

    uint32_t virtual_page = virtual_address & ~(PAGE_SIZE - 1);
    uint32_t physical_page = physical_address & ~(PAGE_SIZE - 1);
    uint32_t end = virtual_address + length;
    uint32_t* directory = kernel_page_directory;
    for (; virtual_page < end; virtual_page += PAGE_SIZE,
         physical_page += PAGE_SIZE) {
        uint32_t directory_index = virtual_page >> 22;
        uint32_t table_index = (virtual_page >> 12) & 0x3FF;
        uint32_t* page_table;
        if (directory[directory_index] & 1) {
            page_table =
                (uint32_t*)(directory[directory_index] & 0xFFFFF000);
        } else {
            page_table = (uint32_t*)pmm_alloc_block();
            if (!page_table ||
                (uint32_t)page_table >= IDENTITY_MAP_SIZE) {
                if (page_table) {
                    pmm_free_block(page_table);
                }
                return 0;
            }
            memset(page_table, 0, PAGE_SIZE);
            directory[directory_index] = (uint32_t)page_table | 3;
        }

        if (page_table[table_index] & 1) {
            return 0;
        }
        page_table[table_index] = physical_page | 0x1B;
        asm volatile("invlpg (%0)" : : "r"(virtual_page) : "memory");
    }

    if (page_directory != kernel_page_directory) {
        uint32_t directory_index =
            (virtual_address & ~(PAGE_SIZE - 1)) >> 22;
        page_directory[directory_index] =
            kernel_page_directory[directory_index];
        asm volatile("mov %0, %%cr3" : : "r"(page_directory) : "memory");
    }
    return 1;
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
