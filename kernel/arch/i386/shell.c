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
        
        shell_execute(); // Выполняем команду
        if (tty->shell_capacity > 32) {
            kfree(tty->shell_buf);
            tty->shell_capacity = 32;
            tty->shell_buf = (char*)kmalloc(tty->shell_capacity);
        }
        tty->shell_idx = 0; // Сбрасываем индекс активной консоли
        shell_prompt();
    } 
    else if (c == '\b') {
         if (tty->shell_idx > 0) {
            tty->shell_idx--;
            terminal_putchar('\b');
        }
    } 
    else {
        // Безопасное сохранение в буфер виртуального окна (лимит 256 байт)
        if (tty->shell_idx >= tty->shell_capacity - 1) {
            size_t new_capacity = tty->shell_capacity + 32; // Увеличиваем шаг на 32 байта
            char* new_buffer = (char*)kmalloc(new_capacity);
            
            // Копируем старые накопленные символы в новый блок кучи
            for (size_t i = 0; i < tty->shell_idx; i++) {
                new_buffer[i] = tty->shell_buf[i];
            }
            
            kfree(tty->shell_buf); // Освобождаем старый маленький буфер
            tty->shell_buf = new_buffer;
            tty->shell_capacity = new_capacity;
        }
        
        // Безопасно сохраняем символ
        tty->shell_buf[tty->shell_idx++] = c;
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
        printf("  cat <file> - Display a FAT16 file\n");
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
    // Команда cat ожидает имя файла, поэтому проверяем подстроку через strncmp
    else if (strncmp(cmd, "cat ", 4) == 0) {
        // cmd + 4 указывает на начало имени файла (пропускаем "cat ")
        fat16_cat(cmd + 4);
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
