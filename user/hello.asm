; ============================================================
; SAM OS — user/hello.asm
; Sprint 19: clean-exit task built as a REAL ELF64 executable
; (nasm -f elf64 + ld -Ttext=0x19000000 -e start)
; ============================================================

BITS 64

section .text
global start
start:
    mov rax, 1              ; write(1, msg, msg_len)
    mov rdi, 1
    mov rsi, msg
    mov rdx, msg_len
    int 0x80

    mov rax, 2              ; exit(0)
    mov rdi, 0
    int 0x80

.halt:
    jmp .halt

section .data
msg:    db "[ring3] Hello from an ELF64 SAM OS process!", 10, \
           "[ring3] ELF loader + write + exit syscalls OK", 10
msg_len equ $ - msg
