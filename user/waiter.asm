; ============================================================
; SAM OS — user/waiter.asm
; Sprint 25: ring-3 parent for waitpid lifecycle tests.
;
; argv contract (built by the kernel as real decimal strings):
;   argv[1] mode : 'S' specific-child wait
;                  'A' wait-any (-1)
;                  'E' invalid-wait error probes
;   argv[2] target pid decimal            (S)
;   argv[3] spin iteration count          (busy-wait before waiting;
;                                          large => child zombies first)
;   argv[4] expected reaped pid           (S,A)
;   argv[5] expected reason 0=exit 1=fault(S,A; implied code 0 / 14)
;
; Prints identity evidence ([W] pid= ppid=), blocks inside waitpid when
; the child has not terminated, validates returned PID + 64-bit status,
; exits 0 on success / 42 on any mismatch.
;
; SAM-native waitpid errors used by mode E:
;   WPID_E_BADPID=-1  WPID_E_NOTCHILD=-2  WPID_E_BADOPTS=-5
; ============================================================

BITS 64

%define SYS_WRITE   1
%define SYS_EXIT    2
%define SYS_WAITPID 4
%define SYS_GETPID  5
%define SYS_GETPPID 6

section .text
global start
start:
    lea r12, [rsp+8]            ; argv[] base

    ; ---- identity evidence ----
    mov rax, SYS_GETPID
    int 0x80
    mov r14, rax                ; my pid
    mov rax, SYS_GETPPID
    int 0x80
    mov r15, rax                ; my ppid

    mov rdi, s_wmsg
    call puts
    mov rdi, s_pidlbl
    call puts
    mov rax, r14
    call putdec
    mov rdi, s_pplbl
    call puts
    mov rax, r15
    call putdec
    call putnl

    ; ---- self-identity: pid != 0 ; ppid == 0 (kernel-parented) ----
    test r14, r14
    jz  fail42
    test r15, r15
    jnz fail42

    ; ---- mode char from argv[1] ----
    xor ebx, ebx
    mov rsi, [r12+8]
    or  rsi, rsi
    jz  fail42
    mov bl, [rsi]

    ; ---- spin phase: argv[3] iterations (0 = skip) ----
    mov rsi, [r12+24]
    call parse_dec
    mov rcx, rax
    test rcx, rcx
    jz .spundone
.spin:
    dec rcx
    jnz .spin
.spundone:

    cmp bl, 'E'
    je  mode_e
    cmp bl, 'A'
    je  wait_any
    cmp bl, 'S'
    jne fail42

    ; ---- specific target from argv[2] ----
    mov rsi, [r12+16]
    call parse_dec
    mov rdi, rax
    jmp do_wait

wait_any:
    mov rdi, -1

do_wait:
    mov rsi, stbuf
    xor rdx, rdx                ; options == 0
    mov rax, SYS_WAITPID
    int 0x80
    mov r10, rax                ; returned child pid

    ; ---- validate returned pid against argv[4] ("0" = accept any) ----
    mov rsi, [r12+32]
    call parse_dec
    test rax, rax
    jz  .pid_any                ; expected 0: any nonzero child accepted
    cmp r10, rax
    jne report_fail
.pid_any:
    test r10, r10
    jz report_fail

    ; ---- validate status word vs argv[5] reason (code implied) ----
    lea r9, [rel stbuf]
    mov r9, [r9]
    mov rax, r9
    shr rax, 32                 ; observed reason
    mov rsi, [r12+40]
    call parse_dec              ; expected reason
    cmp rax, 0
    jne .exp_fault
    ; expect EXIT: code must be 0
    mov eax, r9d                ; low 32 bits (zero-extend)
    test eax, eax
    jnz report_fail
    jmp wait_ok
.exp_fault:
    ; expect FAULT: reason 1, code == vector 14
    cmp rax, 1
    jne report_fail
    mov eax, r9d
    cmp eax, 14
    jne report_fail

wait_ok:
    mov rdi, s_okmsg
    call puts
    mov rdi, s_gotlbl
    call puts
    mov rax, r10
    call putdec
    mov rdi, s_stlbl
    call puts
    mov rax, r9
    call puthex
    call putnl
    mov rdi, 0
    mov rax, SYS_EXIT
    int 0x80

report_fail:
    mov rdi, s_badmsg
    call puts
    mov rdi, s_gotlbl
    call puts
    mov rax, r10
    call putdec
    mov rdi, s_stlbl
    call puts
    lea rax, [rel stbuf] ; mov rax, [rax]
    call puthex
    call putnl
    jmp fail42

