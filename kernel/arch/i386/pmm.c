#include <kernel/pmm.h>
#include <string.h>

// Глобальные переменные без static для синхронизации с Shell
uint32_t* pmm_bitmap = 0;
uint32_t  pmm_max_blocks = 0;
uint32_t  pmm_used_blocks = 0;
static uint32_t pmm_first_allocatable_block;

static inline void bitmap_set(uint32_t bit) {
    pmm_bitmap[bit / 32] |= (1U << (bit % 32)); // 1U гарантирует беззнаковый сдвиг
}

static inline void bitmap_unset(uint32_t bit) {
    pmm_bitmap[bit / 32] &= ~(1U << (bit % 32));
}

static inline int bitmap_test(uint32_t bit) {
    // Безопасное приведение к логическому 0 или 1
    return (pmm_bitmap[bit / 32] & (1U << (bit % 32))) ? 1 : 0;
}

static int bitmap_first_free(void) {
    uint32_t num_words = pmm_max_blocks / 32;
    uint32_t first_word = pmm_first_allocatable_block / 32;
    uint32_t first_bit = pmm_first_allocatable_block % 32;
    for (uint32_t i = first_word; i < num_words; i++) {
        // Если ячейка не забита единицами, внутри точно есть хоть один ноль (свободная страница)
        if (pmm_bitmap[i] != 0xFFFFFFFF) { 
            uint32_t start_bit = i == first_word ? first_bit : 0;
            for (uint32_t j = start_bit; j < 32; j++) {
                if (!(pmm_bitmap[i] & (1U << j))) {
                    return i * 32 + j;
                }
            }
        }
    }
    return -1;
}

void pmm_init(uint32_t mem_size, uint32_t bitmap_address) {
    pmm_max_blocks = mem_size / PMM_BLOCK_SIZE;
    pmm_bitmap = (uint32_t*)bitmap_address;
    pmm_used_blocks = pmm_max_blocks; // Изначально считаем всю память занятой
    uint64_t bitmap_end = (uint64_t)bitmap_address +
        (pmm_max_blocks + 7U) / 8U;
    pmm_first_allocatable_block =
        (uint32_t)((bitmap_end + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE);
    if (pmm_first_allocatable_block < 0x100000U / PMM_BLOCK_SIZE) {
        pmm_first_allocatable_block = 0x100000U / PMM_BLOCK_SIZE;
    }

    // Заполняем битмап единицами напрямую через слова
    uint32_t num_words = pmm_max_blocks / 32;
    for (uint32_t i = 0; i < num_words; i++) {
        pmm_bitmap[i] = 0xFFFFFFFF; 
    }
}

void pmm_init_region(uint32_t base, size_t size) {
    uint32_t align = base / PMM_BLOCK_SIZE;
    uint32_t blocks = size / PMM_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t current_block = align + i;
        if (current_block >= pmm_max_blocks) break;
        if (current_block < pmm_first_allocatable_block) continue;
        
        // Если блок еще не был занят, занимаем его
        if (!bitmap_test(current_block)) {
            bitmap_set(current_block);
            pmm_used_blocks++;
        }
    }
}

void pmm_deinit_region(uint32_t base, size_t size) {
    uint32_t align = base / PMM_BLOCK_SIZE;
    uint32_t blocks = size / PMM_BLOCK_SIZE;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t current_block = align + i;
        if (current_block >= pmm_max_blocks) break;
        
        // Прямое принудительное освобождение региона памяти!
        // Если бит установлен (страница занята), сбрасываем его и уменьшаем счетчик
        if (bitmap_test(current_block)) {
            bitmap_unset(current_block);
            pmm_used_blocks--; 
        }
    }
}

void* pmm_alloc_block(void) {
    int frame = bitmap_first_free();
    if (frame == -1) {
        return 0; // Свободных страниц нет
    }

    bitmap_set(frame);
    pmm_used_blocks++;

    uint32_t addr = frame * PMM_BLOCK_SIZE;
    return (void*)addr;
}

void pmm_free_block(void* p) {
    uint32_t addr = (uint32_t)p;
    if (addr / PMM_BLOCK_SIZE < pmm_first_allocatable_block) {
        return;
    }
    uint32_t frame = addr / PMM_BLOCK_SIZE;

    if (frame < pmm_max_blocks) {
        if (bitmap_test(frame)) {
            bitmap_unset(frame);
            pmm_used_blocks--;
        }
    }
}
