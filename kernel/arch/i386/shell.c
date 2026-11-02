#include <kernel/shell.h>
#include <kernel/tty.h>
#include <stdio.h>
#include <string.h>
#include <kernel/conversions.h>
#include "vga.h" 
#include <kernel/build_no.h> 
#include <kernel/timer.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/fat16.h>
#include <string.h>
int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static char* parse_filename_argument(char* argument) {
    if (*argument != '"') {
        return argument;
    }

    char* filename = argument + 1;
    char* end = filename;
    while (*end && *end != '"') {
        end++;
    }
    if (!*end || end[1] != '\0') {
        return 0;
    }
    *end = '\0';
    return filename;
}

static int shell_reserve(virtual_tty_t* tty, size_t required) {
    if (required <= tty->shell_capacity) {
        return 1;
    }

    size_t capacity = tty->shell_capacity;
    while (capacity < required) {
        capacity += 32;
    }
    char* new_buffer = (char*)kmalloc(capacity);
    if (!new_buffer) {
        return 0;
    }
    memcpy(new_buffer, tty->shell_buf, tty->shell_idx);
    new_buffer[tty->shell_idx] = '\0';
    kfree(tty->shell_buf);
    tty->shell_buf = new_buffer;
    tty->shell_capacity = capacity;
    return 1;
}

static void shell_replace_input(virtual_tty_t* tty, const char* text,
                                size_t length) {
    while (tty->shell_idx) {
        terminal_putchar('\b');
        tty->shell_idx--;
    }
    tty->shell_buf[0] = '\0';

    if (!shell_reserve(tty, length + 1)) {
        printf("Shell: Not enough memory to restore command line.\n");
        return;
    }
    memcpy(tty->shell_buf, text, length);
    tty->shell_buf[length] = '\0';
    tty->shell_idx = length;
    terminal_write(text, length);
}

static void shell_clear_history_navigation(virtual_tty_t* tty) {
    tty->history_position = 0;
    if (tty->history_draft) {
        kfree(tty->history_draft);
        tty->history_draft = 0;
    }
    tty->history_draft_length = 0;
}

static void shell_remember_command(virtual_tty_t* tty) {
    if (!tty->shell_idx) {
        return;
    }

    char* command = (char*)kmalloc(tty->shell_idx + 1);
    if (!command) {
        printf("Shell: Not enough memory to save command history.\n");
        return;
    }
    memcpy(command, tty->shell_buf, tty->shell_idx + 1);

    if (tty->command_history[tty->history_next]) {
        kfree(tty->command_history[tty->history_next]);
    }
    tty->command_history[tty->history_next] = command;
    tty->history_next =
        (tty->history_next + 1) % TTY_COMMAND_HISTORY_SIZE;
    if (tty->history_count < TTY_COMMAND_HISTORY_SIZE) {
        tty->history_count++;
    }
}

void shell_history_up(void) {
    virtual_tty_t* tty = tty_get_current();
    if (!tty || !tty->history_count ||
        tty->history_position >= tty->history_count) {
        return;
    }

    if (!tty->history_position) {
        tty->history_draft = (char*)kmalloc(tty->shell_idx + 1);
        if (!tty->history_draft) {
            printf("Shell: Not enough memory to navigate command history.\n");
            return;
        }
        memcpy(tty->history_draft, tty->shell_buf, tty->shell_idx);
        tty->history_draft[tty->shell_idx] = '\0';
        tty->history_draft_length = tty->shell_idx;
    }

    tty->history_position++;
    size_t index = (tty->history_next + TTY_COMMAND_HISTORY_SIZE -
                    tty->history_position) % TTY_COMMAND_HISTORY_SIZE;
    const char* command = tty->command_history[index];
    shell_replace_input(tty, command, strlen(command));
}

void shell_history_down(void) {
    virtual_tty_t* tty = tty_get_current();
    if (!tty || !tty->history_position) {
        return;
    }

    tty->history_position--;
    if (!tty->history_position) {
        shell_replace_input(tty, tty->history_draft,
                            tty->history_draft_length);
        kfree(tty->history_draft);
        tty->history_draft = 0;
        tty->history_draft_length = 0;
        return;
    }

    size_t index = (tty->history_next + TTY_COMMAND_HISTORY_SIZE -
                    tty->history_position) % TTY_COMMAND_HISTORY_SIZE;
    const char* command = tty->command_history[index];
    shell_replace_input(tty, command, strlen(command));
}

