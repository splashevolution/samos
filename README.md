# SAM OS

**Structured Adaptive Machine Operating System**

A bare-metal x86-64 research kernel exploring CPU-only AI inference and retro game domains on old hardware — no GPU, no OS overhead.

> **Status: Research prototype, Sprint 13.** Boot → graphical OOBE wizard → interactive kernel shell. What works today is real. What comes next is clearly labelled.

---

## The Problem

1.2 billion PCs older than 7 years have no upgrade path for AI. GPU prices are out of reach. Modern OSes carry decades of general-purpose overhead that gets in the way of real-time compute.

A Core i5 with SSE4.2 can do meaningful INT8 matrix math in bare-metal mode. Nobody is harvesting that properly — especially not on machines that Windows 11 no longer supports.

---

## What Exists Today (Sprint 13)

- **Multiboot2 boot** — GRUB2 → 64-bit long mode, identity-mapped 4 GiB, SSE/FPU initialised
- **Graphical OOBE wizard** — 1024×768 pixel GUI (Bochs VBE direct I/O), dark sidebar, hostname/WiFi setup
- **Hardware scan** — RAM (E820 multiboot map), CPU brand + SIMD level (CPUID), PCI device classes, NVMe/wireless detection
- **INT8 compute** — scalar, SSE4.2, and AVX2 paths; unified dispatch; bias-correct dot product
- **Cooperative scheduler** — three task slots (AI / game / general), round-robin, no preemption yet
- **Kernel shell** — PS/2 keyboard, `help`, `info`, `setup` commands
- **STF model format** — synthetic tensor loader for AI domain; proof-of-concept, not real inference

**Not present yet:** process isolation, filesystem, storage driver, network stack, real model inference, user mode (ring 3), Android/WSL compatibility.

---

## Roadmap (6 Phases)

See [ROADMAP.md](ROADMAP.md) for the full plan. Summary:

| Phase | Goal | Status |
|-------|------|--------|
| 1 | Truth stabilization — docs, reproducible build, `make test` | 🔄 In progress |
| 2 | Kernel safety — exception handlers, panic screen, bounds checks | Planned |
| 3 | Real hardware — ACPI, full PCI scan, ATA/AHCI disk, USB HID | Planned |
| 4 | Storage + app model — VFS, initrd, syscall ABI, first ring-3 process | Planned |
| 5 | Real inference — tokenizer, transformer forward pass, next-token loop | Future |
| 6 | Compatibility — SAM ABI → Lua → WASM → JVM subset → Linux compat | Far future |

The near-term target: **bootable USB appliance for old x86-64 hardware** — interactive AI inference shell, hardware diagnostics. No install required.

---

## Build

### Prerequisites (Ubuntu/Debian/WSL2)

```bash
sudo apt update
sudo apt install -y nasm gcc binutils grub-pc-bin grub-common xorriso mtools
```

### Build the ISO

```bash
cd /mnt/d/Projects/Products/sam_os   # WSL path example
sudo rm -rf build                     # required if previous build was root-owned
make
# ISO is at build/sam_os.iso
```

### Run in VirtualBox

Attach `sam_os.iso` as optical drive. Required settings:

- RAM: 512 MB minimum
- Graphics controller: **VBoxVGA** (not VMSVGA, not VBoxSVGA — those break the Bochs VBE framebuffer)
- Video memory: 128 MB
- 3D acceleration: **disabled**
- EFI: **disabled** (BIOS boot only)

The graphical OOBE wizard requires VBoxVGA. On VMSVGA the pixel framebuffer is blank.

### Boot on real hardware

```bash
sudo dd if=build/sam_os.iso of=/dev/sdX bs=4M && sync
```

---

## SIMD / AVX2 Notes

The Makefile compiles with `-msse4.2` as the baseline. The AVX2 dispatch path in `simd.h` is gated by `#ifdef __AVX2__` — without `-mavx2` in CFLAGS, `__AVX2__` is not defined and `sam_int8_dot_avx2()` is not compiled in. At runtime, CPUID detection sets `sam_simd_level` correctly, but on an SSE4.2-only build the banner will show "AVX2" on AVX2 hardware while actually running the SSE4.2 path.

**Why not compile with `-mavx2`?** GCC with `-mavx2` is free to emit VEX-encoded SSE instructions across all SSE code, not just AVX2 intrinsics. VEX-encoded instructions require AVX support in the CPU, and VirtualBox VMs do not expose AVX by default. Enabling `-mavx2` globally caused Guru Meditation crashes. Sprint 14 will introduce a separate compile unit for the AVX2 path to fix this properly.

---

## Formal Foundation (Honest Version)

SAM OS is developed alongside the **PSL (Paninian Systems Language)** project — 39 sprints of Lean 4 formal proofs covering memory isolation, scheduler fairness, privilege enforcement, and deterministic execution on an abstract machine model.

The kernel's design goals (non-overlapping domains, cooperative scheduler with fair progress, privilege-gated memory operations) are **inspired by** the same invariants the PSL proofs establish. The connection is currently **conceptual** — runtime traceability from kernel code to Lean proof terms is a future milestone. The domain allocator prevents overlap at allocation time; it does not enforce hardware isolation (no separate page tables per domain, no ring-3 yet). The scheduler is cooperative; fairness is enforced by convention, not preemption.

Claims in this file are meant to match what the code does. If you find a gap, file an issue.

---

## License

GPL-3.0. Free forever. Cannot be made proprietary.
