# SAM OS — Project Status

**SAM OS** (Structured Adaptive Machine Operating System)
A bare-metal x86-64 research kernel exploring CPU-only INT8 inference and
real-time game domains on old hardware. No GPU required.

---

## Philosophy

Modern OSes were built for general-purpose computing and patched for AI and gaming.
SAM OS explores the opposite direction: design the memory model, scheduler, and compute
engine around AI inference and real-time game domains from the start.

The PSL (Paninian Systems Language) project proved formal theorems about memory isolation,
scheduling liveness, and privilege enforcement across 39 Lean 4 sprints. SAM OS tests
whether those design properties are achievable on real constrained hardware. The
connection between PSL proofs and SAM OS code is currently **conceptual** — runtime
proof traceability is a future milestone, not a current claim.

---

## Target Hardware

- **Primary**: Sony VAIO VPCEB15FX (Intel Core i5, x86-64, SSE4.2, 4 GiB RAM)
- **Goal**: Any x86-64 machine — SSE4.2 minimum (2008+), AVX2 for full performance (2013+)
- **No GPU required**: AI inference via INT8 matrix multiply (scalar → SSE4.2 → AVX2)

---

## Sprint History

### Sprint 1 — Bare Metal Kernel Bootstrap  ✅
**Date**: 2026-06-21
**Goal**: Boot to 64-bit long mode, detect AVX2, allocate PSL-style memory domains,
prove INT8 compute engine works.

**Files produced**:
| File | Purpose |
|------|---------|
| `kernel/boot.asm` | Multiboot2 header, GDT, long mode entry (NASM) |
| `kernel/main.c` | AVX2 detection, domain allocator, INT8 dot-product proof |
| `kernel/linker.ld` | Kernel ELF linker script (load at 1 MiB) |
| `kernel/grub.cfg` | GRUB2 multiboot2 menu entry |
| `Makefile` | Single `make` → bootable `build/sam_os.iso` |

**What Sprint 1 demonstrates**:
- GRUB2 multiboot2 handshake → 64-bit long mode entry
- AVX2 detection via CPUID (leaf 7, subleaf 0, EBX bit 5)
- Two PSL-style isolated memory domains allocated at boot:
  - AI domain   id=0x10  base=2 MiB   size=64 MiB
  - Game domain id=0x20  base=66 MiB  size=64 MiB
- AVX2 INT8 dot-product: `dot([1..32],[1..1]) = 528` [PASS]
- VGA text output with colour-coded status

**Boot screen on success**:
```
============================================================
  SAM OS  v0.1.0  |  Structured Adaptive Machine
  Proof-Native Kernel  |  Sprint 1  |  2026
============================================================

[OK] Multiboot2 handshake verified
[OK] CPU feature detection:
     AVX  : YES
     AVX2 : YES
[OK] Allocating memory domains:
  [domain 0x10] AI-INFERENCE  base=0x0000000000200000  size=64 MiB
  [domain 0x20] GAME-ENGINE   base=0x0000000004200000  size=64 MiB
[OK] Domain isolation: no overlap detected

[OK] AVX2 INT8 compute engine test:
     dot([1..32],[1..1]) = 528  expected=528  [PASS]

============================================================
[SAM OS] AI domain  : ready  (id=0x10, 64 MiB)
[SAM OS] Game domain: ready  (id=0x20, 64 MiB)
[SAM OS] Sprint 1 PASS — bare metal kernel live
============================================================
```

---

### Sprint 2 — Unified INT8 Compute Engine  ✅
**Date**: 2026-06-21
**Goal**: Wire `kernel/simd.h` into `kernel_main`. Detect SSE4.2 at boot.
Dispatch INT8 dot product through unified `sam_int8_dot()`. Prove result=528 on VAIO.

**Files produced / modified**:
| File | Change |
|------|--------|
| `kernel/simd.h` | New: CPUID detection, scalar/SSE4.2/AVX2 INT8 dispatch |
| `kernel/main.c` | Updated: `sam_simd_init()`, unified compute test section |
| `Makefile` | Updated: `-msse4.2 -fno-tree-vectorize` (replaced `-mavx2`) |
| `evidence/sprint2_compute_evidence.json` | New: boot log, result, debug history |

**Boot output on VAIO**:
```
[OK] INT8 compute engine test:
     SIMD path: SSE4.2 (128-bit)
     dot([1..32],[1..1]) = 528  expected=528  [PASS]
```

**Debug lessons**:
- `-mavx2` causes GCC auto-vectoriser to emit YMM instructions → illegal instruction on VAIO
- SSE4.2 intrinsic path (`PMADDUBSW`) requires MXCSR init in `boot.asm` before use
- Scalar path produces correct result now; SSE4.2 hardware path deferred to Sprint 3

---

### Sprint 3 — MXCSR Init + SSE4.2 Hardware INT8 Path  ✅
**Date**: 2026-06-21
**Goal**: Fix bare-metal SSE4.2 crash. Enable `PMADDUBSW` hardware path. Prove `[PASS]`.

**Root cause**: GRUB2 does not set `CR4.OSFXSR` (bit 9). `LDMXCSR` requires this bit —
without it the CPU raises `#UD` (undefined instruction) which triple-faults silently.

**Fix in `kernel/boot.asm`**:
- Set `CR4.OSFXSR` (bit 9) + `CR4.OSXMMEXCPT` (bit 10) before any SSE instruction
- `fninit` — reset x87 FPU to default state
- `ldmxcsr [rel mxcsr_default]` — load `0x1F80`: mask all SSE FP exceptions, round-to-nearest
- `mxcsr_default dd 0x00001F80` added to `.rodata`

**Boot output on VAIO**:
```
[OK] INT8 compute engine test:
     SIMD path: SSE4.2 (128-bit)
     dot([1..32],[1..1]) = 528  expected=528  [PASS]
```

**What this proves**: SAM OS runs `PMADDUBSW` hardware INT8 matmul on a 2010 Core i5.
No GPU. No OS. No libc. Any x86-64 machine from 2008+ can run SAM OS inference.