void shell_prompt(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    printf("naroch> ");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

void shell_input_char(char c) {
    virtual_tty_t* tty = tty_get_current();

    if (c == '\n') {
        terminal_putchar('\n');
        tty->shell_buf[tty->shell_idx] = '\0'; // Финализируем строку в TTY
        shell_remember_command(tty);
        shell_clear_history_navigation(tty);
        shell_execute(); // Выполняем команду
        tty->shell_idx = 0; // Сбрасываем индекс активной консоли
        tty->shell_buf[0] = '\0';
        shell_prompt();
    } 
    else if (c == '\b') {
         if (tty->shell_idx > 0) {
            tty->shell_idx--;
            terminal_putchar('\b');
        }
    } 
    else {
        if (!shell_reserve(tty, tty->shell_idx + 2)) {
            printf("Shell: Not enough memory for command line.\n");
            return;
        }
        
        tty->shell_buf[tty->shell_idx++] = c;
        tty->shell_buf[tty->shell_idx] = '\0';
        terminal_putchar(c);
    }
    
}

void shell_execute(void) {
    virtual_tty_t* tty = tty_get_current();
    char* cmd = tty->shell_buf;

    // ИСПРАВЛЕНО: Проверяем реальный индекс активного TTY буфера!
    if (tty->shell_idx == 0) {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        printf("Available commands:\n");
        printf("  help    - Display this information\n");
        printf("  clear   - Clear the current screen\n");
        printf("  version - Print OS architecture details\n");
        printf("  crash   - Trigger division by zero exception\n");
        printf("  sleep   - Delay system for 3000 ms\n");
        printf("  malloc  - Test PMM allocation\n");
        printf("  ktest   - Test Kernel Heap allocation\n");
        printf("  pagefault - Trigger page fault exception\n");
        printf("  ls      - List files in the FAT16 root directory\n");
        printf("  cat <file> - Display a FAT16 file (quote names with spaces)\n");
        printf("  touch <file> - Create an empty file (long names supported)\n");
        printf("  write <file> <data> - Create or replace a file (quote spaces)\n");
    } 
    else if (strcmp(cmd, "shellstat") == 0) {
        char diag_buf[32];
        printf("Active TTY Shell Buffer Stats:\n");
        inttostr(tty->shell_idx, diag_buf);      printf("  Current length: "); printf(diag_buf); printf(" bytes\n");
        inttostr(tty->shell_capacity, diag_buf); printf("  Current capacity: "); printf(diag_buf); printf(" bytes\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        tty->column = 0; tty->row = 0;
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            tty->buffer[i] = ' ' | (tty->color << 8);
            ((uint16_t*)0xB8000)[i] = tty->buffer[i];
        }
        terminal_update_cursor();
    } 
    else if (strcmp(cmd, "version") == 0) {
        char buf[16];
        inttostr(KERNEL_BUILD_NUMBER, buf);
        printf("Codename Naroch OS [v0.1-milestone2-build-");
        printf(buf);
        printf("]\n");
        printf("Architecture: i386 Protected Mode (Ring 0)\n");
        printf("Compiled on: "); printf(__DATE__); printf(" "); printf(__TIME__); printf("\n");
    }
    else if (strcmp(cmd, "crash") == 0) {
        volatile int num = 5;
        volatile int zero = 0;
        volatile int result = num / zero; 
        (void)result;
    }
    else if (strcmp(cmd, "sleep") == 0) {
        printf("Sleeping for 3 seconds...\n");
        sleep(3000); 
        printf("Awake! PIT timer works perfectly.\n");
    }
    else if (strcmp(cmd, "ls") == 0) {
        fat16_ls();
    }
    // Внутри shell_execute() в вашем shell.c:
    else if (strncmp(cmd, "touch ", 6) == 0) {
        char* filename = parse_filename_argument(cmd + 6);
        if (filename) {
            fat16_touch(filename);
        } else {
            printf("Usage: touch <filename> (quote names containing spaces)\n");
        }
    }
    // Команда cat ожидает имя файла, поэтому проверяем подстроку через strncmp
    else if (strncmp(cmd, "cat ", 4) == 0) {
        char* filename = parse_filename_argument(cmd + 4);
        if (filename) {
            fat16_cat(filename);
        } else {
            printf("Usage: cat <filename> (quote names containing spaces)\n");
        }
    }
    else if (strncmp(cmd, "write ", 6) == 0) {
        char* filename = cmd + 6;
        char* data;
        if (*filename == '"') {
            filename++;
            data = filename;
            while (*data && *data != '"') {
                data++;
            }
            if (!*data || data[1] != ' ') {
                printf("Usage: write <filename> <data> (quote names containing spaces)\n");
                return;
            }
            *data++ = '\0';
            data++;
        } else {
            data = filename;
            while (*data && *data != ' ') {
                data++;
            }
            if (!*data) {
                printf("Usage: write <filename> <data>\n");
                return;
            }
            *data++ = '\0';
        }
        fat16_write(filename, data);
    }
    else if (strcmp(cmd, "malloc") == 0) {
        extern uint32_t pmm_max_blocks;
        extern uint32_t pmm_used_blocks;

        char diag_buf[32];
        printf("PMM Stats:\n");
        inttostr(pmm_max_blocks, diag_buf);  printf("  Total blocks: "); printf(diag_buf); printf("\n");
        inttostr(pmm_used_blocks, diag_buf); printf("  Used blocks:  "); printf(diag_buf); printf("\n");

        void* page1 = pmm_alloc_block();
        void* page2 = pmm_alloc_block();

        char buf[32];
        printf("Allocated page 1 at: 0x");
        inttohex((uint32_t)page1, buf); printf(buf); printf("\n");

        printf("Allocated page 2 at: 0x");
        inttohex((uint32_t)page2, buf); printf(buf); printf("\n");

        if (page1) pmm_free_block(page1);
        if (page2) pmm_free_block(page2);
    }
    else if (strcmp(cmd, "pagefault") == 0) {
        printf("Reading unmapped memory region... Triggering Page Fault.\n");
        volatile uint32_t* ptr = (volatile uint32_t*)0xA0000000;
        volatile uint32_t val = *ptr; 
        (void)val;
    }
    else if (strcmp(cmd, "ktest") == 0) {
        printf("Allocating custom buffers inside Kernel Heap...\n");

        char* str1 = (char*)kmalloc(16);
        char* str2 = (char*)kmalloc(64);

        char buf[16];
        printf("Buffer 1 (16 bytes) address: 0x");
        inttohex((uint32_t)str1, buf); printf(buf); printf("\n");

        printf("Buffer 2 (64 bytes) address: 0x");
        inttohex((uint32_t)str2, buf); printf(buf); printf("\n");

        str1[0] = 'O'; str1[1] = 'K'; str1[2] = '\0';
        printf("Testing data inside heap string: "); printf(str1); printf("\n");

        kfree(str1);
        kfree(str2);
        printf("Heap blocks successfully cleaned up.\n");
    }
    else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
        printf("Unknown command: %s\n", cmd); 
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }
}
