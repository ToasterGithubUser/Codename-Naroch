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
#include <kernel/ring3.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/serial.h>
#include <kernel/rtc.h>
#include <kernel/pci.h>
#include <kernel/net.h>
#include <kernel/pc_speaker.h>
#include <kernel/ps2_mouse.h>
#include <kernel/video.h>
#include <kernel/ata.h>
#include <kernel/vfs.h>
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

static const char* const shell_command_names[] = {
    "beep", "cat", "cd", "clear", "date", "devices", "exec", "format",
    "help", "crash", "display", "ktest", "lspci", "ls", "malloc", "mkdir", "mouse",
    "netinfo", "pagefault", "ring3check", "rm", "rmdir", "shellstat",
    "sleep", "touch", "udp", "udp-recv", "version", "write"
};

static void shell_complete_command(virtual_tty_t* tty) {
    size_t prefix_length = tty->shell_idx;
    for (size_t i = 0; i < prefix_length; i++) {
        if (tty->shell_buf[i] == ' ' || tty->shell_buf[i] == '\t') {
            return;
        }
    }

    for (size_t i = 0; i < sizeof(shell_command_names) /
                                sizeof(shell_command_names[0]); i++) {
        if (strcmp(shell_command_names[i], tty->shell_buf) == 0) {
            shell_input_char(' ');
            return;
        }
    }

    const char* match = 0;
    size_t match_count = 0;
    for (size_t i = 0; i < sizeof(shell_command_names) /
                                sizeof(shell_command_names[0]); i++) {
        const char* candidate = shell_command_names[i];
        if (strncmp(candidate, tty->shell_buf, prefix_length) == 0) {
            match = candidate;
            match_count++;
        }
    }
    if (!match_count) {
        return;
    }

    if (match_count == 1) {
        size_t length = strlen(match);
        while (tty->shell_idx < length) {
            shell_input_char(match[tty->shell_idx]);
        }
        shell_input_char(' ');
        return;
    }

    terminal_putchar('\n');
    for (size_t i = 0; i < sizeof(shell_command_names) /
                                sizeof(shell_command_names[0]); i++) {
        const char* candidate = shell_command_names[i];
        if (strncmp(candidate, tty->shell_buf, prefix_length) == 0) {
            printf("%s  ", candidate);
        }
    }
    terminal_putchar('\n');
    shell_prompt();
    terminal_write(tty->shell_buf, tty->shell_idx);
}

static void shell_print_hex(uint32_t value, uint32_t digits) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t shift = digits * 4; shift; shift -= 4) {
        putchar(hex[(value >> (shift - 4)) & 0xF]);
    }
}

