; ============================================================
; SAM OS — user/loop.asm
; Sprint 20: preemption demo. Prints [lp]<n> for n=1..5 with a
; busy-wait gap between prints so the PIT (100 Hz) preempts the task
; several times; the kernel resumes it each tick and the task cannot
; tell the difference — it finishes cleanly.
; ============================================================

BITS 64

section .text
global start
start:
    mov r15, 1              ; iteration counter 1..5
.loop:
    ; write(1, "[lp] ", 5)
    mov rax, 1
    mov rdi, 1
    mov rsi, pre
    mov rdx, pre_len
    int 0x80

    ; write(1, &digit_char, 1)
    mov rax, 1
    mov rdi, 1
    lea rsi, [digs]
    add rsi, r15
    mov rdx, 1
    int 0x80

    ; write(1, "\n", 1)
    mov rax, 1
    mov rdi, 1
    mov rsi, nl
    mov rdx, 1
    int 0x80

    ; busy-wait ~80 ms so PIT ticks fire mid-task
    mov rcx, 4000000
.wait:
    dec rcx
    jnz .wait

    inc r15
    cmp r15, 5
    jle .loop

    ; exit(0)
    mov rax, 2
    mov rdi, 0
    int 0x80
.halt:
    jmp .halt

section .data
pre:      db '[lp] '
digs:     db '0123456789'
nl:       db 10
pre_len   equ $ - pre - 12   ; 5 bytes ("[lp] ")
