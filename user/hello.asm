; ============================================================
; SAM OS — user/hello.asm
; Sprint 17: first SAM OS ring-3 process + isolation probe.
;
; Phase 1: print via write(1) syscall.
; Phase 2: read KERNEL memory (0x100000) — MUST #PF under the task's
;          page tables; the kernel dispatcher reports this as the
;          Sprint 17 isolation PASS. Only if isolation is broken does
;          control continue to the failure path.
; ============================================================

BITS 64
org 0x19000000

section .text
start:
    ; ── Phase 1: write(1, msg, msg_len)
    mov rax, 1
    mov rdi, 1
    mov rsi, msg
    mov rdx, msg_len
    int 0x80

    ; ── Phase 2: announce + touch kernel memory
    mov rax, 1
    mov rdi, 1
    mov rsi, probe_msg
    mov rdx, probe_len
    int 0x80

    mov rax, [0x100000]     ; kernel text — must #PF in ring 3

    ; Isolation FAILED if we get here — report and exit nonzero.
    mov rax, 1
    mov rdi, 1
    mov rsi, fail_msg
    mov rdx, fail_len
    int 0x80
    mov rax, 2
    mov rdi, 1
    int 0x80

.halt:
    jmp .halt

section .data
msg:        db "[ring3] Hello from the first SAM OS user process!", 10, \
               "[ring3] write + exit syscalls OK", 10
msg_len     equ $ - msg
probe_msg:  db "[ring3] Phase 2: touching kernel memory at 0x100000...", 10
probe_len   equ $ - probe_msg
fail_msg:   db "[ring3] ISOLATION FAILURE: kernel memory was readable!", 10
fail_len    equ $ - fail_msg
