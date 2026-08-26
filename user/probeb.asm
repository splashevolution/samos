; ============================================================
; SAM OS — user/probeb.asm
; Sprint 24: cross-process physical-backing isolation probe.
; Linked at the canonical USER_CODE_VA. Deliberately reads
; 0x19600000 — the PHYSICAL stack backing owned by slot 1.
; Under the shared-VA model this address exists in THIS task's
; address space only as a supervisor-only identity mapping, so
; ring-3 access must #PF. The kernel must survive and another
; runnable task must continue.
; ============================================================

BITS 64

section .text
global start
start:
    ; write(1, msg, msg_len)
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, msg_len
    int 0x80

    ; Ring-3 read of another task's physical backing → must fault (#PF)
    mov al, [abs 0x19600000]

    ; Unreachable if isolation holds; otherwise report via exit(7)
    mov rax, 2
    mov rdi, 7
    int 0x80
.halt:
    jmp .halt

section .data
msg:    db "[PB] probing slot-1 stack PA 0x19600000 from ring 3", 10
msg_len equ $ - msg