---

### Sprint 4 — INT8 Matrix Multiply (Tiled 4x32 x 32x4)  ✅
**Date**: 2026-06-21
**Goal**: Prove `sam_int8_matmul()` on real silicon. All 16 outputs of a 4×32 × 32×4
matmul must equal 32. This is the exact operation at the core of every transformer layer.

**New in `kernel/simd.h`**: `sam_int8_matmul(A, B_T, C, M, K, N)` — M×N calls to
`sam_int8_dot()`, B pre-transposed for contiguous memory access per output element.

**Boot output**:
```
[OK] INT8 matrix multiply test (4x32 x 32x4):
     SIMD path: SSE4.2 (128-bit)
     C[0][0]=32  C[0][3]=32  C[3][0]=32  C[3][3]=32  expected=32  [PASS]
[SAM OS] Sprint 4 PASS -- INT8 matmul on bare metal
```

**What this proves**: The SAM OS kernel can execute transformer-layer matrix multiply
on bare metal on a 2010 Core i5 with no GPU. Scale K to 128+ and you have real LLM
weight projection. The AI domain (0x10) holds the matmul buffers — fully isolated.

**Vision update**: SAM OS is a full OS for constrained hardware — not just AI+Game.
Three domains: AI (inference priority), GAME (16ms deadline), GENERAL (lightweight apps).
Hardware-honest: one heavy workload at a time. Designed for India's 500M+ old PCs.

---

### Sprint 5 — Throughput Benchmark (RDTSC INT8 GOPS)  ✅
**Date**: 2026-06-21
**Goal**: Measure real INT8 throughput on VAIO SSE4.2 using bare-metal RDTSC.
No OS timer. No libc. No PIT/HPET. Pure TSC ticks.

**Shape**: 32×128 × 128×32 matmul, 100 iterations → 26,214,400 total arithmetic ops

**Raw measurements on VAIO Core i5**:
- TSC ticks for 100 iterations: **32,335,360**
- Calibration: 50M busy-loop = 101,477,420 ticks → ~2.03 GHz effective clock
- **Corrected throughput: ~1.6 GOPS INT8** (SSE4.2, bare metal, no OS)

**Model feasibility**:
- TinyLlama 1B → ~0.75 tok/sec on VAIO bare metal
- GPT-2 small 117M → ~6 tok/sec ← first real inference target

**Boot output** (after formula fix — cal-loop calibrated, no assumed GHz):
```
[OK] Sprint 5: Throughput benchmark
     TSC ticks : 32335360  |  Total ops : 26214400
     Cal ticks  : 101477420 (50M loop)
     Throughput : 1.64 GOPS (SSE4.2 INT8, cal-loop calibrated)  [PASS]
[SAM OS] Sprint 5 PASS -- throughput benchmark live
```
Note: the original banner printed 121.60 GOPS using a hardcoded 3 GHz assumption.
The corrected formula uses the calibration loop as the clock reference. The
~1.6 GOPS figure is the hardware-honest number.

---

### Sprint 6 — GGUF Header Parse (Bare Metal Model Discovery)  ✅
**Date**: 2026-06-21
**Goal**: Prove the SAM OS kernel can locate and parse a GGUF model file on bare metal.
GRUB2 loads the file as a multiboot2 module; the kernel walks the MB2 tag list, finds
the module physical address, and reads the header directly from raw memory.

**GGUF file**: synthetic 292-byte v3 header (`tools/make_gguf_header.py`), loaded by GRUB
at physical address `0x10C000`.

**Boot output**:
```
[OK] Sprint 6: GGUF header parse
     Module addr : 0x000000000010C000
     Module size  : 292 bytes
     Cmdline      : gguf_model
     Magic        : GGUF  [OK]
     Version      : 3
     Tensor count : 2
     Metadata KVs : 3
     [PASS] GGUF header valid on bare metal
[SAM OS] Sprint 6 PASS -- GGUF header parsed on bare metal
```

**What this proves**: SAM OS can load model weight metadata on bare metal. The kernel
finds the GGUF file at its physical address and reads it with a direct pointer cast —
zero copies, zero allocations, no OS, no filesystem driver.

---

### Sprint 7 — Pong (Real-Time Game Loop in GAME Domain)  ✅
**Date**: 2026-06-21
**Goal**: Prove SAM OS can run a real-time interactive game on bare metal.
PIC remapped, IDT installed, PIT at 60Hz, VGA framebuffer, PS/2 keyboard — all wired
by hand with no OS, no libc, no GPU. Game state lives at physical address 0x4200000
(GAME domain), isolated from the AI domain by the PSL-derived domain allocator.

**Hardware wired this sprint**:
- 8259 PIC remapped: IRQ0-7 → vectors 32-39 (avoids exception collision)
- 8253 PIT: channel 0, ~60Hz, fires IRQ0 → increments `game_ticks`
- IDT: 64-bit, vector 32 (timer) + vector 33 (keyboard) installed
- VGA: 0xB8000, 80×25 text mode, block chars for ball and paddles
- PS/2 keyboard: port 0x60, scan codes for W/S/Up/Down/Q

**Final score on first ever SAM OS game: L:29 R:29 — a draw.**

**Boot output**:
```
[OK] Sprint 7: Launching Pong in GAME domain (0x4200000)
     PIC remap, IDT load, PIT 60Hz, game loop starting...
[OK] Sprint 7: Game loop exited cleanly
[SAM OS] Game domain: ready  (id=0x20, 64 MiB)
[SAM OS] Sprint 7 PASS -- Pong on bare metal
```

**Photo evidence**: VAIO screen showing "GAME OVER Q pressed / Final score L:29 R:29 /
SAM OS GAME domain: proved" — the first game ever played on SAM OS.

---

### Sprint 8 — GENERAL Domain + Three-Way Scheduler  ✅
**Date**: 2026-06-21
**Goal**: Add GENERAL domain, real CPU frequency via PIT calibration,
cooperative three-way scheduler. Prove all three domains can coexist.

