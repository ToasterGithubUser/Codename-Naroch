.section .text
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

    # Переключаем контекст только после IRQ0 и только после инициализации планировщика.
    .extern current_task
    .extern task_switch

    cmpl $32, 36(%esp)
    jne 1f
    movl current_task, %eax
    testl %eax, %eax
    jz 1f
    call task_switch
    movl current_task, %eax
    movl 4(%eax), %esp  # Принудительно подменяем стек процессора на стек новой задачи!

1:
    pop %eax        # Восстанавливаем оригинальные сегменты данных новой задачи
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    popa            # Восстанавливаем регистры общего назначения новой задачи
    add $8, %esp    # Очищаем фиктивные числа прерывания
    iret            # ВЗЛЕТАЕМ в контекст выполнения новой задачи!
