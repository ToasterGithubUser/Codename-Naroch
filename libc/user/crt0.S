.section .text.start,"ax"
.global _start
.type _start, @function
.extern main
.extern exit
.extern __libc_init_array

_start:
    xorl %ebp, %ebp
    movl %esp, %ebp
    andl $-16, %esp
    call __libc_init_array
    movl (%ebp), %eax
    leal 4(%ebp), %ecx
    leal 4(%ecx,%eax,4), %edx
    subl $4, %esp
    pushl %edx
    pushl %ecx
    pushl %eax
    call main
    addl $16, %esp
    subl $12, %esp
    pushl %eax
    call exit
    ud2
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