; ---------------- mode E: invalid-wait probes ----------------
mode_e:
    ; (a) waitpid(getpid(), buf, 0) -> WPID_E_NOTCHILD (-2)
    mov rdi, r14
    mov rsi, stbuf
    xor rdx, rdx
    mov rax, SYS_WAITPID
    int 0x80
    cmp rax, -2
    jne e_fail
    mov rdi, s_e1
    call puts

    ; (b) waitpid(1, buf, options=1) -> WPID_E_BADOPTS (-5)
    mov rdi, 1
    mov rsi, stbuf
    mov rdx, 1
    mov rax, SYS_WAITPID
    int 0x80
    cmp rax, -5
    jne e_fail
    mov rdi, s_e2
    call puts

    ; (c) waitpid(-7, buf, 0) -> WPID_E_BADPID (-1)
    mov rdi, -7
    mov rsi, stbuf
    xor rdx, rdx
    mov rax, SYS_WAITPID
    int 0x80
    cmp rax, -1
    jne e_fail
    mov rdi, s_e3
    call puts

    mov rdi, s_eok
    call puts
    mov rdi, 0
    mov rax, SYS_EXIT
    int 0x80

e_fail:
    mov rdi, s_efail
    call puts
    mov rax, rax                ; (no-op) keep rax visible in trace
    mov rdi, s_gotlbl
    call puts
    lea rax, [rel stbuf] ; mov rax, [rax]
    call puthex
    call putnl
    jmp fail42

fail42:
    mov rdi, 42
    mov rax, SYS_EXIT
    int 0x80
.halt:
    jmp .halt

; ---------------- helpers ----------------
; putnl()
putnl:
    mov rdi, s_nl
    ; fall through to puts
; puts(rdi = asciz)
puts:
    test rdi, rdi
    jz .pdone
    mov rsi, rdi
    xor rdx, rdx
.plen:
    cmp byte [rsi+rdx], 0
    je .pgo
    inc rdx
    jmp .plen
.pgo:
    test rdx, rdx
    jz .pdone
    mov rax, SYS_WRITE
    mov rdi, 1
    int 0x80
.pdone:
    ret

; putdec(rax = u64)
putdec:
    sub rsp, 32
    lea rsi, [rsp+31]
    mov byte [rsi], 0
    mov rbx, 10
.pdloop:
    xor rdx, rdx
    div rbx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test rax, rax
    jnz .pdloop
    mov rdi, rsi
    call puts
    add rsp, 32
    ret

; puthex(rax = u64) — fixed 16 digits
puthex:
    sub rsp, 40
    lea rsi, [rsp+39]
    mov byte [rsi], 0
    mov rcx, 16
.phloop:
    mov rdx, rax
    and rdx, 0xF
    movzx rdx, byte [hexd+rdx]
    dec rsi
    mov [rsi], dl
    shr rax, 4
    dec rcx
    jnz .phloop
    mov rdi, rsi
    call puts
    add rsp, 40
    ret

; parse_dec(rsi = asciz) -> rax
parse_dec:
    xor rax, rax
.pd2:
    movzx edx, byte [rsi]
    cmp dl, '0'
    jb  .pdone2
    cmp dl, '9'
    ja  .pdone2
    imul rax, rax, 10
    sub dl, '0'
    add rax, rdx
    inc rsi
    jmp .pd2
.pdone2:
    ret

section .data
hexd:     db '0123456789ABCDEF'
s_wmsg:   db "[W] ", 0
s_pidlbl: db "pid=", 0
s_pplbl:  db " ppid=", 0
s_nl:     db 10, 0
s_okmsg:  db "[W] wait OK", 0
s_badmsg: db "[W] WAIT MISMATCH", 0
s_gotlbl: db " got=", 0
s_stlbl:  db " status=0x", 0
s_e1:     db "[W] probe not-child -> -2 OK", 10, 0
s_e2:     db "[W] probe bad-options -> -5 OK", 10, 0
s_e3:     db "[W] probe bad-pid -> -1 OK", 10, 0
s_eok:    db "[W] error probes passed", 10, 0
s_efail:  db "[W] ERROR-PROBE MISMATCH", 10, 0

section .bss
alignb 8
stbuf:  resq 1                  ; 8-byte-aligned wait status buffer
