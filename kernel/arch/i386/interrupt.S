.section .text
.equ SYSCALL_WRITE, 1
.equ SYSCALL_EXIT, 2
.equ SYSCALL_STDOUT, 1

.global idt_load
.global _idt_load
.extern idt_ptr
.extern _idt_ptr

idt_load:
_idt_load:
    lidt [idt_ptr]
    ret

# --- МАКРОСЫ ---

.macro ISR_NOERRCODE num
.global isr\num
.global _isr\num
isr\num:
_isr\num:
    cli
    push $0          # Push dummy error code
    push $\num        # Push interrupt number
    jmp irq_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
.global _isr\num
isr\num:
_isr\num:
    cli
    push $\num        # Push interrupt number
    jmp irq_common_stub
.endm


# --- ГЕНЕРАЦИЯ ОБОРОТЧИКОВ ---
# Явно вызываем макросы для векторов, которые вы регистрируете в idt.c:
ISR_NOERRCODE 1   # 1: Debug Exception
ISR_NOERRCODE 2   # 2: Non Maskable Interrupt Exception
ISR_NOERRCODE 3   # 3: Int 3 Breakpoint Exception
ISR_NOERRCODE 4   # 4: Into Detected Overflow Exception
ISR_NOERRCODE 5   # 5: Out of Bounds Exception
ISR_NOERRCODE 6   # 6: Invalid Opcode Exception
ISR_NOERRCODE 7   # 7: No Coprocessor Exception
ISR_ERRCODE   8   # 8: Double Fault Exception (С кодом ошибки!)
ISR_NOERRCODE 9   # 9: Coprocessor Segment Overrun Exception
ISR_ERRCODE   10  # 10: Bad TSS Exception (С кодом ошибки!)
ISR_ERRCODE   11  # 11: Segment Not Present Exception (С кодом ошибки!)
ISR_ERRCODE   12  # 12: Stack Fault Exception (С кодом ошибки!)
ISR_ERRCODE   13  # 13: General Protection Fault Exception (С кодом ошибки!)
ISR_ERRCODE   14  # 14: Page Fault Exception (С кодом ошибки!)
ISR_NOERRCODE 15  # 15: Unknown Interrupt Exception
ISR_NOERRCODE 16  # 16: Coprocessor Fault Exception
ISR_ERRCODE   17  # 17: Alignment Check Exception
ISR_NOERRCODE 18  # 18: Machine Check Exception
ISR_NOERRCODE 19  # 19: Reserved
ISR_NOERRCODE 0   # Для деления на ноль (isr0)

ISR_NOERRCODE 32  #IRQ0 - sys timer
ISR_NOERRCODE 33  # Для клавиатуры (isr33)
ISR_NOERRCODE 128 # User syscall interrupt
ISR_NOERRCODE 129 # Scheduler yield

# --- ОБЩИЙ СТАБ ---

.extern irq_handler
.extern _irq_handler

irq_common_stub:
    pusha           # Заталкивает в стек: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI

    mov %ds, %ax    # Сохраняем оригинальный селектор данных
    push %eax

    mov $0x10, %ax  # Загружаем селектор данных ядра (0x10)
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp       # Передаем regs_t *r как аргумент в Си
    call irq_handler
    add $4, %esp    # Счищаем аргумент

    .extern current_task
    .extern task_switch

    cmpl $32, 36(%esp)
    je 2f
    cmpl $128, 36(%esp)
    je 2f
    cmpl $129, 36(%esp)
    je 2f
    cmpl $32, 36(%esp)
    jae 1f
    testb $3, 48(%esp)
    jz 1f
2:
    movl current_task, %eax
    testl %eax, %eax
    jz 1f
    call task_switch
    movl current_task, %eax
    testl %eax, %eax
    jz 3f
    movl 4(%eax), %esp  # Принудительно подменяем стек процессора на стек новой задачи!
    jmp irq_restore_context

3:
    cli
4:
    hlt
    jmp 4b
