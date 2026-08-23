; ============================================================
; SAM OS — userasm.asm
; Sprint 16 / Phase 4: ring-3 entry/exit trampolines
;
; _user_enter(rdi=entry RIP, rsi=user RSP):
;   Saves callee-saved regs + RFLAGS on the kernel stack, records the
;   stack pointer in sam_kernel_rsp, then iretqs into ring 3.
;
; _user_exit():
;   Called from the SYS_EXIT syscall handler. Restores the saved kernel
;   stack and returns into sam_user_enter's caller. Never returns to the
;   interrupted user task — the task is gone by definition of exit().
; ============================================================

global _user_enter
global _user_exit
global sam_kernel_rsp

section .data
sam_kernel_rsp: dq 0

section .text
bits 64

_user_enter:
    ; Preserve kernel-thread state across the user-mode excursion
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Remember where the kernel stack is (consumed by _user_exit)
    mov [rel sam_kernel_rsp], rsp

    ; Build a fake interrupt frame and switch to ring 3:
    ;   SS = 0x23 (user data, RPL=3)
    ;   RSP = user stack top
    ;   RFLAGS = 0x202 (IF=1 + reserved bit)
    ;   CS = 0x1B (user code, RPL=3)
    ;   RIP = entry point
    push qword 0x23
    push rsi
    push qword 0x202
    push qword 0x1B
    push rdi
    iretq

_user_exit:
    ; Resume exactly where the kernel thread left off.
    mov rsp, [rel sam_kernel_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    xor eax, eax
    ret
