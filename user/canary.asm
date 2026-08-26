; ============================================================
; SAM OS — user/canary.asm
; Sprint 25H: sequential data-remanence probe.
;   mode 'W': plant unique canaries at fixed canonical VAs
;             spanning code-region scratch and both stack halves,
;             then exit(0).
;   mode 'R': read the same sites. ANY nonzero byte observed
;             before this process writes them = remanence leak.
; Exits 0 clean / 42 on detected leakage or W failure.
; ============================================================
BITS 64
%define SYS_WRITE 1
%define SYS_EXIT  2

%define SITE1 0x19001000        ; code region scratch
%define SITE2 0x19010000        ; code region far
%define SITE3 0x19200100        ; stack low
%define SITE4 0x193FFF00        ; stack high (below argv area)
%define CANARY 0xC0DEC0DEC0DEC0DE

section .text
global start
start:
    lea r12,[rsp+8]
    mov rsi,[r12+8]
    or rsi,rsi
    jz badmode
    mov bl,[rsi]

    cmp bl,'W'
    je mode_w
    cmp bl,'R'
    je mode_r
badmode:
    jmp fail

mode_w:
    mov rax,CANARY
    mov [abs SITE1],rax
    mov [abs SITE2],rax
    mov [abs SITE3],rax
    mov [abs SITE4],rax
    ; 64-byte spray across stack mid
    mov rdi,0x19280000
    mov rcx,8
.spray:
    mov [rdi],rax
    add rdi,8
    dec rcx
    jnz .spray
    mov rdi,0
    mov rax,SYS_EXIT
    int 0x80

mode_r:
    mov rax,[abs SITE1]
    test rax,rax
    jnz leaked
    mov rax,[abs SITE2]
    test rax,rax
    jnz leaked
    mov rax,[abs SITE3]
    test rax,rax
    jnz leaked
    mov rax,[abs SITE4]
    test rax,rax
    jnz leaked
    mov rdi,0x19280000
    mov rcx,8
.chk:
    mov rax,[rdi]
    test rax,rax
    jnz leaked
    add rdi,8
    dec rcx
    jnz .chk

    mov rdi,s_clean
    call puts
    mov rdi,0
    mov rax,SYS_EXIT
    int 0x80

leaked:
    ud2
fail:
    mov rdi,42
    mov rax,SYS_EXIT
    int 0x80

puts:
    test rdi,rdi
    jz .d
    mov rsi,rdi
    xor rdx,rdx
.l: cmp byte [rsi+rdx],0
    je .g
    inc rdx
    jmp .l
.g: mov rax,SYS_WRITE
    mov rdi,1
    int 0x80
.d: ret

section .data
s_clean: db "[C] backing clean",10,0
s_leak:  db "[C] REMANENCE LEAK DETECTED",10,0