**New this sprint**:
- GENERAL domain: id=0x30, base=130 MiB (0x8200000), size=64 MiB
- PIT channel-2 calibration: real CPU MHz measured vs. known 1.193182 MHz PIT clock
  (~10ms window, polled OUT2 bit) — collapses Sprint 5 MOPS range to one accurate number
- `kernel/scheduler.h`: cooperative round-robin scheduler, three slots (AI/GAME/GENERAL)
- Sentinel proof: each domain writes and reads back a unique magic value to its region
- Scheduler proof: submit null-work to all three, call one tick, verify 3 quanta ran

**Three-domain physical layout (non-overlapping, verified at boot)**:

| Domain  | ID   | Base       | End        | Size  |
|---------|------|------------|------------|-------|
| AI      | 0x10 | 0x0200000  | 0x4200000  | 64 MiB |
| GAME    | 0x20 | 0x4200000  | 0x8200000  | 64 MiB |
| GENERAL | 0x30 | 0x8200000  | 0xC200000  | 64 MiB |

**Boot output**:
```
[OK] Sprint 8: GENERAL domain + three-way scheduler
     GENERAL domain base : 0x0000000008200000  size=64 MiB
     AI sentinel      : 0x00000000A1A1A1A1  [PASS]
     GAME sentinel    : 0x00000000CAFECAFE  [PASS]
     GENERAL sentinel : 0x000000006E6E6E6E  [PASS]
     Scheduler quanta : 3  [PASS] all 3 domains scheduled
     [PASS] Sprint 8: three-domain isolation + scheduler

[SAM OS] AI domain     : ready  (id=0x10, 64 MiB)
[SAM OS] Game domain   : ready  (id=0x20, 64 MiB)
[SAM OS] General domain: ready  (id=0x30, 64 MiB)
[SAM OS] Sprint 8 PASS -- three-domain kernel
```

**What this proves**: SAM OS now has three isolated memory domains, a real CPU
frequency measurement (not an assumed clock), and a cooperative scheduler that
can round-robin all three domains in a single tick without corruption.

---

### Sprint 9 — STF Tensor Loader + AI Domain Expansion  ✅
**Date**: 2026-06-21
**Goal**: Define SAM Tensor Format (STF), load model weights into AI domain on bare metal.

**New this sprint**:
- `kernel/stf.h`: SAM Tensor Format — magic header, tensor index, Q8_0 dequantize proof
- `tools/to_stf.py`: converts GGUF/safetensors to STF for GRUB module loading
- AI domain expanded to 256 MiB (base=2 MiB, size=256 MiB)
- STF loader wired into `kernel_main`: walks tensor index, dequantizes first weight block, proves value in range

**Boot output**:
```
[OK] Sprint 9: STF tensor loader
     Magic    : STF1  [OK]
     Tensors  : 2
     Dequant  : weight[0] = 0.0312  (expected ~0.03)  [PASS]
[SAM OS] Sprint 9 PASS -- STF loader on bare metal
```

---

### Sprint 10 — Hardware MCP + VGA Boot Configurator TUI  ✅
**Date**: 2026-06-22
**Goal**: Scan CPU/RAM/GPU at boot, present interactive mode-picker TUI, write domain config to 0x7000.

**New this sprint**:
- `kernel/mcp.h`: Machine Capability Profile — RAM, CPU brand, SIMD, GPU class, NVMe, wireless
- `kernel/fb.h`: Framebuffer abstraction (falls back to VGA text mode on VirtualBox BIOS)
- `kernel/boot_config.h`: VGA text-mode TUI — 80×25, dark blue bg, 4 boot modes, `>` cursor, countdown, PS/2 keyboard
- `kernel/main.c`: MCP scan wired in, `sam_boot_config_run()` called at boot, domain sizes computed from RAM

**Key discovery**: GRUB2 + BIOS + VirtualBox always returns EGA text mode (0xB8000, bpp=16).
VBE pixel framebuffer is impossible in this stack. Solution: full VGA text TUI as primary path.

**Boot screen** (confirmed on VAIO via VirtualBox):
- Dark blue 80×25 screen, cyan title bar, 4 mode items, bright-blue `>` cursor blinks on General Mode
- RAM: 511 MB, CPU: 12th Gen Intel i5-12450H, SSE4.2
- Arrow keys navigate, Enter confirms, 10s countdown auto-selects GENERAL

**Boot output on selection**:
```
Boot GUI  : shown (VGA text TUI)
Boot mode : GENERAL
Sprint 10 PASS -- Boot Configurator on bare metal
```

---

### Sprint 11 — Interactive Kernel Shell  ✅
**Date**: 2026-06-22
**Goal**: After boot TUI selection, drop into a bare-metal interactive shell instead of halting.

**Changes**:
- Removed ping-pong game (`game.h`, `sam_game_run`) — Sprint 7 proof preserved in STATUS.md
- Removed number prefixes from TUI menu (arrow keys primary, numbers still work)
- Added `kernel/shell.h`: full PS/2 + VGA interactive shell, self-contained, no libc

**Shell features**:
- Commands: `help`, `cpu`, `mem`, `mode`, `clear`, `reboot`
- 8-entry command history, up/down arrow navigation
- Cursor blink via polling counter (~4 Hz), invert-colour block cursor
- Scroll: shifts all 25 rows up when bottom reached
- Reboot: pulse 0xFE to port 0x64 (keyboard controller reset)

**Confirmed on VAIO (VirtualBox, 511 MB RAM)**:
```
============================================================
  SAM OS  v0.1.0  |  Kernel Shell  |  Sprint 11
============================================================
  Type 'help' for available commands.

SAM> cpu
  CPU   : 12th Gen Intel(R) Core(TM) i5-12450H
  SIMD  : SSE4.2
  RAM   : 511 MB
  GPU   : Integrated

SAM> mem
  Domain layout:
  AI      base=0x0000000000200000  size=64 MiB
  GAME    base=0x0000000004200000  size=32 MiB
  GENERAL base=0x0000000006200000  size=32 MiB

SAM> mode
  Boot mode : GENERAL
  Config at : 0x7000
```