static void shell_print_ipv4(uint32_t address) {
    printf("%u.%u.%u.%u", (address >> 24) & 0xFF,
           (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
}

static int shell_parse_port(const char* text, uint16_t* port) {
    if (!text || !*text || !port) {
        return 0;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (65535U - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }
    if (*text || !value) {
        return 0;
    }
    *port = (uint16_t)value;
    return 1;
}

static int shell_parse_u32(const char* text, uint32_t* value) {
    if (!text || !*text || !value) {
        return 0;
    }
    uint32_t result = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (result > (UINT32_MAX - digit) / 10) {
            return 0;
        }
        result = result * 10 + digit;
    }
    if (*text) {
        return 0;
    }
    *value = result;
    return 1;
}

static void shell_print_pci_details(const pci_device_t* device) {
    uint8_t header_type = device->header_type & 0x7F;
    uint32_t bar_count = header_type == 0 ? 6 : header_type == 1 ? 2 : 1;
    for (uint32_t i = 0; i < bar_count; i++) {
        uint8_t offset = (uint8_t)(0x10 + i * 4);
        uint32_t bar = pci_read_config32(device->bus, device->device,
                                         device->function, offset);
        if (!bar || bar == 0xFFFFFFFF) {
            continue;
        }

        printf("  BAR%u ", i);
        if (bar & 1) {
            printf("I/O 0x");
            shell_print_hex(bar & 0xFFFFFFFC, 8);
        } else {
            uint32_t type = (bar >> 1) & 3;
            if (type == 2 && i + 1 < bar_count) {
                uint32_t high = pci_read_config32(
                    device->bus, device->device, device->function,
                    (uint8_t)(offset + 4));
                printf("MMIO64 0x");
                shell_print_hex(high, 8);
                shell_print_hex(bar & 0xFFFFFFF0, 8);
                i++;
            } else if (type == 0 || type == 1) {
                printf("MMIO32 0x");
                shell_print_hex(bar & (type == 1 ? 0x000FFFF0 : 0xFFFFFFF0),
                                8);
            } else {
                printf("reserved (0x");
                shell_print_hex(bar, 8);
                putchar(')');
            }
        }
        putchar('\n');
    }

    uint32_t status_command = pci_read_config32(
        device->bus, device->device, device->function, 0x04);
    if (!((status_command >> 20) & 1)) {
        printf("  Capabilities: none\n");
        return;
    }

    uint8_t pointer_offset = header_type == 2 ? 0x14 : 0x34;
    uint8_t pointer = (uint8_t)pci_read_config32(
        device->bus, device->device, device->function, pointer_offset);
    uint8_t visited[64] = {0};
    uint32_t count = 0;
    while (pointer >= 0x40 && pointer <= 0xFC && count++ < 48) {
        pointer &= 0xFC;
        uint32_t slot = pointer >> 2;
        if (visited[slot]) {
            printf("  Capabilities: malformed list (cycle)\n");
            return;
        }
        visited[slot] = 1;
        uint32_t capability = pci_read_config32(
            device->bus, device->device, device->function, pointer);
        printf("  Capability 0x");
        shell_print_hex(capability & 0xFF, 2);
        printf(" at 0x");
        shell_print_hex(pointer, 2);
        putchar('\n');
        pointer = (uint8_t)(capability >> 8);
    }
    if (pointer && (pointer < 0x40 || pointer > 0xFC)) {
        printf("  Capabilities: malformed pointer\n");
    } else if (count >= 48 && pointer) {
        printf("  Capabilities: list limit reached\n");
    }
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

static int shell_parse_arguments(char* command, char* argv[],
                                 int capacity) {
    char* read = command;
    char* write = command;
    int argc = 0;
    while (*read) {
        while (*read == ' ' || *read == '\t') {
            read++;
        }
        if (!*read) {
            break;
        }
        if (argc == capacity) {
            return -1;
        }
        argv[argc++] = write;
        char quote = '\0';
        while (*read) {
            if ((*read == '"' || *read == '\'') &&
                (!quote || quote == *read)) {
                quote = quote ? '\0' : *read;
                read++;
                continue;
            }
            if (!quote && (*read == ' ' || *read == '\t')) {
                break;
            }
            if (*read == '\\' && read[1] &&
                (read[1] == '"' || read[1] == '\'' || read[1] == '\\')) {
                read++;
            }
            *write++ = *read++;
        }
        if (quote) {
            return -1;
        }
        if (*read) {
            read++;
        }
        *write++ = '\0';
    }
    return argc;
}

static void shell_launch_elf(char* command) {
    char* argv[16];
    int argc = shell_parse_arguments(command, argv, 16);
    if (argc <= 0) {
        printf("Usage: ./program.elf [arguments] (quote arguments with spaces)\n");
        return;
    }

    int background = argc > 1 && strcmp(argv[argc - 1], "&") == 0;
    if (background) {
        argc--;
    }

    const char* path = argv[0];
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    if (!*path) {
        printf("Executable path is empty.\n");
        return;
    }

    void* image;
    size_t image_size;
    if (!vfs_read_file(path, &image, &image_size)) {
        printf("Could not read executable: %s\n", argv[0]);
        return;
    }
    int process_id;
    int started = task_create_elf(image, image_size, argc,
                                  (const char* const*)argv,
                                  current_task ? current_task->pid : 0,
                                  &process_id);
    kfree(image);
    if (!started) {
        printf("Invalid ELF32/i386 executable or insufficient memory.\n");
        return;
    }
    char pid[16];
    inttostr((uint32_t)process_id, pid);
    if (background) {
        printf("Started background ELF process %s.\n", pid);
        return;
    }

    syscall_set_foreground_pid(process_id);
    int status = 0;
    while (task_process_status(process_id, &status) == 0) {
        task_yield();
    }
    syscall_set_foreground_pid(0);
    printf("ELF process %s exited with status ", pid);
    char exit_status[16];
    inttostr((uint32_t)status, exit_status);
    printf(exit_status);
    printf(".\n");
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
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    virtual_tty_t* tty = tty_get_current();
    terminal_setcolor(vga_entry_color(VGA_COLOR_GREEN, VGA_COLOR_BLACK));
    printf("naroch:%s> ", vfs_getcwd());
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    if (tty) {
        tty->prompt_initialized = 1;
        tty->prompt_pending = 0;
    }
    if (flags & 0x200) {
        asm volatile("sti" : : : "memory");
    }
}

void shell_input_char(char c) {
    virtual_tty_t* tty = tty_get_current();
    if (!tty || tty->command_active) {
        return;
    }

    if (c == '\t') {
        shell_complete_command(tty);
    }
    else if (c == '\n') {
        terminal_putchar('\n');
        while (tty->shell_idx &&
               (tty->shell_buf[tty->shell_idx - 1] == ' ' ||
                tty->shell_buf[tty->shell_idx - 1] == '\t')) {
            tty->shell_idx--;
        }
        tty->shell_buf[tty->shell_idx] = '\0'; // Финализируем строку в TTY
        shell_remember_command(tty);
        shell_clear_history_navigation(tty);
        tty->command_pending = 1;
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

void shell_process_pending(void) {
    virtual_tty_t* tty = tty_get_current();
    if (!tty) {
        return;
    }
    if (tty->prompt_pending && !tty->prompt_initialized) {
        shell_prompt();
        return;
    }
    if (!tty->command_pending || tty->command_active) {
        return;
    }

    tty->command_active = 1;
    tty->command_pending = 0;
    shell_execute();
    tty->shell_idx = 0;
    tty->shell_buf[0] = '\0';
    tty->command_active = 0;
    shell_prompt();
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
        printf("  ls      - List the current FAT16 directory\n");
        printf("  cd <dir> - Change the current FAT16 directory\n");
        printf("  mkdir <dir> - Create a FAT16 directory\n");
        printf("  rm <file> - Remove a file (not an open file)\n");
        printf("  rmdir <dir> - Remove an empty directory\n");
        printf("  cat <file> - Display a FAT16 file (quote names with spaces)\n");
        printf("  touch <file> - Create an empty file (long names supported)\n");
        printf("  write <file> <data> - Create or replace a file (quote spaces)\n");
        printf("  format              - Create a FAT16 filesystem on the primary disk\n");
        printf("  format yes          - Confirm formatting (destroys filesystem metadata)\n");
        printf("  exec <file> [args] - Run a static ELF32 i386 executable\n");
        printf("  ./app.elf [args] - Run an ELF directly with arguments\n");
        printf("  ELF programs also support malloc/free and basic file I/O\n");
        printf("  ring3check - Test user-mode access to kernel memory\n");
        printf("  ring3check reports the boot-time user-mode protection test\n");
        printf("  date    - Read the CMOS real-time clock\n");
        printf("  display - Show the active framebuffer mode\n");
        printf("  devices - Show detected devices\n");
        printf("  lspci   - List enumerated PCI devices\n");
        printf("  netinfo - Show network device and static IPv4 settings\n");
        printf("  udp <ip> <port> <message> - Send a UDP datagram\n");
        printf("  udp-recv - Show queued UDP datagrams\n");
        printf("  mouse   - Show PS/2 mouse position and buttons\n");
        printf("  beep <Hz> <ms> - Play a PC speaker tone\n");
    }
    else if (strcmp(cmd, "format") == 0) {
        printf("Formatting destroys the current filesystem and may replace the partition table.\n");
        printf("This is a quick format; it does not securely erase old file contents.\n");
        printf("Type 'format yes' to confirm.\n");
    }
    else if (strcmp(cmd, "format yes") == 0) {
        if (vfs_format_fat16()) {
            printf("FAT16 format completed and mounted.\n");
        } else {
            printf("FAT16 format failed; close open files and check disk status.\n");
        }
    }
    else if (strcmp(cmd, "shellstat") == 0) {
        char diag_buf[32];
        printf("Active TTY Shell Buffer Stats:\n");
        inttostr(tty->shell_idx, diag_buf);      printf("  Current length: "); printf(diag_buf); printf(" bytes\n");
        inttostr(tty->shell_capacity, diag_buf); printf("  Current capacity: "); printf(diag_buf); printf(" bytes\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        terminal_clear();
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
    else if (strcmp(cmd, "display") == 0) {
        if (video_is_framebuffer()) {
            printf("Framebuffer: %u x %u, %u bpp\n",
                   video_width(), video_height(), video_bpp());
            printf("Console: %u x %u cells\n",
                   (uint32_t)terminal_columns(), (uint32_t)terminal_rows());
        } else {
            printf("Framebuffer unavailable; using VGA text mode (80 x 25).\n");
        }
        printf("Select a boot mode in GRUB; run videoinfo there to list "
               "monitor-supported modes.\n");
    }
    else if (strcmp(cmd, "date") == 0) {
        rtc_time_t time;
        if (rtc_read_time(&time)) {
            printf("%u-%u-%u %u:%u:%u\n", time.year, time.month, time.day,
                   time.hour, time.minute, time.second);
        } else {
            printf("RTC time is unavailable or invalid.\n");
        }
    }
    else if (strcmp(cmd, "devices") == 0) {
        rtc_time_t time;
        printf("COM1 serial: %s\n",
               serial_is_available() ? "available" : "not detected");
        printf("Storage: %s\n", ata_backend_name());
        printf("CMOS RTC: %s\n",
               rtc_read_time(&time) ? "available" : "not detected");
        if (pci_device_list_truncated()) {
            printf("PCI: at least %u device(s) detected (list truncated)\n",
                   pci_device_count());
        } else {
            printf("PCI: %u device(s) detected\n", pci_device_count());
        }
        printf("Network: %s\n", net_driver_name());
        printf("PS/2 mouse: %s\n",
               ps2_mouse_is_available() ? "available" : "not detected");
    }
    else if (strcmp(cmd, "netinfo") == 0) {
        const uint8_t* mac = net_mac_address();
        if (!mac) {
            printf("No supported network device detected.\n");
        } else {
            printf("Network: %s\nMAC: ", net_driver_name());
            for (uint32_t i = 0; i < 6; i++) {
                if (i) {
                    putchar(':');
                }
                shell_print_hex(mac[i], 2);
            }
            printf("\nIPv4: ");
            shell_print_ipv4(net_ipv4_address());
            printf("/24, gateway 10.0.2.2 (QEMU user networking)\n");
            printf("RX frames: %u, ARP entries: %u\n",
                   net_received_frame_count(), net_arp_cache_count());
            printf("UDP packets dropped: %u\n", net_udp_dropped_count());
        }
    }
    else if (strncmp(cmd, "udp-recv", 8) == 0 &&
             (cmd[8] == '\0' || cmd[8] == ' ')) {
        net_udp_packet_t packet;
        uint32_t received = 0;
        while (net_udp_receive(&packet)) {
            printf("UDP from ");
            shell_print_ipv4(packet.source_ip);
            printf(":%u, %u bytes: ", packet.source_port, packet.length);
            for (uint32_t i = 0; i < packet.length; i++) {
                uint8_t character = packet.payload[i];
                putchar(character >= 32 && character <= 126
                    ? (char)character : '.');
            }
            putchar('\n');
            received++;
        }
        if (!received) {
            printf("No UDP datagrams queued.\n");
        }
    }
    else if (strncmp(cmd, "udp ", 4) == 0) {
        char* arguments[5];
        int argc = shell_parse_arguments(cmd, arguments, 5);
        uint32_t destination_ip;
        uint16_t destination_port;
        if (argc != 4 ||
            !net_parse_ipv4(arguments[1], &destination_ip) ||
            !shell_parse_port(arguments[2], &destination_port)) {
            printf("Usage: udp <IPv4-address> <port> <message>\n");
        } else {
            size_t length = strlen(arguments[3]);
            if (length > NET_UDP_MAX_PAYLOAD) {
                printf("UDP message is limited to %u bytes.\n",
                       NET_UDP_MAX_PAYLOAD);
            } else if (net_udp_send(destination_ip, destination_port,
                                    arguments[3], (uint16_t)length)) {
                printf("UDP datagram sent (%u bytes).\n", (uint32_t)length);
            } else {
                printf("UDP send failed (network, ARP, or transmit error).\n");
            }
        }
    }
    else if (strcmp(cmd, "mouse") == 0) {
        if (!ps2_mouse_is_available()) {
            printf("PS/2 mouse not detected.\n");
        } else {
            ps2_mouse_state_t state;
            ps2_mouse_get_state(&state);
            printf("PS/2 mouse at %u,%u; buttons:", state.x, state.y);
            printf(" left=%u right=%u middle=%u; packets=%u\n",
                   state.buttons & 1, (state.buttons >> 1) & 1,
                   (state.buttons >> 2) & 1, state.packets);
        }
    }
    else if (strncmp(cmd, "beep ", 5) == 0) {
        char* arguments[4];
        uint32_t frequency;
        uint32_t duration;
        int argc = shell_parse_arguments(cmd, arguments, 4);
        if (argc != 3 ||
            !shell_parse_u32(arguments[1], &frequency) ||
            !shell_parse_u32(arguments[2], &duration) ||
            frequency < 20 || frequency > 20000 ||
            duration < 1 || duration > 5000) {
            printf("Usage: beep <frequency 20-20000 Hz> <duration 1-5000 ms>\n");
        } else {
            pc_speaker_beep(frequency, duration);
            printf("Played %u Hz for %u ms.\n", frequency, duration);
        }
    }
    else if (strcmp(cmd, "lspci") == 0) {
        uint32_t count = pci_device_count();
        if (!count) {
            printf("No PCI devices detected.\n");
        }
        for (uint32_t i = 0; i < count; i++) {
            const pci_device_t* device = pci_device_at(i);
            if (!device) {
                continue;
            }
            shell_print_hex(device->bus, 2);
            putchar(':');
            shell_print_hex(device->device, 2);
            printf(".%u ", device->function);
            shell_print_hex(device->vendor_id, 4);
            putchar(':');
            shell_print_hex(device->device_id, 4);
            printf(" class ");
            shell_print_hex(device->class_code, 2);
            putchar(':');
            shell_print_hex(device->subclass, 2);
            putchar(':');
            shell_print_hex(device->prog_if, 2);
            putchar('\n');
            shell_print_pci_details(device);
        }
        if (pci_device_list_truncated()) {
            printf("PCI device list truncated at %u entries.\n",
                   PCI_MAX_DEVICES);
        }
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
        vfs_list();
    }
    else if (strncmp(cmd, "cd ", 3) == 0) {
        char* path = parse_filename_argument(cmd + 3);
        if (path) {
            vfs_chdir(path);
        } else {
            printf("Usage: cd <directory> (quote names containing spaces)\n");
        }
    }
    else if (strncmp(cmd, "mkdir ", 6) == 0) {
        char* name = parse_filename_argument(cmd + 6);
        if (name) {
            vfs_mkdir(name);
        } else {
            printf("Usage: mkdir <directory> (quote names containing spaces)\n");
        }
    }
    else if (strncmp(cmd, "rmdir ", 6) == 0) {
        char* path = parse_filename_argument(cmd + 6);
        if (!path) {
            printf("Usage: rmdir <empty-directory> (quote names containing spaces)\n");
        } else if (vfs_rmdir(path)) {
            printf("Removed directory: %s\n", path);
        } else {
            printf("rmdir: cannot remove '%s' (not found, not empty, or current directory)\n",
                   path);
        }
    }
    else if (strncmp(cmd, "rm ", 3) == 0) {
        char* path = parse_filename_argument(cmd + 3);
        if (!path) {
            printf("Usage: rm <file> (quote names containing spaces)\n");
        } else if (vfs_unlink(path)) {
            printf("Removed file: %s\n", path);
        } else {
            printf("rm: cannot remove '%s' (not found, a directory, or open)\n",
                   path);
        }
    }
    else if (strcmp(cmd, "ring3check") == 0) {
        if (!ring3_test_state) {
            printf("Ring 3 test is not initialized.\n");
        } else if (ring3_test_state->result == RING3_TEST_RUNNING) {
            printf("Ring 3 test is still running; try again shortly.\n");
        } else if (ring3_test_state->result == RING3_TEST_BLOCKED) {
            printf("PASS: Ring 3 was blocked from reading kernel memory.\n");
        } else if (ring3_test_state->result == RING3_TEST_BYPASSED) {
            printf("FAIL: Ring 3 read supervisor-only kernel memory.\n");
        } else {
            printf("Ring 3 protection test has not completed.\n");
        }
    }
    // Внутри shell_execute() в вашем shell.c:
    else if (strncmp(cmd, "touch ", 6) == 0) {
        char* filename = parse_filename_argument(cmd + 6);
        if (filename) {
            vfs_touch(filename);
        } else {
            printf("Usage: touch <filename> (quote names containing spaces)\n");
        }
    }
    // Команда cat ожидает имя файла, поэтому проверяем подстроку через strncmp
    else if (strncmp(cmd, "cat ", 4) == 0) {
        char* filename = parse_filename_argument(cmd + 4);
        if (filename) {
            vfs_cat(filename);
        } else {
            printf("Usage: cat <filename> (quote names containing spaces)\n");
        }
    }
    else if (strncmp(cmd, "exec ", 5) == 0) {
        shell_launch_elf(cmd + 5);
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
        vfs_write_text(filename, data);
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
    else if (cmd[0] == '.' && cmd[1] == '/') {
        shell_launch_elf(cmd);
    }
    else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
        printf("Unknown command: %s\n", cmd); 
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }
}
