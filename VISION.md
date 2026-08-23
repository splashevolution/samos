# SAM OS — Vision & Problem Statement

## The Problem

There are operating systems for gaming — PlayStation's Orbis OS, SteamOS — but they
run on **vendor-locked devices**:

- Devices are destined to stop receiving updates after a few years.
- OEMs force users to purchase new hardware every year.
- Hardware prices are skyrocketing because AI demand consumes the supply of GPUs
  and modern silicon.
- Millions of capable x86-64 machines sit stale around the world — abandoned not
  because they are broken, but because their software stopped supporting them.

And since the arrival of AI, nobody has built an operating system whose native
purpose is **AI inference** — lightweight, where software works in harmony with
hardware instead of fighting it.

## The Idea

**SAM OS (Structured Adaptive Machine Operating System)** is a bare-metal x86-64
kernel that turns stale hardware into an AI inference and gaming appliance:

1. **No GPU required.** INT8 matrix compute via SSE4.2/AVX2 — the CPU is the
   inference engine.
2. **No OS overhead.** Bare metal: the kernel is the scheduler, memory planner,
   and compute dispatcher. Nothing between the model and the silicon.
3. **Hardware-software harmony.** Memory domains, scheduler quanta, and compute
   paths are designed around AI and real-time game workloads from the start —
   not patched on afterwards.
4. **Revive, don't replace.** One bootable USB turns a 2008+ (SSE4.2) machine
   into a useful device again.

It follows the same design discipline as the companion project
[Paninian Systems Language (PSL)](https://github.com/splashevolution/paninian-systems-language) —
formally reasoned system properties (memory isolation, scheduler liveness,
privilege enforcement) proven in Lean 4 across 39 sprints, now being grounded in
real constrained hardware.

## Sovereignty: Make in India

The Government of India has called for a **Make-in-India operating system with no
dependency on foreign players**. SAM OS is aligned with that goal:

| Layer | Foreign dependency today | SAM OS answer |
|-------|--------------------------|---------------|
| Kernel | Windows / Linux distros | Original kernel, GPL-3.0, developed in India |
| Boot | GRUB2 (GPL, free) — replaceable | Multiboot2 spec; custom bootloader planned |
| Toolchain | GCC/NASM (free software) | Acceptable; cross-compiler self-hosting is long-term |
| AI stack | Closed cloud APIs / vendor SDKs | On-device INT8 inference; open STF tensor format |
| App runtime | Vendor app stores | Open SAM ABI + Lua/WASM sandbox (Phase 6 roadmap) |

Everything in this repository is original work licensed GPL-3.0 — free forever,
cannot be made proprietary, and can be audited, forked, and shipped by anyone,
including Indian public institutions, without permission from any foreign vendor.

## What Success Looks Like

- A student in a tier-3 town flashes SAM OS onto a 10-year-old laptop and runs a
  local AI assistant — no internet, no GPU, no subscription.
- Government/school labs run standardized SAM OS images on hardware that would
  otherwise be scrapped.
- The full stack — kernel, drivers, scheduler, inference engine — is documented,
  formally reasoned about (PSL), and reproducible from source by any citizen.

See [ROADMAP.md](ROADMAP.md) for how we get there, phase by phase.