**What this proves**: SAM OS is now interactive on bare metal. The kernel can receive typed
commands, parse them, query hardware state, and respond — all without an OS, libc, or GPU.

---

### Sprint 12 — Graphical OOBE Wizard (Phase 1)  ✅
**Date**: 2026-06-22
**Goal**: Replace VGA text TUI with a full pixel-rendered OOBE wizard using Bochs VBE framebuffer.

**New this sprint**:
- `kernel/wizard.h`: pixel GUI wizard — dark sidebar, step progress dots, content panel
- Screens: Welcome → Hostname (text input) → WiFi scan (PCI class 0x0280) → Summary → Done
- Bochs VBE direct I/O (ports 0x01CE/0x01CF): 1024×768, 32bpp linear framebuffer at 0xE0000000
- `kernel/fb.h`: pixel primitives — `fb_fill_rect`, `fb_draw_char`, `fb_draw_str`, 8×16 bitmap font
- VirtualBox constraint: VBoxVGA required (not VMSVGA); framebuffer at fixed physical 0xE0000000

**Boot flow**: GRUB → kernel → wizard (pixel GUI) → config written to 0x7000 → kernel shell

---

### Sprint 13 — Wizard Polish + Shell Transition Fix  ✅
**Date**: 2026-06-23
**Goal**: Fix wizard → shell transition. Root-cause and fix keyboard handling bug.

**Bug found and fixed**:
- `wz_drain_keyboard()` ran a 1,000,000-iteration `pause` loop before the done screen's `wz_wait_key()` call
- During that loop, VirtualBox injected the user's Enter keypress (0x1C make code) into the i8042 buffer
- The drain then read and discarded it — `wz_wait_key()` never saw the keypress and blocked forever
- **Fix**: removed `wz_drain_keyboard()` entirely from `wz_screen_done()`. `wz_wait_key()` already
  filters break codes (`sc >= 0x80`) so no drain is needed. Added `pause` instruction to `wz_wait_key()`
  polling loop to yield to hypervisor between polls.
- Added `pause` to `wz_wait_key()` — required in VM: without it, tight polling starves VirtualBox's
  i8042 emulation and OBF never gets set.

**Diagnostic approach**: added a live keyboard diagnostic screen showing raw 0x64 status register,
last scancode received, and count — confirmed keyboard was delivering data and identified the drain bug.

**Result**: wizard completes → Enter pressed on done screen → screen clears → kernel shell appears at `SAM>` prompt.

**Shell confirmed working**:
```
SAM OS  v0.1.0  |  Kernel Shell  |  Sprint 13
============================================================
Type 'help' for available commands.
SAM>
```

### Sprint 14 — Kernel Safety Baseline (IDT + Panic + E820 Validation)  ✅
**Date**: 2026-06-23
**Goal**: The kernel stops triple-faulting silently. Any CPU exception now shows a
full panic screen (VGA text + pixel framebuffer overlay + serial) and halts cleanly.

**Files added**:
| File | Purpose |
|------|---------|
| `kernel/panic.h` | `sam_panic()` — red panic screen on VGA text + framebuffer + serial, then HLT |
| `kernel/idt.h` | 256-entry IDT, 32 per-vector stubs (inline asm), `idt_init()` |

