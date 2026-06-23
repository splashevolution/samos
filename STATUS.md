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

---

## Roadmap

See [ROADMAP.md](ROADMAP.md) for the full 6-phase plan.

| Phase | Goal | Status |
|-------|------|--------|
| 1 — Truth stabilization | Docs accurate, reproducible build, `make test` | ✅ Done |
| 2 — Kernel safety | Exception handlers, panic, E820 validation | ✅ Done (Sprint 14) |
| 3 — Hardware | ACPI, full PCI scan, ATA/AHCI disk, USB HID | Planned |
| 4 — App model | VFS, initrd, syscall ABI, ring-3 process | Planned |
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
