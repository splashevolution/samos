; ============================================================
; SAM OS — user/hello.asm
; Sprint 16: the FIRST SAM OS ring-3 process.
;
; Flat binary linked at USER_CODE_BASE (0x40000000). The kernel copies
; it there from the initrd and enters ring 3 at its first byte.
;
; Syscall ABI (Sprint 16, frozen):
;   rax = syscall number | rdi = arg0 | rsi = arg1 | rdx = arg2
;   1 = write(fd=rdi, buf=rsi, len=rdx)
;   2 = exit(code=rdi)
;
; Build: nasm -f bin -o hello.bin hello.asm   (org must match kernel copy addr)
; ============================================================

BITS 64
org 0x19000000

section .text
start:
    ; write(1, msg, msg_len)
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, msg_len
    int 0x80

    ; exit(0)
    mov rax, 2
    mov rdi, 0
    int 0x80

    ; If exit ever returns, spin — the kernel should never let this run
.halt:
    jmp .halt

section .data
msg:    db "[ring3] Hello from the first SAM OS user process!", 10, \
           "[ring3] write + exit syscalls OK", 10
msg_len equ $ - msg