**What Sprint 14 adds**:
- 256-entry IDT loaded via `lidt` at the very start of `kernel_main`
- 32 CPU exception stubs covering all x86-64 vectors (0–31); vectors 32–255 get a harmless `iretq` stub
- Stubs save all 15 GP registers + vector + error_code → `cpu_frame_t`, then call `sam_exception_handler()`
- `sam_exception_handler()` → `sam_panic()`: displays exception name, vector, error code, RIP, RSP, CR2 (for #PF)
- Panic overlay drawn on pixel framebuffer if `g_fb.ready`, always on VGA text and serial
- E820 memory map validation: multiboot2 mmap tag parsed before domain allocation; domains overlapping
  reserved/ACPI/bad memory emit `[WARN]` and are skipped rather than silently corrupting hardware state
- Before Sprint 14: any #GP or #PF → instant triple-fault, screen goes black
- After Sprint 14: red panic screen with full fault context — same experience as a Linux kernel oops

**Serial output on panic (example #GP)**:
```
============================================================
  *** SAM OS KERNEL PANIC ***
============================================================
  Exception : #GP General Protection
  Vector    : 13
  Err code  : 0x0000000000000000
  RIP       : 0x000000000010ABCD
  RSP       : 0x0000000000200FF0
  System halted.
============================================================
```

### Sprint 15 — Phase 3: Real Hardware Abstraction  ✅
**Date**: 2026-06-23
**Goal**: The kernel knows what hardware it is actually running on.

**Files added**:
| File | Purpose |
|------|---------|
| `kernel/acpi.h` | RSDP search (EBDA + BIOS ROM), RSDT/XSDT walk, MADT parse → CPU count + I/O APIC base |
| `kernel/ps2kbd.h` | 8259A PIC remap, IRQ 1 handler at vector 0x21, 64-byte ring buffer, US keymap, `ps2_wait_char()` |
| `kernel/ata.h` | ATA PIO: IDENTIFY DEVICE, LBA28 sector read, primary channel detect |

**What Sprint 15 adds**:
- **ACPI**: RSDP located in EBDA and BIOS ROM area. RSDT (ACPI 1.0) and XSDT (ACPI 2.0+) both supported. MADT parsed for enabled CPU count and I/O APIC base address. Graceful degradation: 1 CPU assumed if absent.
- **Full recursive PCI scan**: replaced the old bus-0-function-0 loop with a proper depth-first recursive scan. Checks `header type` bit 7 (multifunction) → enumerates all 8 functions. Follows PCI-PCI bridge secondary bus numbers recursively. 256-bit visited bitmap prevents loops. AHCI BAR5 (ABAR) and ATA I/O base recorded for disk drivers.
- **PS/2 keyboard IRQ driver**: 8259A master PIC remapped (IRQs 0-7 → vectors 0x20-0x27, slave → 0x28-0x2F). IRQ 1 handler installed at vector 0x21. Scancode → ASCII US keymap with shift tracking. Ring buffer. `ps2_wait_char()` blocks via `HLT` instead of a tight spin.
- **ATA PIO disk**: software reset, IDENTIFY DEVICE, model string and LBA28 sector count extracted. `ata_read_sectors(lba, count, buf)` for read-only access. No DMA, no LBA48 yet.
- **Before Sprint 15**: PS/2 keyboard polled in a tight loop (VirtualBox starvation risk). PCI scan missed multifunction devices and secondary buses. No disk access. No CPU topology.
- **After Sprint 15**: interrupt-driven keyboard, full PCI device inventory, ATA disk detected and readable, CPU count from ACPI.

---


### Sprint 20 - Phase 4: PIT Preemption + Transparent Task Resume  Done
**Date**: 2026-08-24
**Goal**: The timer, not the task, decides when it runs.

**How it works**:
- kernel/pit.h: 8254 channel 0 at 100 Hz; IRQ0 unmasked at the master PIC.
- _irq0 full-frame stub at vector 0x20: EOI + tick count; when a ring-3 task
  has consumed its quantum (3 ticks ~ 30 ms) the complete register frame is
  snapshotted into a TCB and the kernel longjmps home.
- sam_user_resume(frame, cr3) rebuilds the iretq frame and puts the task
  straight back on the CPU - mid-instruction, registers intact.
- The task cannot tell it was preempted: /loop.elf was interrupted twice and
  still printed every iteration and exited cleanly.

**Boot evidence**:
[tick] preempted, resuming task
[tick] preempted, resuming task
[PASS] Sprint 20: preempted 2 time(s), task finished cleanly
[SAM OS] Sprint 20 PASS -- preemption + task resume + ELF loader

**Honest scope**: one runnable task at a time (preemption returns it to the
same kernel await-point; no second task to switch TO yet). Round-robin between
MULTIPLE resident tasks is the natural Sprint 21.

---


### Sprint 21 - Phase 4: Round-Robin Between Two Resident Ring-3 Tasks  Done
**Date**: 2026-08-24
**Goal**: Two tasks live at once; the timer decides who runs.

**How it works**:
- Per-task user windows: task slots get separate 4 MiB regions
  (0x19000000 / 0x19500000), each with its own address space (CR3) and stack.
- The PIT tick handler is now a SCHEDULER: on quantum expiry it snapshots the
  running task into its TCB and iretqs directly into the next resident task -
  no kernel round-trip. First dispatch of a not-yet-started task synthesizes
  its initial frame (rip/cs/rflags/rsp/ss).
- Exit/fault longjmps to the kernel await-point, which counts it and resumes
  the survivor until all resident tasks are gone.
- Boot evidence (interleaved output from two independent address spaces):
  [A]1 [A]2 [B]1 [B]2 [B]3 [A]3 [A]4 [A]5 [B]4 [B]5
  [sched] task 0 ended (exit)
  [sched] task 1 ended (exit)
  [PASS] Sprint 21: two resident tasks scheduled round-robin

**Honest scope**: fixed two slots, static load addresses per slot, no
priorities, no sleep/wake, exit codes not yet surfaced to shell. Natural
Sprint 22: N-task table + argv + wait().

---

### Sprint 22 - Phase 4: argv + Exit Codes  Done
**Date**: 2026-08-24
**Goal**: Programs receive arguments and report status back.

**How it works**:
- sam_build_args() lays out a SysV-style initial stack: argument strings at
  the top, argv[] pointer array (NULL-terminated) below, argc beneath that.
  Entry RSP points at argc.
- ABI (frozen): [rsp]=argc, [rsp+8..]=argv[0..n] pointers, argv[argc]=NULL.
  User programs use `mov r15,[rsp]` / `lea r14,[rsp+8]`.
- exit(code) is recorded per task; the shell `run` command now reports
  "task exited with code N".
- echo.elf prints all its arguments and exits with argc — CI verifies the
  full round trip (4 args in, exit code 4 out).

**Honest scope**: max 8 args, single-digit-friendly printing, no environment
block, no wait()/status-retrieval between tasks yet.

---
### Sprint 16 — Phase 4: VFS + initrd + Syscalls + First Ring-3 Process  ✅


**Date**: 2026-08-23
**Goal**: Run a second piece of code that is not the kernel — the app model exists.

**Files added / modified**:
| File | Purpose |
|------|---------|
| `kernel/vfs.h` | In-memory VFS over a USTAR initrd: parse, open/read/close, exact-name lookup |
| `kernel/syscall.h` | `int 0x80` gate (DPL=3): `write(1)`, `exit(2)`, `read(3)` |
| `kernel/userasm.asm` | `_user_enter`/`_user_exit` ring-3 trampolines (iretq in, kernel-stack restore out) |
| `user/hello.asm` | **The first SAM OS user process** — flat binary at 0x19000000, prints via syscall, exits |
| `kernel/idt.h` | `idt_set_gate_dpl()`; vector 0x80 installed with DPL=3; `_syscall80` stub |
| `kernel/boot.asm` | GDT gains user code (0x1B) + user data (0x23) segments, DPL=3 |
| `kernel/grub.cfg` | New module: `/boot/initrd.tar "initrd"` |
| `Makefile` | `hello.bin` → ustar `build/initrd.tar`; new test marker |

**Syscall ABI (frozen for Sprint 16)**:
```
rax = syscall nr | rdi = arg0 | rsi = arg1 | rdx = arg2 | result in rax
  1 = write(fd, buf, len)   fd=1 only (COM1 serial), returns len
  2 = exit(code)            never returns; kernel resumes
  3 = read(fd, buf, len)    returns 0 (EOF) until stdin is wired
```

**Honest scope**: one user task; the `exit(2)` syscall verifies the ring-3
return leg (gate DPL=3 → TSS.RSP0 stack switch back to ring 0) and then halts
the kernel cleanly — resume-to-shell waits for real task switching. No page-table
isolation yet (GiB 0-1 are mapped user-accessible until per-task CR3 arrives),
flat-binary executables (not ELF), no preemption. Those are the next Phase 4 steps.

---

### Sprint 17 — Phase 4: Per-Task Page Tables (Real Ring-3 Isolation)  ✅
**Date**: 2026-08-23
**Goal**: Ring 3 must not be able to touch kernel memory. Make isolation real.

**Files added / modified**:
| File | Purpose |
|------|---------|
| `kernel/vmm.h` | Bump page-table allocator + `vmm_create_user_as()`: fresh PML4 per task; user region (2×2 MiB at 0x19000000/0x19200000) U/S=1, everything else supervisor-only |
| `kernel/syscall.h` | CR3 save/switch in `sam_user_enter()`; ring-3 `#PF` dispatcher branch reports isolation PASS |
| `kernel/userasm.asm` | `_user_halt` restores kernel CR3 before halting |
| `kernel/boot.asm` | Export `pml4_table`/`pdpt_table`/`pd_table0`/`tss`; clear CR4.PCIDE (stale-TLB hazard) |
| `kernel/idt.h` | **Critical fix**: `_isr_common` saved its frame pointer in RDI across the C call — caller-saved! Now saved on the stack. This latent bug corrupted the kernel stack whenever GCC's register allocation changed |
| `user/hello.asm` | Phase 2: deliberately reads 0x100000 → #PF → isolation proven |

**What Sprint 17 demonstrates**:
```
[ring3] Hello from the first SAM OS user process!
[ring3] write + exit syscalls OK
[ring3] Phase 2: touching kernel memory at 0x100000...
[OK] Sprint 17: ring-3 access to kernel memory faulted (#PF)
[SAM OS] Sprint 17 PASS -- per-task page tables isolate ring 3
```

**Honest scope**: one user task at a time; exit/isolation-fault halts cleanly
(resume-to-shell = real task switching, next). No demand paging, no swap,
flat-binary executables. The U/S bits are enforced by hardware on every access —
the PSL `user_cannot_store` invariant now has a hardware enforcement point.

---

### Sprint 18 — Phase 4: Task Switching (Kernel Resumes After Exit)  ✅
**Date**: 2026-08-24
**Goal**: The kernel SURVIVES its tasks. Exit/fault resumes the caller instead
of halting — the prerequisite for a shell that can run programs.

**How it works**:
- `__builtin_setjmp` in the task runner; the exit syscall and ring-3 fault
  paths record a reason (`TASK_END_EXIT` / `TASK_END_FAULT`) and
  `__builtin_longjmp` back into the kernel.
- Resumed branch restores the kernel CR3 before anything else.
- Two initrd programs prove both paths: `/guard.bin` faults by design
  (isolation held), `/hello.bin` exits cleanly (task switch worked).

**Debug war story (kept for posterity)**: resume initially landed in random
code because GCC proved `sam_user_enter` never returned (unreachable hlt tail)
and merged arbitrary blocks at the resume address; fixing that exposed a
returns-twice clobber of non-volatile locals. Both are classic setjmp pitfalls,
now documented here.

---

### Sprint 19 — Phase 4: ELF64 Loader + Shell `run` Command  ✅
**Date**: 2026-08-24
**Goal**: Real executables, runnable from the shell.

**Files added / modified**:
| File | Purpose |
|------|---------|
| `kernel/elf.h` | Minimal ELF64 loader: ET_EXEC/x86-64/PT_LOAD only; every segment must land inside the user region or it is rejected |
| `user/linker.ld` | User programs link at 0x19000000 (single LOAD via `-z noseparate-code`) |
| `user/hello.asm`, `user/guard.asm` | Rebuilt as real ELF64 binaries |
| `kernel/shell.h` | New command: `run <name>` — runs an initrd ELF in ring 3, kernel resumes after |
| `Makefile` | ELF build rules; CI marker `Sprint 19 PASS` |

**Honest scope**: static flat-memory ELFs (no dynamic linking/PIE), single task
at a time, no argv yet, no per-task scheduling (cooperative, one shot). Next:
preemption via PIT, multiple resident tasks, argv + exit codes surfaced to shell.

---

### Sprint 23 — Phase 4: Fixed-Capacity N-Task Kernel ✅
**Date**: 2026-08-25
**Goal**: Generalize the Sprint 21 two-resident-task implementation into a fixed-capacity (16-slot) preemptive kernel with explicit TCB state machine, generic creation, and a run/drain loop.

**How it works**:
- `kernel/syscall.h`: `task_state_t` (FREE/READY/RUNNING/ZOMBIE) with enforced invariants — at most one RUNNING; ZOMBIE only via exit/#PF; full-drain cleanup resets slots.
- `sam_task_create()` / `sam_task_run_loop()` replace the per-test setjmp scaffolding; PIT tick delegates to `sam_scheduler_tick()`.
- Per-create E820 validation of each task's window; first dispatch uses the argv-adjusted `init_rsp`; saved `cpu_frame_t` authoritative after preemption.

**Honest scope**: slot index served as task ID; binaries were tied to slot link addresses (5 MiB stride) because mappings were identity-like — removed in Sprint 24. VMM page-table pages leak (~12 KiB/CR3); no waitpid.

---

### Sprint 24 — Phase 4: Shared User Virtual Address Space ✅
**Date**: 2026-08-25
**Goal**: Remove the slot ↔ executable-link-address coupling. One canonical user VA layout; one ET_EXEC binary runs simultaneously in any number of slots.

**How it works**:
- Canonical layout (identical in every CR3): code VA `0x19000000–0x19200000`, stack VA `0x19200000–0x19400000`.
- Deterministic physical backing by slot: `code_pa = 0x19000000 + k·4 MiB`, `stack_pa = code_pa + 2 MiB`. Freed slots reuse their same backing — the arena does not leak across batches.
- `vmm_create_user_as_pa(code_pa, stack_pa)` replaces wholesale the two canonical PD entries (pd[200]/pd[201], U/S=1 → task-owned frames); every other GiB-0 entry stays supervisor-only identity. A live audit walks the fresh tables and prints both entries.
- `elf_load_pa()` validates that every PT_LOAD vaddr (and entry) lies in the canonical 2 MiB code region, then copies to `code_pa + (p_vaddr - USER_CODE_VA)`; BSS zeroed in backing; foreign (non-canonical) binaries rejected.
- argv: builder writes into PHYSICAL stack backing but stores VIRTUAL pointers and returns the virtual RSP (`pa2va_delta = stack_pa − USER_STACK_BASE`). Slot-0-only identity coincidence made this invisible until multi-slot runs — caught by CI as #PF on `argv[1]` dereference in slots ≥1, fixed, regression-proven by Sprint 22 marker.
- Boot-time E820 capacity discovery counts consecutive usable 4 MiB slots at the arena; runtime capacity = min(16, usable). Creation beyond capacity fails safely.

**Boot evidence**:
```
[OK] Sprint 24: task backing capacity 16/16 slots usable @0x19000000
[aud] pd[200]→0x19000000 pd[201]→0x19200000  U/S=1 [PASS]   (slot 0)
[aud] pd[200]→0x19400000 pd[201]→0x19600000  U/S=1 [PASS]   (slot 1)
[aud] task 0 va=0x19000000→pa=0x19000000 | task 1 va=0x19000000→pa=0x1940000  DIVERGENT [PASS]
[PASS] Sprint 24: 4 instances of one binary resident
[PASS] Sprint 24: 4/4 exited cleanly, N preemptions   (W/X/Y/Z interleaved)
[PASS] Sprint 24: cross-process PA read faulted (#PF), survivor ran on
[SAM OS] Sprint 24 PASS -- shared user VA, per-task physical backing
```

**Honest scope**: no waitpid/PPID/background jobs/PID allocator; no VMM reclamation (page-table bump still leaks ~12 KiB per new CR3); 2 MiB huge pages only; no COW/PIE/ASLR/execve/SMP.

---

### Sprint 25 — Phase 4: Process Identity, Parent/Child Lifecycle, Blocking waitpid ✅
**Date**: 2026-08-25
**Goal**: Introduce real process identity (PID independent of slot), parent/child
relationships on TCB metadata, a WAITING state with true blocking `waitpid`,
and unified exit/fault termination bookkeeping.

**How it works**:
- 32-bit monotonic PIDs from 1 (`SAM_PID_KERNEL`=0 reserved for the kernel;
  shell/boot-created tasks get ppid 0). Wraparound skips 0 and scans live
  TCBs; a reused slot always receives a fresh PID.
- TCB gains `pid/ppid/term_reason/wait_target/wait_status_va`; states add
  TASK_WAITING. Scheduler selection still matches only READY/RUNNING.
- Unified `sam_task_terminate()` handles exit AND fatal ring-3 faults:
  ZOMBIE + reason/code recorded, orphans re-parented to kernel, then one
  matching WAITING parent is woken — 64-bit status (reason<<32|code; fault
  code = vector) written through the Sprint-24 VA→PA ownership helper,
  saved RAX preset to child PID, child ZOMBIE→FREE only after transfer.
- Blocking waitpid switches away FROM SYSCALL CONTEXT exactly like PIT
  preemption: validated args first; successor REQUIRED before state change
  (else `-WPID_E_DEADLOCK`, caller stays RUNNING); frame snapshot into TCB;
  `sam_user_resume()` of successor. The abandoned syscall never returns.
- Immediate reap path never touches the scheduler; deterministic lowest-slot
  selection for wait-any zombies. SAM-native errors: BADPID/-1 NOTCHILD/-2
  NOCHILDREN/-3 BADPTR/-4 BADOPTS/-5 DEADLOCK/-6. options must be 0.
- Ring-3 #PF branch now prints CR2 directly — hardware evidence of the
  exact faulting linear address (Sprint 24 isolation probe shows CR2 =
  0x19600000, the slot-1 stack PA).
- Latent bug fixed en route: argc==0 programs previously received
  rsp == USER_STACK_TOP (unmapped boundary); builder now reserves scratch,
  so argv-scanning binaries can no longer fault on the boundary word.

**Boot evidence**:
```
[rel] child slot 0 pid=13 -> parent pid=14
[reap] task 1 reaped child 13 (slot 0 freed)          ← immediate reap
[W] wait OK got=13 status=0x0000000000000000
[block] task 1 WAITING (target 4294967295)            ← RUNNING→WAITING
[run] successor task 2 dispatched from syscall context
[CR2] faulting linear address: 0x0000000019600000
[wake] task 1 READY <- child 17 (fault)               ← reap-at-wake
[sched] task 2 ended (fault, already reaped)          ← idempotent await point
[W] wait OK got=17 status=0x000000010000000E          ← TERM_FAULT<<32 | 14
[W] probe not-child -> -2 OK / bad-options -> -5 OK / bad-pid -> -1 OK
[OK] Sprint 25: slot 0 reused by fresh pid 20 (max previously seen 17) [PASS]
[SAM OS] Sprint 25 PASS -- process identity + waitpid lifecycle
```

**Honest scope**: no spawn/fork/execve (CI constructs parenthood directly on
TCB metadata; validation is the same code any future creator would use);
no background jobs/signals/WNOHANG; VMM page-table leak unchanged (~12 KiB
per created address space, pool exhaustion after ~85 four-task batches);
one waiter per child (a second matching waiter stays blocked until the
batch-drain sweep frees everything).

---

### Sprint 25H — Hardening: Kernel Boundary, Lifecycle & Context ✅
**Date**: 2026-08-26
**Goal**: Close every P0/P1 from the three-audit correctness review before any userspace process creation.

**Landed**:
- **IF/orchestrator invariant**: interrupt gates clear IF; `__builtin_longjmp` does not restore flags. The await point now restores IF once (`sam_orchestrator_irq_restore`) after kernel-CR3 re-entry. Proven: post-churn check asserts IF=1 **and** PIT ticks advance in orchestrator context.
- **User-memory boundary**: `SAM_SYS_WRITE` validates fd, caps length (`SAM_WRITE_MAX`), rejects non-canonical/cross-region/overflow ranges via subtraction-form checks (`sam_uva_range_ok`). Adversarial probes (kernel ptr, boundary crossing, overflow addr, huge len, supervisor hole, zero len) all return SAM-native errors; never serial-dumps kernel memory.
- **CPL3 containment**: #DE/#DB/#BP/#OF/#BR/#UD/#NM/#TS/#NP/#SS/#GP/#PF/#MF/#AC/#XM raised at CPL 3 terminate ONLY the offending task (`TERM_FAULT`, code=vector). NMI/#DF/MCE remain fatal; CPL0 faults unchanged. CI proves vectors 0/6/13 contained with survivor + parent reap.
- **Back zeroization**: entire deterministic backing cleared before load/build; remanence canary test proves successor sees nothing.
- **Transactional create**: PD-audit failure now rolls back to FREE with unchanged accounting (deterministic injection test); single publish point FREE→READY.
- **Exact accounting**: every TCB state write goes through `sam_task_set_state()`; continuous consistency checker runs at each reap and drain.
- **ELF loader**: two-pass validate→copy; subtraction-form bounds everywhere; rejects filesz>memsz(-5), entry outside exec segment(-6), overlapping PT_LOAD(-7), wrap/geometry attacks. Six generated malformed fixtures all rejected.
- **FPU/SIMD context (the deep one)**: kernel C clobbers caller-saved XMM on ANY trap serving ring 3 — even a no-op tick. Now preserved via eager per-slot save in `_isr_common` (pre-C), same-context tail reload, and restore inside `sam_user_resume` immediately before `iretq`; virgin tasks get initialized FCW/MXCSR. FXSAVE path on qemu64; XSAVE auto-selected where OSXSAVE exists (same asm family). Dual-task preemption exchange test passes repeatedly (single-task isolation variant used during debugging).
- **Verdict integrity**: `g_exit_pid` + `g_reap_count` bind scenario verdicts to specific tasks/reaps instead of last-writer exit codes; several test binaries signal disagreement via contained #UD so batch fault counters see failures.

**Debugging war stories (kept)**:
1. `_isr_common` insertion left duplicate label+pop triples → every trap shifted RSP by 24–48 bytes → frame garbage presenting as #MF/RIP=0x3 then #DF. Found by disassembling current vs checkpoint-40efc57 stubs.
2. A multi-line C comment without `\` continuations inside the macro terminated it early; builds "succeeded" against stale objects for cycles. Lesson: verify linked binaries, not source intent.
3. The ticking timer was never dead: harness cached async-modified `g_tick_count`; global is now `volatile`.

**Boot evidence (25H battery, all PASS)**:
```
[PASS] 25H: injected audit failure rolled back cleanly
[PASS] 25H: all 6 malformed ELFs rejected
[PASS] 25H: write() boundary probes passed
[PASS] 25H: CPL3 vector {0,6,13} contained; survivor ran; parent reaped FAULT   ×3
[PASS] 25H: reused backing exposes no predecessor data
[PASS] 25H: XMM context preserved across preemptions (A+B)
[PASS] 25H post-churn: sentinels OK, IF=1, PIT live (+7 ticks), isolation re-proven
[OK]   25H VMM pool remaining 3664 KiB of 4096 KiB (12 KiB leaked per address space)
```

**Honest scope**: VMM page-table leak retained (~12 KiB/address space); waitpid remains single-waiter-per-child; argv stays bounded by producer discipline (shell line ≤79 B ⇒ ≤8 args); no spawn/fork/execve/jobs/signals; XSAVE-class path validated by construction+qemu FXSAVE coverage, AVX hardware soak deferred.

---

See [ROADMAP.md](ROADMAP.md) for the full 6-phase plan.

| Phase | Goal | Status |
|-------|------|--------|
| 1 — Truth stabilization | Docs accurate, reproducible build, `make test` | ✅ Done |
| 2 — Kernel safety | Exception handlers, panic, E820 validation | ✅ Done (Sprint 14) |
| 3 — Hardware | ACPI, PS/2 IRQ keyboard, ATA PIO disk | ✅ Done (Sprint 15); full PCI scan + USB HID remain |
| 4 — App model | VFS, initrd, syscall ABI, ring-3 process, N-task scheduling | 🔄 In progress; Sprints 16–25 done + **25H hardening** (boundary validation, CPL3 containment, transactional create, exact accounting, hardened two-pass ELF loader, FPU/SIMD context preservation, backing zeroization). Next: spawn-shaped creation (now unblocked), writable FS |
| 5 — Inference | Real model end-to-end on bare metal | Future |
| 6 — Compatibility | SAM ABI → Lua → WASM → Linux compat | Far future |

---

## Relationship to PSL

The PSL project (39 Lean 4 sprints) proved theorems about memory isolation, privilege
separation, and scheduling liveness on an abstract machine. SAM OS is the real-hardware
counterpart — same design principles, different substrate.

Current status of the connection:

| PSL Theorem | Design intent in SAM OS | Runtime enforcement today |
|-------------|--------------------------|--------------------------|
| `hypervisor_isolation` | AI and GAME domains must not overlap | `domain_alloc()` overlap check — **no hardware isolation yet** |
| `global_liveness` | One heavy domain at a time | **Sprint 8**: cooperative round-robin, one quantum per domain per tick |
| `machine_complete` | All kernel states reachable from boot | Structural goal — **not verified** |
| `user_cannot_store` | Ring 3 cannot write kernel domains | **Not implemented** — no privilege rings yet |

The domain allocator is an if-statement, not a hardware MMU boundary. Real enforcement
requires separate page table roots per domain — planned for a future sprint.

---

## License

GPL-3.0 — Free forever. Cannot be made proprietary.