1:
irq_restore_context:
    pop %eax        # Восстанавливаем оригинальные сегменты данных новой задачи
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    popa            # Восстанавливаем регистры общего назначения новой задачи
    add $8, %esp    # Очищаем фиктивные числа прерывания
    iret            # ВЗЛЕТАЕМ в контекст выполнения новой задачи!

.global task_start_asm
task_start_asm:
    call task_switch
    movl current_task, %eax
    movl 4(%eax), %esp
    jmp irq_restore_context

.section .usertext,"ax"
.global user_mode_demo_start
.global user_mode_demo_end
.global user_probe_fault
.global user_probe_resume
user_mode_demo_start:
    movl $1, 0x40002000
user_probe_fault:
    movl 0x00100000, %eax
    movl $3, 0x40002000
    jmp 2f
user_probe_resume:
    nop
2:
    cmpl $2, 0x40002000
    jne 3f
    call 4f
4:
    popl %ecx
    addl $(user_probe_pass_message - 4b), %ecx
    movl $(user_probe_pass_message_end - user_probe_pass_message), %edx
    jmp 5f
3:
    call 6f
6:
    popl %ecx
    addl $(user_probe_fail_message - 6b), %ecx
    movl $(user_probe_fail_message_end - user_probe_fail_message), %edx
5:
    movl $SYSCALL_STDOUT, %ebx
    movl $1, %eax
    int $0x80

    movl $SYSCALL_WRITE, %eax
    movl $SYSCALL_STDOUT, %ebx
    movl $0x40004000, %ecx
    movl $1, %edx
    int $0x80
    cmpl $-1, %eax
    jne 7f
    call 8f
8:
    popl %ecx
    addl $(user_pointer_pass_message - 8b), %ecx
    movl $(user_pointer_pass_message_end - user_pointer_pass_message), %edx
    jmp 9f
7:
    call 10f
10:
    popl %ecx
    addl $(user_pointer_fail_message - 10b), %ecx
    movl $(user_pointer_fail_message_end - user_pointer_fail_message), %edx
9:
    movl $SYSCALL_STDOUT, %ebx
    movl $1, %eax
    int $0x80

    movl $0xFFFFFFFF, %eax
    int $0x80
    cmpl $-1, %eax
    jne 11f
    call 12f
12:
    popl %ecx
    addl $(user_number_pass_message - 12b), %ecx
    movl $(user_number_pass_message_end - user_number_pass_message), %edx
    jmp 13f
11:
    call 14f
14:
    popl %ecx
    addl $(user_number_fail_message - 14b), %ecx
    movl $(user_number_fail_message_end - user_number_fail_message), %edx
13:
    movl $SYSCALL_STDOUT, %ebx
    movl $1, %eax
    int $0x80

    cmpl $1, 0x40002004
    jne 11f
    ud2
11:
    call 15f
15:
    popl %ecx
    addl $(user_survivor_message - 15b), %ecx
    movl $(user_survivor_message_end - user_survivor_message), %edx
    movl $SYSCALL_STDOUT, %ebx
    movl $SYSCALL_WRITE, %eax
    int $0x80

    movl $0x02000000, %ecx
16:
    decl %ecx
    jnz 16b

    movl $SYSCALL_EXIT, %eax
    int $0x80
    ud2

user_probe_pass_message:
    .ascii "PASS: ring 3 cannot read kernel memory.\n"
user_probe_pass_message_end:
user_probe_fail_message:
    .ascii "FAIL: ring 3 read kernel memory.\n"
user_probe_fail_message_end:
user_pointer_pass_message:
    .ascii "PASS: syscall rejected invalid user pointer.\n"
user_pointer_pass_message_end:
user_pointer_fail_message:
    .ascii "FAIL: syscall accepted invalid user pointer.\n"
user_pointer_fail_message_end:
user_number_pass_message:
    .ascii "PASS: syscall rejected unknown number.\n"
user_number_pass_message_end:
user_number_fail_message:
    .ascii "FAIL: syscall accepted unknown number.\n"
user_number_fail_message_end:
user_survivor_message:
    .ascii "PASS: process 2 survived process 1 fault.\n"
user_survivor_message_end:
user_mode_demo_end:

.section .text
