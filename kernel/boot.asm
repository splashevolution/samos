; ============================================================
; SAM OS — boot.asm
; Multiboot2 header → protected mode → GDT → 64-bit long mode
; Calls kernel_main(multiboot_magic, multiboot_info_ptr)
; ============================================================
; Build:  nasm -f elf64 boot.asm -o boot.o
; ============================================================

global _start
extern kernel_main

; ── Multiboot2 constants ─────────────────────────────────────
MULTIBOOT2_MAGIC    equ 0xE85250D6
MULTIBOOT_ARCH_I386 equ 0
HEADER_LENGTH       equ (multiboot_header_end - multiboot_header)
CHECKSUM            equ -(MULTIBOOT2_MAGIC + MULTIBOOT_ARCH_I386 + HEADER_LENGTH)

; ── Page / stack sizes ───────────────────────────────────────
PAGE_SIZE   equ 0x1000          ; 4 KiB
STACK_SIZE  equ 0x4000          ; 16 KiB

; ── GDT selectors ────────────────────────────────────────────
GDT_NULL    equ 0x00
GDT_CODE64  equ 0x08
GDT_DATA64  equ 0x10

; ── CR0 / CR4 / EFER flags ───────────────────────────────────
CR0_PE      equ (1 << 0)        ; Protection Enable
CR0_PG      equ (1 << 31)       ; Paging
CR4_PAE     equ (1 << 5)        ; Physical Address Extension
EFER_MSR    equ 0xC0000080
EFER_LME    equ (1 << 8)        ; Long Mode Enable

; ============================================================
; Section: .multiboot2  (must be in first 32 KB of image)
; ============================================================
section .multiboot2 progbits alloc noexec nowrite align=8

multiboot_header:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT_ARCH_I386
    dd HEADER_LENGTH
    dd CHECKSUM

    ; Request a pixel framebuffer for the first-boot GUI.
    ; Multiboot2 framebuffer request tag:
    ;   type=5, flags=0, size=20, width=1024, height=768, depth=32
    ; The explicit padding keeps the following end tag 8-byte aligned.
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    dd 0            ; padding to 8-byte alignment

    ; ── End tag (required) ──────────────────────────────────
    dw 0            ; type  = 0
    dw 0            ; flags = 0
    dd 8            ; size  = 8
multiboot_header_end:

; ============================================================
; Section: .bss  (zero-initialised data — stack + page tables)
; ============================================================
section .bss nobits alloc noexec write align=4096

; Identity-map page tables (PML4 → PDPT → 4× PD covers 0–4 GiB)
; 4 GiB needed so kernel can reach VBoxVGA framebuffer at 0xE0000000
pml4_table: resb PAGE_SIZE
pdpt_table: resb PAGE_SIZE
pd_table0:  resb PAGE_SIZE   ; covers 0x00000000 – 0x3FFFFFFF  (GiB 0)
pd_table1:  resb PAGE_SIZE   ; covers 0x40000000 – 0x7FFFFFFF  (GiB 1)
pd_table2:  resb PAGE_SIZE   ; covers 0x80000000 – 0xBFFFFFFF  (GiB 2)
pd_table3:  resb PAGE_SIZE   ; covers 0xC0000000 – 0xFFFFFFFF  (GiB 3, has 0xE0000000)

; Boot stack (grows downward)
stack_bottom:
    resb STACK_SIZE
stack_top:

; ============================================================
; Section: .text  (32-bit entry — GRUB drops us here)
; ============================================================
section .text
bits 32

_start:
    ; Save multiboot info before we clobber registers
    mov edi, eax        ; multiboot2 magic  → edi (arg0 for kernel_main)
    mov esi, ebx        ; multiboot info ptr → esi (arg1 for kernel_main)

    ; Set up boot stack
    mov esp, stack_top

    ; ── 1. Check for CPUID support (toggle bit 21 of EFLAGS) ─
    pushfd
    pop  eax
    mov  ecx, eax
    xor  eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop  eax
    push ecx
    popfd
    cmp  eax, ecx
    je   .no_cpuid          ; bit didn't toggle → no CPUID

    ; ── 2. Check for Long Mode (CPUID leaf 0x80000001) ───────
    mov  eax, 0x80000000
    cpuid
    cmp  eax, 0x80000001
    jb   .no_long_mode

    mov  eax, 0x80000001
    cpuid
    test edx, (1 << 29)     ; LM bit
    jz   .no_long_mode

    ; ── 3. Build identity-map page tables (all 4 GiB) ────────
    ;   PML4[0]   → pdpt_table
    ;   PDPT[0..3]→ pd_table0..3   (each covers 1 GiB)
    ;   Each PD[0..511] → 2 MiB huge pages
    ;   Required: VBoxVGA framebuffer lives at 0xE0000000 (GiB 3)
    mov  eax, pdpt_table
    or   eax, 0x03
    mov  [pml4_table], eax

    ; Wire all 4 PDPT entries
    mov  eax, pd_table0
    or   eax, 0x03
    mov  [pdpt_table + 0 * 8], eax

    mov  eax, pd_table1
    or   eax, 0x03
    mov  [pdpt_table + 1 * 8], eax

    mov  eax, pd_table2
    or   eax, 0x03
    mov  [pdpt_table + 2 * 8], eax

    mov  eax, pd_table3
    or   eax, 0x03
    mov  [pdpt_table + 3 * 8], eax

    ; Fill pd_table0: physical 0x00000000 – 0x3FFFFFFF (GiB 0)
    mov  ebx, 0x00000000
    mov  ecx, 0
