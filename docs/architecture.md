# SAM OS — Architecture Document

**Structured Adaptive Machine Operating System**  
Bare-metal x86-64 research kernel for CPU-only AI inference on legacy hardware (SSE4.2+, 2008+)

---

## Module Graph

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              GRUB2 / Multiboot2                             │
└─────────────────────────────────┬───────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            boot.asm (Entry)                                 │
│  • Multiboot2 header check    • Protected mode → Long mode                 │
│  • Identity-map 4 GiB (2 MiB pages)  • GDT + TSS + IST                     │
│  • FPU/SSE init (CR4.OSFXSR)   • Calls kernel_main(magic, info)            │
└─────────────────────────────────┬───────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           kernel/main.c (Core)                              │
│  • Serial + VGA output         • IDT (32 exception stubs + IRQ 0x21/0x20) │
│  • Domain allocator (3 domains)  • SIMD init + benchmarks                 │
│  • MCP hardware scan            • E820 validation                         │
│  • Boot configurator / Wizard   • Shell entry point                       │
└────────┬──────────────┬──────────────┬──────────────┬──────────────────────┘
         │              │              │              │
         ▼              ▼              ▼              ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  Compute     │ │  Hardware    │ │  Memory/Task │ │  User-Space  │
│  (AI Domain) │ │  Abstraction │ │  Management  │ │  Stack       │
├──────────────┤ ├──────────────┤ ├──────────────┤ ├──────────────┤
│ simd.h       │ │ acpi.h       │ │ vmm.h        │ │ syscall.h    │
│ stf.h        │ │ ps2kbd.h     │ │ scheduler.h  │ │ userasm.asm  │
│ elf.h        │ │ ata.h        │ │ pit.h        │ │ vfs.h        │
└──────────────┘ │ fb.h         │ │ idt.h        │ │ shell.h      │
                 │ mcp.h        │ │ panic.h      │ └──────┬───────┘
                 │ wizard.h     │ │ boot_config.h│        │
                 │ boot_config.h│ └──────────────┘        ▼
                 └──────────────┘                    ┌──────────────┐
                                                    │ User Programs│
                                                    │ (initrd ELF) │
                                                    │ hello.elf    │
                                                    │ guard.elf    │
                                                    │ loop*.elf    │
                                                    │ echo.elf     │
                                                    └──────────────┘
```

**Module Responsibilities**

| Module | File(s) | Primary Responsibility |
|--------|---------|------------------------|
| **Boot/Entry** | `boot.asm` | Multiboot2 → protected mode → GDT → 4 GiB identity-map page tables → long mode → FPU/SSE init → `kernel_main` |
| **Core Kernel** | `main.c` | Serial/VGA output, IDT setup, domain allocator, SIMD init, MCP scan, E820 validation, wizard/shell launch |
| **SIMD Compute** | `simd.h` | INT8 dot-product & matmul with scalar/SSE4.2/AVX2 dispatch; CPUID detection; PIT-calibrated GOPS benchmark |
| **Model Formats** | `stf.h`, `tools/to_stf.py` | STF (SAM Tensor Format) loader: Q8_0 dequantization, tensor index; GGUF header parse for metadata |
| **Hardware Abstraction** | `acpi.h`, `ps2kbd.h`, `ata.h` | ACPI RSDP/RSDT/XSDT/MADT parse; full recursive PCI scan; IRQ-driven PS/2 keyboard; ATA PIO read-only |
| **Framebuffer/GUI** | `fb.h`, `wizard.h`, `boot_config.h` | Bochs VBE 1024×768 pixel FB; 8×16 font; graphical OOBE wizard (hostname, WiFi scan); VGA text fallback |
| **Scheduler** | `scheduler.h` | Cooperative round-robin over 3 fixed domains (AI/GAME/GENERAL); each gets 1 quantum per `sam_sched_tick()` |
| **VMM (Phase 4)** | `vmm.h` | Per-task page tables: fresh PML4/PDPT/PD; user window (2×2 MiB at 0x19000000/0x19200000) U/S=1; rest supervisor-only |
| **Syscalls** | `syscall.h`, `userasm.asm` | `int 0x80` gate (DPL=3); ABI: `write(1)`, `exit(code)`, `read(0)`; ring-3 trampolines via `iretq` / longjmp |
| **VFS** | `vfs.h` | In-memory USTAR initrd parser; `open/read/close`; no writeback, no dirs |
| **ELF Loader** | `elf.h` | Static ET_EXEC x86-64 loader; PT_LOAD copy + BSS zero; validates segments inside user window |
| **Shell** | `shell.h` | Interactive PS/2+FB/VGA shell: `help`, `cpu`, `mem`, `mode`, `res`, `setup`, `clear`, `reboot`, `run <elf> [args]` |
| **PIT/Preemption** | `pit.h`, `syscall.h:138-155` | 100 Hz timer on IRQ0; preempts ring-3 task after 3 ticks (~30 ms); full register snapshot + `sam_user_resume()` |

---

## Boot Flow

```
GRUB2 (multiboot2)
    │
    ▼
