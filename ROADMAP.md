# SAM OS — Realistic Growth Plan

Current state: **Sprint 15 complete.** Phase 2 (IDT, panic screen, E820 validation)
and Sprint 15 hardware work (ACPI, PS/2 IRQ keyboard, ATA PIO disk) done. Kernel shell
live at `SAM>` prompt. All prior sprint claims (SIMD, STF, domain isolation,
PCI scan, VESA framebuffer) verified on VirtualBox via boot logs.

---

## Phase 1 — Truth Stabilization

**Goal:** every claim in the repo traces to a boot log, test output, or serial dump.
Nothing asserted that cannot be reproduced by someone following the build instructions.

- Fix README/STATUS to reflect actual capabilities, not aspirations
- Restore real git history; tag each sprint at its verified state
- Write reproducible build instructions for Linux and WSL (tested from a clean Ubuntu install)
- Add `make test` target that runs SIMD dot-product, matmul, STF load, and domain-sentinel checks under QEMU and exits with a pass/fail code
- Serial log capture via `make run-serial` so CI can assert on output

---

## Phase 2 — Kernel Safety Baseline

**Goal:** the kernel does not silently corrupt state or hang on bad inputs.

- CPU exception handlers for all 32 x86-64 vectors (currently missing — a GPF triple-faults)
- Panic screen: freeze, display fault vector + RIP + RSP on the framebuffer, halt
- Memory-map validation: parse multiboot2 memory map tag; refuse to allocate domains over reserved/ACPI regions
- Module bounds check: verify every multiboot2 module fits within reported RAM before accessing
- Real test harness: a tiny in-kernel `assert()` that prints to serial and calls panic on failure; wire into SIMD, STF, and domain math tests

---

## Phase 3 — Real Hardware Abstraction

**Goal:** the kernel knows what hardware it is actually running on.

- **ACPI**: parse RSDP → RSDT/XSDT → MADT (for CPU topology) and FADT (for power management base)
- **PCI**: full multifunction + secondary-bus recursive scan (not just class-0x02xx grep); read BAR0 for framebuffer and storage controllers
- **Input**: PS/2 keyboard interrupt-driven (IRQ 1) rather than polling; USB HID deferred until after XHCI driver exists
- **Storage**: ATA PIO read-only (identify + LBA28 sector read); AHCI read-only if AHCI BAR detectable; expose `disk` command in shell
- **Framebuffer**: prefer GOP (UEFI) over VESA/VBE; fall back gracefully; make shell render into pixel FB (already partially implemented)

---

## Phase 4 — Minimal Storage and App Model

**Goal:** load and run a second piece of code that is not the kernel.
This is more foundational than any compatibility layer.

- **VFS skeleton**: in-memory mount table, `open`/`read`/`close` ops, no writeback yet
- **initrd**: simple flat archive (tar or custom) loaded as a multiboot2 module; VFS mounts it as `/`
- **Package format**: define a minimal SAM executable format (ELF subset or custom); loader maps segments, sets up a stack
- **Syscall ABI**: `int 0x80` or `syscall` gate; 10–20 syscalls: `read`, `write`, `exit`, `mmap`, `yield`
- **First ring-3 process**: a 200-line C program that prints "hello" via write syscall and exits cleanly

This milestone — user space running — matters more than Android, WSL, or Java compatibility.

---

## Phase 5 — Actual Inference Milestone

**Goal:** run one real model end-to-end on bare metal. Synthetic weights prove nothing.

- Load a real quantized model (e.g. a 1M–10M parameter model in Q8_0 or Q4_K)
- Implement a tokenizer (BPE or unigram; read vocab from STF metadata or a sidecar file)
- Forward pass: token embeddings → N transformer blocks (attention + FFN) → logits
- Next-token loop: argmax or top-k sampling; print decoded tokens to shell
- Measure tokens/second via PIT-calibrated TSC; report honestly

The STF format and SIMD matmul infrastructure from Sprints 2–9 are the foundation.
The gap is the forward-pass logic and a real vocab/model file.

---

## Phase 6 — Compatibility Strategy

**Goal:** run useful software without re-implementing Linux.

The order matters. Do not skip ahead.

1. **SAM ABI stable**: freeze the syscall numbers and calling convention from Phase 4
2. **Lua**: embed a Lua 5.4 interpreter as a SAM executable; gives scripting with zero kernel changes
3. **WASM**: a minimal WASM interpreter (wasm3 or a custom one) as a SAM executable; gives a portable app sandbox
4. **JVM subset**: a tiny JVM (classfile loader + bytecode interpreter for a useful subset) if there is a clear use case
5. **Linux syscall compatibility** (long-term): requires Linux-compatible kernel ABI, filesystem semantics, signal delivery, process model, sockets, mmap with MAP_FIXED, epoll, ioctl — realistically 2–3 years of work after Phase 4 is solid. Do not plan for this before Phase 4 is done.
6. **WSL-like behavior**: depends entirely on Linux syscall compat above. Not a near-term goal.

Android/Java/WSL are not targets until Phases 4–5 are complete and the SAM ABI is stable.

---

## What This Is Not (Yet)

- Not a general-purpose OS
- Not production-safe (no MMU protection between kernel and user yet)
- Not network-capable (no NIC driver, no TCP/IP)
- Not persistent (no writable filesystem)
- Not compatible with Linux binaries

These are honest constraints. Each phase above moves one step closer to removing them.
