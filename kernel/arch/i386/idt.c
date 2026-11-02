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
#include <kernel/ring3.h>
#include <kernel/syscall.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/vfs.h>

idt_entry_t idt_entries[256]; 
idt_ptr_t   idt_ptr;
extern char user_probe_fault[];
extern char user_probe_resume[];
extern char user_mode_demo_start[];

#define USER_FILE_COUNT 16
#define USER_FILE_NAME_MAX 256
#define USER_FILE_MAX_SIZE (1024U * 1024U)
#define USER_HEAP_PAGE_SIZE 4096U

typedef struct {
    int owner_pid;
    int flags;
    uint32_t position;
    uint32_t size;
    uint32_t capacity;
    uint8_t* contents;
    int dirty;
    char path[USER_FILE_NAME_MAX];
} user_file_t;

static user_file_t user_files[USER_FILE_COUNT];
static int foreground_pid;

void syscall_set_foreground_pid(int pid) {
    foreground_pid = pid;
}

int syscall_console_input(char character) {
    if (!foreground_pid) {
        return 0;
    }

    task_stdin_feed(foreground_pid, character);
    if (character == '\n') {
        terminal_putchar('\n');
    } else if (character == '\b') {
        terminal_putchar('\b');
    } else {
        terminal_putchar(character);
    }
    return 1;
}

static int copy_user_string(uint32_t address, char* destination,
                            uint32_t capacity) {
    if (!address || !destination || !capacity) {
        return 0;
    }
    for (uint32_t i = 0; i < capacity; i++) {
        if (address > UINT32_MAX - i ||
            !paging_user_range_valid(address + i, 1)) {
            return 0;
        }
        destination[i] = *(const char*)(address + i);
        if (!destination[i]) {
            return i != 0;
        }
    }
    return 0;
}

static user_file_t* user_file_for(int descriptor) {
    if (descriptor < 3 || descriptor >= 3 + USER_FILE_COUNT ||
        !current_task) {
        return 0;
    }
    user_file_t* file = &user_files[descriptor - 3];
    return file->owner_pid == current_task->pid ? file : 0;
}

static int close_user_file(user_file_t* file) {
    int success = 1;
    if (file->dirty &&
        !vfs_write_file(file->path, file->contents, file->size)) {
        success = 0;
    }
    vfs_track_close(file->path);
    if (file->contents) {
        kfree(file->contents);
    }
    memset(file, 0, sizeof(*file));
    return success;
}

static void close_task_files(int pid) {
    for (uint32_t i = 0; i < USER_FILE_COUNT; i++) {
        if (user_files[i].owner_pid == pid) {
            (void)close_user_file(&user_files[i]);
        }
    }
}

static int open_user_file(uint32_t user_path, uint32_t flags) {
    char path[USER_FILE_NAME_MAX];
    if (!copy_user_string(user_path, path, sizeof(path)) ||
        (flags & ~(SYSCALL_O_ACCMODE | SYSCALL_O_CREAT |
                   SYSCALL_O_TRUNC | SYSCALL_O_APPEND)) ||
        (flags & SYSCALL_O_ACCMODE) > SYSCALL_O_RDWR ||
        ((flags & SYSCALL_O_TRUNC) &&
         (flags & SYSCALL_O_ACCMODE) == SYSCALL_O_RDONLY)) {
        return -1;
    }
    char resolved_path[USER_FILE_NAME_MAX];
    if (!vfs_resolve_path(path, resolved_path, sizeof(resolved_path))) {
        return -1;
    }
    memcpy(path, resolved_path, strlen(resolved_path) + 1);

    uint32_t slot = USER_FILE_COUNT;
    for (uint32_t i = 0; i < USER_FILE_COUNT; i++) {
        if (!user_files[i].owner_pid) {
            slot = i;
            break;
        }
    }
    if (slot == USER_FILE_COUNT) {
        return -1;
    }

    void* contents = 0;
    size_t size = 0;
    int exists = vfs_read_file(path, &contents, &size);
    if (!exists) {
        if (!(flags & SYSCALL_O_CREAT) ||
            !vfs_write_file(path, 0, 0)) {
            return -1;
        }
        exists = vfs_read_file(path, &contents, &size);
        if (!exists) {
            return -1;
        }
    }
    if ((flags & SYSCALL_O_TRUNC) &&
        !vfs_write_file(path, 0, 0)) {
        kfree(contents);
        return -1;
    }
    if (flags & SYSCALL_O_TRUNC) {
        kfree(contents);
        contents = (uint8_t*)kmalloc(1);
        size = 0;
        if (!contents) {
            return -1;
        }
    }
    if (size > USER_FILE_MAX_SIZE) {
        kfree(contents);
        return -1;
    }
    if (!size && !contents) {
        contents = (uint8_t*)kmalloc(1);
        if (!contents) {
            return -1;
        }
    }

    user_file_t* file = &user_files[slot];
    if (!vfs_track_open(path)) {
        kfree(contents);
        return -1;
    }
    file->owner_pid = current_task->pid;
    file->flags = flags;
    file->position = (flags & SYSCALL_O_APPEND) ? (uint32_t)size : 0;
    file->size = (uint32_t)size;
    file->capacity = size ? (uint32_t)size : 1;
    file->contents = (uint8_t*)contents;
    memcpy(file->path, path, strlen(path) + 1);
    return (int)slot + 3;
}

