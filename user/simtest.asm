; ============================================================
; SAM OS — user/simtest.asm
; Sprint 25H: FPU/SIMD context-preservation proof under preemption.
; argv[1]: 'A' or 'B' — distinct XMM0 patterns.
; Loop 5x: load pattern into XMM0 -> busy delay -> verify XMM0 intact.
; If the scheduler failed to preserve SIMD state across a PIT
; preemption, the verify step observes the other task's pattern
; or zeros and exits 42. All 5 iterations intact -> exit 0.
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

    lea r13,[rel patA]
    cmp bl,'A'
    je .havepat
    lea r13,[rel patB]
.havepat:
    ; stamp identity marker into output buffer for serial evidence
    mov r14,5                 ; iterations
.iter:
    movdqu xmm0,[r13]         ; load unique pattern
    mov rcx,2000000           ; busy delay long enough to be preempted
.delay:
    dec rcx
    jnz .delay

    ; ---- verify xmm0 still holds OUR pattern ----
    lea rbx,[rel spill]
    movdqu [rbx],xmm0
    mov rax,[rbx]
    cmp rax,[r13]
    jne report_fail_sim
    mov rax,[rbx+8]
    cmp rax,[r13+8]
    jne report_fail_sim

    dec r14
    jnz .iter

    ; print "[S]<mode> ok"
    mov rdi,s_ok
    mov byte [abs s_ok+3],bl
    call puts
    mov rdi,0
    mov rax,SYS_EXIT
    int 0x80

report_fail_sim:
    ; ── 25H deep diagnostic ──
    sub rsp,64                     ; room: spill16 + gp scratch
    mov [rsp+32],r13               ; stash bases
    mov [rsp+40],rbx
    mov [rsp+48],rcx               ; remaining delay count
    mov [rsp+56],r14               ; round counter
    lea rbx,[rel spill]
    movdqu [rbx],xmm0
    mov rdi,s_live
    call puts
    mov rax,[rbx]
    call phex
    mov rax,[rbx+8]
    call phex
    mov rdi,s_expe
    call puts
    mov rax,[r13]
    call phex
    mov rax,[r13+8]
    call phex
    mov rdi,r_spare                ; " | "
    call puts
    mov rdi,s_r13
    call puts
    mov rax,[rsp+32]
    call phex
    mov rdi,s_rbx
    call puts
    mov rax,[rsp+40]
    call phex
    mov rdi,s_rcx
    call puts
    mov rax,[rsp+48]
    call phex
    mov rdi,s_r14
    call puts
    mov rax,[rsp+56]
    call phex
    lea rdi,[rel nl2]
    mov rsi,rdi
    mov rdx,1
    call putsp
    ud2
    ; Signal SIMD corruption as a CONTAINED fault (#UD) rather than an
    ; exit code — batch verdicts count faults, making corruption loud.
    ud2
report_bad:
    mov rdi,s_bad
    call puts
    mov rdi,42
    mov rax,SYS_EXIT
    int 0x80

badmode:
    jmp report_bad

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

section .data
patA: dq 0AAAAAAAAAAAAAAAAh, 0AAAAAAAAAAAAAAAAh
patB: dq 0BBBBBBBBBBBBBBBBh, 0BBBBBBBBBBBBBBBBh
s_ok:  db "[S]x ok",10,0
s_bad: db "[S] SIMD STATE CORRUPTED",10,0

section .bss
alignb 16
spill: resq 2

section .data
s_live: db "[S] live=",0
s_expe: db " exp=",0
r_spare: db " |",0
s_r13:  db " r13=",0
s_rbx:  db " rbx=",0
s_rcx:  db " rcx=",0
s_r14:  db " r14=",0
hexd2:  db "0123456789ABCDEF"
nl2:    db 10

section .text
phex:
    sub rsp,40
    lea rsi,[rsp+39]
    mov byte [rsi],0
    mov rcx,16
.ph:
    mov rdx,rax
    and rdx,0xF
    lea rdi,[rel hexd2]
    movzx edx,byte [rdi+rdx]
    dec rsi
    mov [rsi],dl
    shr rax,4
    dec rcx
    jnz .ph
    mov rdi,rsi
    xor rdx,rdx
.dlen:
    cmp byte [rsi+rdx],0
    je .emit
    inc rdx
    jmp .dlen
.emit:
    test rdx,rdx
    jz .done
    mov rdi,1
    mov rax,SYS_WRITE
    int 0x80
.done:
    add rsp,40
    ret

putsp:
    mov rax,SYS_WRITE
    int 0x80
    ret
