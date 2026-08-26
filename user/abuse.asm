; ============================================================
; SAM OS — user/abuse.asm
; Sprint 25H: SYS_WRITE boundary/adversarial probes.
; Exits 0 iff every probe returns the exact SAM-native code.
; Any mismatch raises #UD (contained fault) so batch verdicts,
; which count faults, see it loudly. Prints one '.' per pass.
; ============================================================
BITS 64
%define SYS_WRITE 1
%define SYS_EXIT  2

section .text
global start
start:
    ; ---- p1: valid buffer, len 5 -> 5 ----
    mov rdi,1
    mov rsi,msg
    mov rdx,5
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,5
    jne bad
    call dot

    ; ---- p2: zero length -> 0 ----
    mov rdi,1
    xor rsi,rsi
    xor rdx,rdx
    mov rax,SYS_WRITE
    int 0x80
    test rax,rax
    jnz bad
    call dot

    ; ---- p3: kernel address -> -4 ----
    mov rdi,1
    mov rsi,0x100000
    mov rdx,4
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,-4
    jne bad
    call dot

    ; ---- p4: stack boundary crossing -> -4 ----
    mov rdi,1
    mov rsi,0x193FFFFE
    mov rdx,4
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,-4
    jne bad
    call dot

    ; ---- p5: address+length overflow -> -4 ----
    mov rdi,1
    mov rsi,-16
    mov rdx,32
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,-4
    jne bad
    call dot

    ; ---- p6: huge length, valid buffer -> -8 ----
    mov rdi,1
    mov rsi,msg
    mov rdx,0x20000000
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,-8
    jne bad
    call dot

    ; ---- p7: supervisor hole -> -4 ----
    mov rdi,1
    mov rsi,0x18000000
    mov rdx,8
    mov rax,SYS_WRITE
    int 0x80
    cmp rax,-4
    jne bad
    call dot

    mov rdi,okmsg
    call puts
    mov rdi,0
    mov rax,SYS_EXIT
    int 0x80

bad:
    ud2

; dot(): write(".",1)
dot:
    push rdi
    push rsi
    push rdx
    push rax
    mov rdi,1
    mov rsi,dotch
    mov rdx,1
    mov rax,SYS_WRITE
    int 0x80
    pop rax
    pop rdx
    pop rsi
    pop rdi
    ret

; puts(rdi = asciz)
puts:
    test rdi,rdi
    jz .d
    mov rsi,rdi
    xor rdx,rdx
.l:
    cmp byte [rsi+rdx],0
    je .g
    inc rdx
    jmp .l
.g:
    mov rax,SYS_WRITE
    mov rdi,1
    int 0x80
.d:
    ret

section .data
msg:     db "ABUSE",10
dotch:   db "."
okmsg:   db "[A] boundary probes OK",10
