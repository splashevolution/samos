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
global _user_halt
global sam_user_resume
global sam_kernel_rsp
extern sam_kernel_rbp
extern sam_kernel_ret
extern sam_kernel_cr3

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
    ; Resume exactly where the kernel thread left off. The return address
    ; was stashed in sam_kernel_ret at enter time — the kernel stack below
    ; the saved frame is NOT trusted (it gets trampled while user runs).
    mov rsp, [rel sam_kernel_rsp]
    mov rbp, [rel sam_kernel_rbp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    popfq
    ; Consume the return-address slot pushed by the original `call`
    ; sam_user_enter — we re-enter mid-function via jmp, not via ret.
    add rsp, 8
    jmp [rel sam_kernel_ret]

_user_halt:
    ; Sprint 17 exit semantics: single-task system — the demo task is done,
    ; so the kernel parks with interrupts disabled. Clean and verifiable.
    cli
.halt:
    hlt
    jmp .halt

; ============================================================
; sam_user_resume(rdi = &saved cpu_frame, rsi = task CR3, rdx = &fpu ctx)
; Sprint 20/25H: restore a preempted ring-3 task — FPU/SIMD state first,
; then CR3, then the full integer frame, then iretq. The extended-state
; load is the FIRST instruction after entry: nothing (C or otherwise) can
; run between fxrstor and iretq, closing every scheduling window.
;
; cpu_frame_t layout (quad offsets):
;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
;   vector error rip cs rflags rsp ss
; ============================================================
extern g_use_xsave
sam_user_resume:
    mov rcx, rdx                ; fpu context buffer
    cmp byte [rel g_use_xsave], 0
    jne .fx
    fxrstor [rcx]
    jmp .cr3
.fx:
    mov eax, 0xFFFFFFFF
    mov edx, 0xFFFFFFFF
    xrstor [rcx]
.cr3:
    mov rcx, rdi                ; frame base
    mov rax, rsi
    mov cr3, rax                ; switch into the task address space

    ; Build the iretq frame first (uses rcx as base)
    push qword [rcx + 168]      ; SS
    push qword [rcx + 160]      ; RSP
    push qword [rcx + 152]      ; RFLAGS
    push qword [rcx + 144]      ; CS
    push qword [rcx + 136]      ; RIP

    ; Restore GP registers
    mov r15, [rcx]
    mov r14, [rcx + 8]
    mov r13, [rcx + 16]
    mov r12, [rcx + 24]
    mov r11, [rcx + 32]
    mov r10, [rcx + 40]
    mov r9,  [rcx + 48]
    mov r8,  [rcx + 56]
    mov rbp, [rcx + 64]
    mov rbx, [rcx + 104]
    mov rax, [rcx + 112]
    mov rsi, [rcx + 80]
    mov rdx, [rcx + 88]
    mov rdi, [rcx + 72]
    mov rcx, [rcx + 96]         ; last: rcx itself
    iretq