.map_pd0:
    mov  eax, ebx
    or   eax, 0x83          ; present + writable + huge
    mov  [pd_table0 + ecx * 8], eax
    add  ebx, 0x200000
    inc  ecx
    cmp  ecx, 512
    jne  .map_pd0

    ; Fill pd_table1: physical 0x40000000 – 0x7FFFFFFF (GiB 1)
    ; ebx = running physical base, step 0x200000 each iteration
    mov  ebx, 0x40000000    ; start of GiB 1
    mov  ecx, 0
.map_pd1:
    mov  eax, ebx
    or   eax, 0x83          ; present + writable + huge
    mov  [pd_table1 + ecx * 8], eax
    add  ebx, 0x200000
    inc  ecx
    cmp  ecx, 512
    jne  .map_pd1

    ; Fill pd_table2: physical 0x80000000 – 0xBFFFFFFF (GiB 2)
    mov  ebx, 0x80000000
    mov  ecx, 0
.map_pd2:
    mov  eax, ebx
    or   eax, 0x83
    mov  [pd_table2 + ecx * 8], eax
    add  ebx, 0x200000
    inc  ecx
    cmp  ecx, 512
    jne  .map_pd2

    ; Fill pd_table3: physical 0xC0000000 – 0xFFFFFFFF (GiB 3, has 0xE0000000)
    mov  ebx, 0xC0000000
    mov  ecx, 0
.map_pd3:
    mov  eax, ebx
    or   eax, 0x83
    mov  [pd_table3 + ecx * 8], eax
    add  ebx, 0x200000
    inc  ecx
    cmp  ecx, 512
    jne  .map_pd3

    ; ── 4. Load PML4 into CR3 ────────────────────────────────
    mov  eax, pml4_table
    mov  cr3, eax

    ; ── 5. Enable PAE ────────────────────────────────────────
    mov  eax, cr4
    or   eax, CR4_PAE
    mov  cr4, eax

    ; ── 6. Enable Long Mode via EFER MSR ─────────────────────
    mov  ecx, EFER_MSR
    rdmsr
    or   eax, EFER_LME
    wrmsr

    ; ── 7. Enable paging + protected mode ────────────────────
    mov  eax, cr0
    or   eax, (CR0_PE | CR0_PG)
    mov  cr0, eax

    ; ── 8. Far jump into 64-bit code segment ─────────────────
    lgdt [gdt64.pointer]
    jmp  GDT_CODE64:.long_mode_entry

.no_cpuid:
.no_long_mode:
    ; Hang — cannot boot on this CPU
    cli
.halt:
    hlt
    jmp .halt

; ============================================================
; 64-bit entry point
; ============================================================
bits 64
.long_mode_entry:
    ; Reload segment registers with 64-bit data selector
    mov  ax, GDT_DATA64
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Reload stack pointer (still valid — just re-confirm)
    mov  rsp, stack_top

    ; ── Initialise x87 FPU and SSE/SSE4.2 state ─────────────
    ; CR4.OSFXSR (bit 9)  must be set for FXSAVE/FXRSTOR and LDMXCSR
    ; CR4.OSXMMEXCPT (bit 10) enables #XM exception for SSE (optional but clean)
    ; Without CR4.OSFXSR, LDMXCSR raises #UD — undefined instruction fault
    mov  rax, cr4
    or   rax, (1 << 9) | (1 << 10)
    mov  cr4, rax

    ; fninit: reset x87 FPU to default state (masks all FP exceptions)
    fninit

    ; ldmxcsr: load SSE control/status register from a static label
    ; 0x1F80 = mask all SSE FP exceptions, round-to-nearest
    ldmxcsr [rel mxcsr_default]

    ; Zero-extend edi/esi into rdi/rsi for System V ABI
    mov  rdi, rdi           ; multiboot magic  (already in rdi via edi)
    mov  rsi, rsi           ; multiboot info ptr

    ; Call C kernel entry
    call kernel_main

    ; Should never return — halt forever
    cli
.dead:
    hlt
    jmp .dead

; ============================================================
; Section: .rodata  (GDT)
; ============================================================
section .rodata

; MXCSR default value: mask all SSE floating-point exceptions, round-to-nearest
mxcsr_default:
    dd 0x00001F80

gdt64:
    ; Null descriptor
    dq 0x0000000000000000

    ; Code segment (64-bit): execute/read, DPL=0, L=1, D=0
    dq 0x00AF9A000000FFFF

    ; Data segment (64-bit): read/write, DPL=0
    dq 0x00CF92000000FFFF

.pointer:
    dw ($ - gdt64 - 1)     ; limit
    dq gdt64               ; base
