; ============================================================
; SAM OS — user/echo.asm
; Sprint 22: argv + exit codes.
;
; Entry state (kernel-built stack): [rsp]=argc, [rsp+8]=argv[].
; Prints every argument, then exit(argc) — the shell/kernel can
; verify the code came back end to end.
; ============================================================

BITS 64

section .text
global start
start:
    mov r15, [rsp]          ; argc
    lea r14, [rsp + 8]      ; argv = &argv[0] (array lives just above argc)

    ; write(1, "[echo] argc=", 12)
    mov rax, 1
    mov rdi, 1
    mov rsi, hdr
    mov rdx, hdr_len
    int 0x80

    ; print argc as single digit (argc <= 9 by ABI limit)
    mov rax, 1
    mov rdi, 1
    mov rsi, digs
    add rsi, r15
    mov rdx, 1
    int 0x80

    ; newline
    mov rax, 1
    mov rdi, 1
    mov rsi, nl
    mov rdx, 1
    int 0x80

    ; for i in 0..argc-1: write(argv[i]); write(" ")
    xor rbx, rbx
.argloop:
    cmp rbx, r15
    jge .argdone

    mov rax, 1
    mov rdi, 1
    mov rsi, [r14 + rbx * 8]
    xor rdx, rdx
.strlen:
    cmp byte [rsi + rdx], 0
    je .strlen_done
    inc rdx
    jmp .strlen
.strlen_done:
    int 0x80

    mov rax, 1
    mov rdi, 1
    mov rsi, sp_ch
    mov rdx, 1
    int 0x80

    inc rbx
    jmp .argloop

.argdone:
    mov rax, 1
    mov rdi, 1
    mov rsi, nl
    mov rdx, 1
    int 0x80

    ; exit(argc)
    mov rax, 2
    mov rdi, r15
    int 0x80
.halt:
    jmp .halt

section .data
hdr:     db '[echo] argc='
digs:    db '0123456789'
sp_ch:   db ' '
nl:      db 10
hdr_len  equ 12
