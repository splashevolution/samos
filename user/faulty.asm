; ============================================================
; SAM OS — user/faulty.asm
; Sprint 25H: deliberate CPL3 faults for containment tests.
; argv[1]: 'D' divide-by-zero (#DE, vector 0)
;          'U' ud2            (#UD, vector 6)
;          'G' lgdt           (#GP, vector 13)
; Each prints [F]<mode> first. If execution somehow survives,
; exits 43 so the test fails loudly instead of hanging.
; ============================================================
BITS 64
%define SYS_WRITE 1
%define SYS_EXIT  2

section .text
global start
start:
    lea r12,[rsp+8]
    mov rsi,[r12+8]
    or rsi,rsi
    jz badmode
    mov bl,[rsi]

    ; marker: write("[F]<mode>",9)
    mov rdi,s_msg
    mov byte [abs s_msg+3],bl
    call puts

    cmp bl,'D'
    je do_de
    cmp bl,'U'
    je do_ud
    cmp bl,'G'
    je do_gp
badmode:
    jmp survive

do_de:
    xor ecx,ecx
    mov eax,1
    div ecx                   ; #DE (vector 0)
    jmp survive

do_ud:
    db 0x0F,0x0B              ; ud2 -> #UD (vector 6)
    jmp survive

do_gp:
    lgdt [rel gdummy]         ; privileged in ring 3 -> #GP(13)
    jmp survive

survive:
    mov rdi,43
    mov rax,SYS_EXIT
    int 0x80

; puts(rdi)
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

section .bss
gdummy: resw 1
        resq 1

section .data
s_msg: db "[F]x",10,0
