; ============================================================
; SAM OS — user/guard.asm
; Sprint 17 isolation guard: reads KERNEL memory (0x100000).
; Under per-task page tables this MUST #PF; the kernel dispatcher
; records TASK_END_FAULT and resumes — proving isolation.
; ============================================================

BITS 64

section .text
global start
start:
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, msg_len
    int 0x80

    mov rax, [0x100000]     ; kernel text — must #PF in ring 3

    mov rax, 2
    mov rdi, 1              ; exit(1) = failure
    int 0x80

.halt:
    jmp .halt

section .data
msg:    db "[ring3] guard: probing kernel memory at 0x100000...", 10
msg_len equ $ - msg
