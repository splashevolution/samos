; ============================================================
; SAM OS — user/loops.asm
; Sprint 24: CANONICAL single instance binary for the shared
; virtual-address model. Linked once at USER_CODE_BASE; any
; number of simultaneous instances run via distinct CR3s.
; Identifies itself by argv[1]: prints "<tag><n>" for n=1..5,
; so four resident instances produce distinguishable output.
;
; ABI (Sprint 22): [rsp]=argc, [rsp+8]=argv[0], [rsp+16]=argv[1]
; ============================================================

BITS 64

section .text
global start
start:
    mov r15, 1                  ; iteration counter 1..5
    mov r13, [rsp]              ; argc
    mov r14, qmark              ; default tag "?"
    cmp r13, 2
    jl .tagok
    mov r14, [rsp+16]           ; argv[1]
.tagok:

.loop:
    ; write(1, tag, strlen(tag))
    mov rsi, r14
    xor rdx, rdx
.tlen:
    cmp byte [rsi+rdx], 0
    je .tdone
    inc rdx
    jmp .tlen
.tdone:
    mov rax, 1
    mov rdi, 1
    int 0x80                    ; regs preserved across syscall (frame restore)

    ; write(1, &digit, 1)
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

    ; busy-wait ~80 ms so PIT preemption fires mid-task
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
digs:     db '0123456789'
nl:       db 10
qmark:    db '?'