static int write_user_file(user_file_t* file, const uint8_t* source,
                           uint32_t count) {
    if ((file->flags & SYSCALL_O_ACCMODE) == SYSCALL_O_RDONLY ||
        file->position > USER_FILE_MAX_SIZE ||
        count > USER_FILE_MAX_SIZE - file->position) {
        return -1;
    }
    if (file->flags & SYSCALL_O_APPEND) {
        file->position = file->size;
    }

    uint32_t end = file->position + count;
    if (end > file->capacity) {
        uint32_t capacity = file->capacity ? file->capacity : 1;
        while (capacity < end) {
            uint32_t next = capacity > USER_FILE_MAX_SIZE / 2
                ? USER_FILE_MAX_SIZE
                : capacity * 2;
            if (next == capacity) {
                return -1;
            }
            capacity = next;
        }
        uint8_t* replacement = (uint8_t*)kmalloc(capacity);
        if (!replacement) {
            return -1;
        }
        memcpy(replacement, file->contents, file->size);
        kfree(file->contents);
        file->contents = replacement;
        file->capacity = capacity;
    }

    memcpy(file->contents + file->position, source, count);
    file->position = end;
    if (end > file->size) {
        file->size = end;
    }
    file->dirty = 1;
    return (int)count;
}

static int seek_user_file(user_file_t* file, int32_t offset, int whence) {
    if (!file) {
        return -1;
    }

    int64_t base;
    if (whence == SYSCALL_SEEK_SET) {
        base = 0;
    } else if (whence == SYSCALL_SEEK_CUR) {
        base = file->position;
    } else if (whence == SYSCALL_SEEK_END) {
        base = file->size;
    } else {
        return -1;
    }
    int64_t position = base + offset;
    if (position < 0 || position > USER_FILE_MAX_SIZE) {
        return -1;
    }
    file->position = (uint32_t)position;
    return (int)file->position;
}

static int spawn_user_process(uint32_t user_path, uint32_t user_argv,
                              uint32_t argc) {
    if (!current_task || !argc || argc > SYSCALL_SPAWN_MAX_ARGS ||
        !user_argv) {
        return -1;
    }

    char path[USER_FILE_NAME_MAX];
    char arguments[SYSCALL_SPAWN_MAX_ARGS][SYSCALL_SPAWN_MAX_ARG_LENGTH];
    const char* argument_pointers[SYSCALL_SPAWN_MAX_ARGS];
    if (!copy_user_string(user_path, path, sizeof(path))) {
        return -1;
    }
    while (path[0] == '.' && path[1] == '/') {
        memmove(path, path + 2, strlen(path + 2) + 1);
    }

    for (uint32_t i = 0; i < argc; i++) {
        uint64_t pointer_address = (uint64_t)user_argv +
            i * sizeof(uint32_t);
        if (pointer_address > UINT32_MAX ||
            !paging_user_range_valid((uint32_t)pointer_address,
                                     sizeof(uint32_t))) {
            return -1;
        }
        uint32_t user_argument =
            *(const uint32_t*)(uint32_t)pointer_address;
        if (!copy_user_string(user_argument, arguments[i],
                              sizeof(arguments[i]))) {
            return -1;
        }
        argument_pointers[i] = arguments[i];
    }

    void* image;
    size_t image_size;
    if (!vfs_read_file(path, &image, &image_size)) {
        return -1;
    }
    int process_id = -1;
    int created = task_create_elf(image, image_size, (int)argc,
                                  argument_pointers, current_task->pid,
                                  &process_id);
    kfree(image);
    return created ? process_id : -1;
}