_start (boot.asm, 32-bit)
    ├─ Check CPUID support
    ├─ Check Long Mode (CPUID 0x80000001:LM)
    ├─ Build identity-map page tables (4 GiB, 2 MiB pages)
    │    PML4[0] → PDPT[0..3] → PD[0..511] each
    │    GiB 0-1: U/S=1 (user-accessible for ring 3)
    │    GiB 2-3: supervisor-only
    ├─ Load PML4 into CR3
    ├─ Enable PAE (CR4.PAE)
    ├─ Enable LME (EFER.MSR)
    ├─ Enable Paging + PE (CR0.PG|PE)
    └─ Far jump to GDT_CODE64:.long_mode_entry (64-bit)
         │
         ▼
.long_mode_entry (64-bit)
    ├─ Load data segments (GDT_DATA64)
    ├─ Patch TSS descriptor base, load TSS (LTR)
    ├─ Set CR4.OSFXSR | CR4.OSXMMEXCPT
    ├─ fninit + ldmxcsr(0x1F80)  ← SSE4.2 requirement
    ├─ Zero-extend EDI/ESI → RDI/RSI (multiboot magic, info ptr)
    └─ CALL kernel_main
         │
         ▼
kernel_main (main.c)
    ├─ serial_init(), vga_clear(), print_banner()
    ├─ idt_init()                    ← Sprint 14: 32 exception stubs
    ├─ acpi_init()                   ← Sprint 15: RSDP→RSDT/XSDT→MADT
    ├─ ps2kbd_init()                 ← Sprint 15: PIC remap, IRQ 1 @ 0x21
    ├─ ata_init()                    ← Sprint 15: ATA PIO identify+read
    ├─ PIT channel-2 TSC calibration ← Sprint 8/5: real CPU MHz
    ├─ sam_mcp_scan()                ← Sprint 10: CPU, RAM, PCI, GPU, NVMe, WiFi
    ├─ fb_init()                     ← Sprint 12: VBE 1024×768 or VGA fallback
    ├─ CI mode check (cmdline "ci")  ← headless test path
    ├─ sam_wizard_run()              ← Sprint 12-13: graphical OOBE (if not CI)
    ├─ sam_boot_config_read()        ← Sprint 10: dynamic domain sizes from 0x7000
    ├─ E820 validation of domains    ← Sprint 14: skip domains in reserved RAM
    ├─ domain_alloc() × 3            ← AI (256 MiB), GAME (64 MiB), GENERAL (64 MiB)
    ├─ SIMD detection + init         ← Sprint 2-3: scalar/SSE4.2/AVX2
    ├─ INT8 dot-product test         ← Sprint 2: dot([1..32],[1..1]) == 528
    ├─ INT8 matmul test              ← Sprint 4: 4×32 × 32×4 all 32
    ├─ Throughput benchmark          ← Sprint 5: 32×128×32 @ 100 iter, PIT-calibrated GOPS
    ├─ GGUF header parse             ← Sprint 6: multiboot module metadata
    ├─ Scheduler init + sentinels    ← Sprint 8: 3-domain round-robin
    ├─ STF tensor loader             ← Sprint 9: Q8_0 dequant proof
    ├─ Sprint 10 dynamic sentinels   ← verify writable domains
    ├─ vmm_init() + pit_init_100hz() ← Sprint 17/20: page-table pool + preemption clock
    ├─ VFS init (initrd USTAR)       ← Sprint 16: mount / from GRUB module
    ├─ sam_run_task("/guard.elf")    ← Sprint 17: isolation probe (#PF expected)
    ├─ sam_run_task("/hello.elf")    ← Sprint 18: clean exit, kernel resumes
    ├─ sam_run_pair("/loopa","loopb")← Sprint 21: two resident tasks round-robin
    └─ sam_shell_run()               ← Sprint 11/12/19/22: interactive shell
```

---

## Memory Layout

### Physical Memory Map (Post-Boot)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 0x0000_0000  ── 0x000F_FFFF  │  1 MiB     │  BIOS / VGA / Low memory        │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x0010_0000  ── 0x001F_FFFF  │  1 MiB     │  Kernel ELF (loaded at 1 MiB)   │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x0020_0000  ── 0x101F_FFFF  │  256 MiB   │  AI DOMAIN (id=0x10)            │
│                              │            │  KV cache, activations, scratch  │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1020_0000  ── 0x141F_FFFF  │  64 MiB    │  GAME DOMAIN (id=0x20)           │
│                              │            │  Game state, framebuffer scratch │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1420_0000  ── 0x181F_FFFF  │  64 MiB    │  GENERAL DOMAIN (id=0x30)        │
│                              │            │  App buffers                     │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1820_0000  ── 0x183F_FFFF  │  2 MiB     │  (Gap)                          │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1840_0000  ── 0x187F_FFFF  │  4 MiB     │  VMM PAGE-TABLE POOL             │
│                              │            │  Bump allocator for per-task CR3  │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1880_0000  ── 0x18FF_FFFF  │  8 MiB     │  (Gap / future expansion)        │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0x1900_0000  ── 0x1CFF_FFFF  │  64 MiB    │  TASK PHYSICAL ARENA (Sprint 24) │
│                              │            │  slot k: code=k·4MiB,            │
│                              │            │          stack=+2MiB             │
│                              │            │  Freed slots REUSE same backing  │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0xE000_0000  ── 0xE00F_FFFF  │  1 MiB     │  Bochs VBE Framebuffer (VBoxVGA) │
├─────────────────────────────────────────────────────────────────────────────┤
│ 0xFFC0_0000  ── 0xFFFF_FFFF  │  4 MiB     │  Boot page tables (identity map) │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Domain Allocation (Boot Config Override)

The OOBE wizard writes a `sam_boot_config_t` at physical `0x7000`. If valid, it overrides static sizes:

| Domain | Default Base | Default Size | Config Field |
|--------|--------------|--------------|--------------|
| AI | 0x0020_0000 (2 MiB) | 0x1000_0000 (256 MiB) | `bcfg.ai_base`, `bcfg.ai_size` |
| GAME | 0x1020_0000 (258 MiB) | 0x0400_0000 (64 MiB) | `bcfg.game_base`, `bcfg.game_size` |
| GENERAL | 0x1420_0000 (322 MiB) | 0x0400_0000 (64 MiB) | `bcfg.general_base`, `bcfg.general_size` |

Each domain is validated against the Multiboot2 E820 memory map (type 1 = available). Domains overlapping reserved/ACPI/bad memory are skipped with `[WARN]`.

### Per-Task Address Space (Sprint 17, canonical-VA model since Sprint 24)

Created by `vmm_create_user_as_pa(code_pa, stack_pa)`. Every task sees the SAME
canonical user virtual layout; each CR3 maps it to slot-owned physical frames:

```
Canonical user VIRTUAL layout (identical in all tasks):
  USER_CODE_VA   0x19000000–0x19200000  (2 MiB code/data)
  USER_STACK_VA  0x19200000–0x19400000  (2 MiB stack, top = 0x19400000)

Page-table construction:
PML4 (fresh page)
 └─ PML4[0] → PDPT (fresh, U/S=1)
      ├─ PDPT[0] → PD (fresh, covers GiB 0, U/S=1)
      │    ├─ All 512 entries: supervisor RW identity 2 MiB pages (0x83)
      │    ├─ pd[200] = code_pa  | 0x87   ← U/S=1 → task code backing
      │    └─ pd[201] = stack_pa | 0x87   ← U/S=1 → task stack backing
      ├─ PDPT[1] → boot pdpt_table[1] & ~0x04  ← GiB 1, supervisor only
      ├─ PDPT[2] → boot pdpt_table[2] & ~0x04  ← GiB 2, supervisor only
      └─ PDPT[3] → boot pdpt_table[3] & ~0x04  ← GiB 3, supervisor only

Physical backing (deterministic by TCB slot k):
  code_pa  = TASK_ARENA_BASE(0x19000000) + k·4 MiB
  stack_pa = code_pa + 2 MiB
```

**Isolation**: exactly two U/S=1 entries exist per fresh PD and they target the
task's own frames. Another task's physical memory is reachable in this address
space ONLY through supervisor-only identity entries — ring-3 access faults (#PF).
A live audit (`sam_audit_user_pd`) walks the constructed tables at creation and
prints both entries; CI asserts `DIVERGENT` PA ownership across slots.

**ELF loading**: `elf_load_pa()` requires every PT_LOAD vaddr and the entry to
lie inside the canonical 2 MiB code region, copies bytes to `code_pa + offset`,
and zeroes BSS in the physical backing. argv strings are written to physical
stack backing while all stored pointers and the initial RSP are canonical
virtual addresses (`pa2va_delta = stack_pa − USER_STACK_BASE`).

---

## Sprint History

| Sprint | Date | Theme | Key Deliverable |
|--------|------|-------|-----------------|
| **1** | 2026-06-21 | Bare Metal Bootstrap | Multiboot2 → long mode, AVX2 detect, 2 domains (AI/GAME), AVX2 dot-product = 528 |
| **2** | 2026-06-21 | Unified INT8 Engine | `simd.h` with scalar/SSE4.2/AVX2 dispatch; SSE4.2 path on VAIO |
| **3** | 2026-06-21 | MXCSR + SSE4.2 HW Path | Fixed CR4.OSFXSR missing; `PMADDUBSW` hardware path works on Core i5 |
| **4** | 2026-06-21 | INT8 MatMul | 4×32 × 32×4 tiled matmul; all 16 outputs = 32 [PASS] |
| **5** | 2026-06-21 | Throughput Benchmark | 32×128 × 128×32 @ 100 iter; PIT-calibrated **1.64 GOPS** INT8 SSE4.2 |
| **6** | 2026-06-21 | GGUF Header Parse | GRUB module → direct pointer cast → GGUF v3 header valid on bare metal |
| **7** | 2026-06-21 | Pong (Game Loop) | PIC remap, IDT, PIT 60Hz, VGA text, PS/2 keyboard; score 29-29 draw |
| **8** | 2026-06-21 | 3-Domain Scheduler | GENERAL domain added; PIT CPU MHz calibration; cooperative round-robin |
| **9** | 2026-06-21 | STF Tensor Loader | STF format, Q8_0 dequant, AI domain expanded to 256 MiB |
| **10** | 2026-06-22 | MCP + Boot Config TUI | Hardware scan (CPU/RAM/GPU/NVMe/WiFi); VGA text TUI mode picker |
| **11** | 2026-06-22 | Interactive Shell | PS/2 + VGA shell: help, cpu, mem, mode, clear, reboot, history |
| **12** | 2026-06-22 | Graphical OOBE Wizard | Bochs VBE 1024×768 pixel GUI: hostname, WiFi scan, summary |
| **13** | 2026-06-23 | Wizard→Shell Fix | Root-caused keyboard drain bug; fixed with `pause` in polling loop |
| **14** | 2026-06-23 | Kernel Safety | Full IDT (32 vectors), panic screen (VGA+FB+serial), E820 validation |
| **15** | 2026-06-23 | Real Hardware | ACPI (RSDP/MADT), full recursive PCI, PS/2 IRQ keyboard, ATA PIO read |
| **16** | 2026-08-23 | VFS + initrd + Syscalls | USTAR initrd, `int 0x80` (write/exit/read), first ring-3 process (hello.bin) |
| **17** | 2026-08-23 | Per-Task Page Tables | Real ring-3 isolation: guard.elf touches 0x100000 → #PF → kernel resumes |
| **18** | 2026-08-24 | Task Switching | Kernel survives exit/fault via setjmp/longjmp; shell can run programs |
| **19** | 2026-08-24 | ELF64 Loader + `run` | Static ET_EXEC loader; shell `run hello.elf` loads initrd ELF in ring 3 |
| **20** | 2026-08-24 | PIT Preemption | 100 Hz timer; 3-tick quantum; full register snapshot + transparent resume |
| **21** | 2026-08-24 | Round-Robin 2 Tasks | Two resident tasks, separate CR3, interleaved output [A]1 [A]2 [B]1 [B]2... |
| **22** | 2026-08-24 | argv + Exit Codes | SysV stack layout (argc/argv); echo.elf prints args, exits with argc |
| **23** | 2026-08-25 | N-Task Kernel | 16-slot FREE/READY/RUNNING/ZOMBIE table; generic create + run/drain loop; E820 per-create validation |
| **24** | 2026-08-25 | Shared User VA | Canonical user VA in every CR3 → slot-owned PA backing; one binary ×4 resident; cross-process #PF probe; E820 capacity discovery |

---

## Roadmap

### Phase 1 — Truth Stabilization ✅ **Done**
- Accurate docs, reproducible build, `make test` (CI boots ISO in QEMU, asserts serial markers)

### Phase 2 — Kernel Safety Baseline ✅ **Done (Sprint 14)**
- 32 exception handlers, panic screen, E820 domain validation, module bounds checks

### Phase 3 — Real Hardware Abstraction ✅ **Mostly Done (Sprint 15)**
- ACPI (RSDP/RSDT/XSDT/MADT), full recursive PCI scan, PS/2 IRQ keyboard, ATA PIO read
- **Remaining**: USB HID (after XHCI driver), full PCI bridge enumeration

### Phase 4 — Minimal Storage & App Model 🔄 **In Progress (Sprints 16–22+)**

| Sprint | Status | Deliverable |
|--------|--------|-------------|
| 16 | ✅ | VFS (USTAR initrd), `int 0x80` syscalls, first ring-3 process |
| 17 | ✅ | Per-task page tables (hardware ring-3 isolation) |
| 18 | ✅ | Kernel resumes after task exit/fault (setjmp/longjmp) |
| 19 | ✅ | ELF64 loader, shell `run` command |
| 20 | ✅ | PIT 100 Hz preemption + transparent task resume |
| 21 | ✅ | Round-robin between 2 resident ring-3 tasks |
| 22 | ✅ | argv + exit codes surfaced to shell |
| 23 | ✅ | Fixed-capacity N-task kernel (16 slots, state machine, generic scheduler) |
| 24 | ✅ | Shared user virtual address space; deterministic per-slot physical backing; identical-binary multi-process; cross-process isolation proof |
| **25 (Next)** | 📋 | **waitpid / parent-child reaping, process lifecycle** |
| **26** | 📋 | **Writable filesystem (ATA write → AHCI), initrd read-write** |

### Phase 5 — Actual Inference Milestone 📋 **Future**
- Load real quantized model (Q8_0/Q4_K, 1M–10M params)
- Tokenizer (BPE/unigram from STF metadata)
- Transformer forward pass (attention + FFN using SIMD matmul)
- Next-token loop (argmax/top-k), honest tok/s via PIT-calibrated TSC

### Phase 6 — Compatibility Strategy 📋 **Far Future**
1. Freeze SAM ABI (syscall numbers, calling convention)
2. Embed Lua 5.4 interpreter as SAM executable
3. WASM interpreter (wasm3 or custom) as SAM executable
4. JVM subset (classfile loader + bytecode interpreter) if use case
5. Linux syscall compatibility (2–3 years after Phase 4 solid)

---

## Design Principles

1. **Hardware-Honest** — One heavy workload at a time (AI or GAME); no fake concurrency
2. **No GPU Required** — INT8 matrix compute via SSE4.2/AVX2 on bare metal
3. **Formal Inspiration** — Domain isolation, scheduler liveness, privilege enforcement designed alongside PSL Lean 4 proofs (39 sprints); connection currently conceptual
4. **Reproducible** — Every claim traces to a boot log marker (`[PASS]`, `[OK]`, `Sprint N PASS`)
5. **Sovereign** — GPL-3.0; no foreign vendor dependencies; Make-in-India aligned

---

## Current Limitations (Honest)

| Area | Status |
|------|--------|
| **SMP** | Single-core only; no AP startup, no per-CPU data |
| **Memory** | No demand paging, no swap, no COW; static per-task CR3 |
| **Storage** | ATA PIO read-only; no writeback, no AHCI/NVMe driver |
| **Network** | No NIC driver, no TCP/IP stack |
| **Userland** | Max 2 resident tasks (fixed); no `wait()`, no dynamic loader, no libc |
| **Graphics** | Framebuffer only; no compositor, no GPU acceleration |
| **Security** | No ASLR, no W^X enforcement beyond U/S bits, no capability system |

---

## Build & Test

```bash
# Prerequisites (Ubuntu/Debian/WSL2)
sudo apt update && sudo apt install -y \
    nasm gcc binutils grub-pc-bin grub-common xorriso mtools qemu-system-x86

# Build ISO
make clean && make
# Output: build/sam_os.iso

# Headless CI test (exact command run by GitHub Actions)
make test
# Boots CI ISO in QEMU, captures serial, asserts:
#   dot([1..32]  [PASS] matmul  [PASS] benchmark  [PASS] STF  Sprint 22 PASS

# Run with VGA (VirtualBox: VBoxVGA controller, 128 MB VRAM, 3D disabled, EFI off)
make run-vga

# Flash to USB (real hardware)
sudo dd if=build/sam_os.iso of=/dev/sdX bs=4M && sync
```

---

## Relationship to PSL (Paninian Systems Language)

| PSL Theorem | Design Intent in SAM OS | Runtime Enforcement Today |
|-------------|------------------------|---------------------------|
| `hypervisor_isolation` | AI/GAME domains non-overlapping | `domain_alloc()` overlap check — **no HW isolation** |
| `global_liveness` | One heavy domain at a time | Sprint 8: cooperative round-robin, 1 quantum/domain/tick |
| `machine_complete` | All kernel states reachable from boot | Structural goal — **not verified** |
| `user_cannot_store` | Ring 3 cannot write kernel domains | **Sprint 17**: per-task CR3 enforces U/S=0 on kernel pages |

*The domain allocator is an if-statement; real enforcement requires separate page table roots per domain — achieved in Sprint 17 for ring-3 user tasks.*

---

*Generated from source review. See `README.md`, `VISION.md`, `ROADMAP.md`, `STATUS.md` for latest updates.*