static int set_user_heap_break(uint32_t requested) {
    if (!current_task || !current_task->heap_start) {
        return -1;
    }
    if (!requested) {
        return (int)current_task->heap_break;
    }
    if (requested < current_task->heap_start || requested > 0x70000000U) {
        return -1;
    }

    uint32_t old_break = current_task->heap_break;
    uint32_t old_page_end =
        (old_break + USER_HEAP_PAGE_SIZE - 1) & ~(USER_HEAP_PAGE_SIZE - 1);
    uint32_t new_page_end =
        (requested + USER_HEAP_PAGE_SIZE - 1) & ~(USER_HEAP_PAGE_SIZE - 1);
    if (new_page_end > old_page_end) {
        uint32_t page = old_page_end;
        for (; page < new_page_end; page += USER_HEAP_PAGE_SIZE) {
            uint32_t physical = (uint32_t)pmm_alloc_block();
            if (!physical || physical >= 0x02000000U) {
                if (physical) {
                    pmm_free_block((void*)physical);
                }
                break;
            }
            memset((void*)physical, 0, USER_HEAP_PAGE_SIZE);
            if (!paging_map_user_page_in(current_task->page_directory, page,
                                         physical, 1)) {
                pmm_free_block((void*)physical);
                break;
            }
        }
        if (page < new_page_end) {
            while (page > old_page_end) {
                page -= USER_HEAP_PAGE_SIZE;
                (void)paging_unmap_user_page_in(current_task->page_directory,
                                                page);
            }
            return -1;
        }
    } else {
        for (uint32_t page = new_page_end; page < old_page_end;
             page += USER_HEAP_PAGE_SIZE) {
            (void)paging_unmap_user_page_in(current_task->page_directory,
                                            page);
        }
    }

    current_task->heap_break = requested;
    return (int)requested;
}
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
extern void isr128(void);
extern void isr129(void);


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
    outb(0x21, 0xFC); // Enable only timer and keyboard IRQs.
    outb(0xA1, 0xFF); // ATA PIO is polled; no slave IRQ handlers are installed.
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
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);
    idt_set_gate(129, (uint32_t)isr129, 0x08, 0x8E);

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
    if (current_task &&
        (r->int_no == 32 || r->int_no == 128 || r->int_no == 129 ||
         (r->int_no < 32 && (r->cs & 3) == 3))) {
        current_task->esp = (uint32_t)r;
    }

    if (r->int_no == 128) {
        if ((r->cs & 3) != 3 || !current_task) {
            r->eax = (uint32_t)-1;
            return;
        }

        switch (r->eax) {
        case SYSCALL_WRITE:
            if (!r->edx) {
                r->eax = 0;
                return;
            }
            if (r->edx > SYSCALL_MAX_IO ||
                !paging_user_range_valid(r->ecx, r->edx)) {
                r->eax = (uint32_t)-1;
            } else if (r->ebx == SYSCALL_STDOUT ||
                       r->ebx == SYSCALL_STDERR) {
                terminal_write((const char*)r->ecx, r->edx);
                r->eax = r->edx;
            } else {
                user_file_t* file = user_file_for((int)r->ebx);
                r->eax = file
                    ? (uint32_t)write_user_file(
                          file, (const uint8_t*)r->ecx, r->edx)
                    : (uint32_t)-1;
            }
            return;
        case SYSCALL_EXIT:
            close_task_files(current_task->pid);
            task_process_exit((int)r->ebx);
            r->eax = 0;
            return;
        case SYSCALL_READ: {
            if (r->edx > SYSCALL_MAX_IO ||
                (r->edx &&
                 !paging_user_range_writable(r->ecx, r->edx))) {
                r->eax = (uint32_t)-1;
                return;
            }
            if (r->ebx == 0) {
                r->eax = (uint32_t)task_read_stdin(r->ecx, r->edx);
                return;
            }
            user_file_t* file = user_file_for((int)r->ebx);
            if (!file ||
                (file->flags & SYSCALL_O_ACCMODE) == SYSCALL_O_WRONLY) {
                r->eax = (uint32_t)-1;
                return;
            }
            uint32_t available = file->size - file->position;
            uint32_t amount = r->edx < available ? r->edx : available;
            if (amount) {
                memcpy((void*)r->ecx, file->contents + file->position,
                       amount);
                file->position += amount;
            }
            r->eax = amount;
            return;
        }
        case SYSCALL_OPEN:
            r->eax = (uint32_t)open_user_file(r->ebx, r->ecx);
            return;
        case SYSCALL_CLOSE: {
            int descriptor = (int)r->ebx;
            if (descriptor == SYSCALL_STDOUT ||
                descriptor == SYSCALL_STDERR) {
                r->eax = 0;
                return;
            }
            user_file_t* file = user_file_for(descriptor);
            r->eax = file
                ? (uint32_t)(close_user_file(file) ? 0 : -1)
                : (uint32_t)-1;
            return;
        }
        case SYSCALL_BRK:
            r->eax = (uint32_t)set_user_heap_break(r->ebx);
            return;
        case SYSCALL_LSEEK:
            r->eax = (uint32_t)seek_user_file(
                user_file_for((int)r->ebx), (int32_t)r->ecx,
                (int)r->edx);
            return;
        case SYSCALL_SPAWN:
            r->eax = (uint32_t)spawn_user_process(r->ebx, r->ecx, r->edx);
            return;
        case SYSCALL_WAITPID:
            if (r->edx) {
                r->eax = (uint32_t)-1;
            } else {
                r->eax = (uint32_t)task_wait_process(
                    (int)r->ebx, r->ecx);
            }
            return;
        case SYSCALL_GETPID:
            r->eax = (uint32_t)current_task->pid;
            return;
        default:
            r->eax = (uint32_t)-1;
            return;
        }
    }

    if (r->int_no < 32) {
        uint32_t faulting_address;
        if ((r->cs & 3) == 3 && current_task) {
            current_task->esp = (uint32_t)r;
        }

        if (r->int_no == 14 && current_task &&
            current_task->user_test_state &&
            current_task->user_test_state->result == RING3_TEST_RUNNING &&
            (r->cs & 3) == 3 &&
            (r->err_code & 7) == 5 &&
            r->eip == 0x40000000 +
                ((uint32_t)user_probe_fault - (uint32_t)user_mode_demo_start)) {
            asm volatile("mov %%cr2, %0" : "=r"(faulting_address));
            if (faulting_address == 0x00100000) {
                current_task->user_test_state->result = RING3_TEST_BLOCKED;
                r->eip = 0x40000000 +
                    ((uint32_t)user_probe_resume - (uint32_t)user_mode_demo_start);
                return;
            }
        }

        if ((r->cs & 3) == 3 && current_task) {
            char task_pid[16];
            char exception_number[16];
            printf("User task ");
            inttostr((uint32_t)current_task->pid, task_pid);
            printf(task_pid);
            printf(" terminated by exception ");
            inttostr(r->int_no, exception_number);
            printf(exception_number);
            printf(".\n");
            close_task_files(current_task->pid);
            task_process_exit(128 + (int)r->int_no);
            return;
        }

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

    if (r->int_no == 129 && current_task) {
        current_task->esp = (uint32_t)r;
    }

    // Обработка клавиатуры
    if (r->int_no == 33) {
        keyboard_handler();
    }

    // СИГНАЛ EOI ДЛЯ АППАРАТНЫХ ПРЕРЫВАНИЙ (ОБЯЗАТЕЛЕН СТРОГО В КОНЦЕ!)
    if (r->int_no >= 32 && r->int_no <= 47) {
        if (r->int_no >= 40) {
            outb(0xA0, 0x20); // Направляем Slave PIC
        }
        outb(0x20, 0x20);     // Направляем Master PIC
    }
    
}
