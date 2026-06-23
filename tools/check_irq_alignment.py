#!/usr/bin/env python3
"""
SAM OS — tools/check_irq_alignment.py
======================================
Verifies that the IRQ stub in kernel/game.h (SAM_IRQ_STUB macro) maintains
16-byte RSP alignment at the point of the C handler CALL instruction.

Run this any time you change the number of pushes, the sub $128, or the
padding strategy in the IRQ stub. If it prints FAIL, fix the stub before
rebuilding — a misaligned call can corrupt MXCSR or crash the handler on
real hardware when SSE instructions are involved.

SysV AMD64 ABI requirement:
  RSP must be 16-byte aligned at the `call` instruction.
  i.e. (RSP % 16) == 0 at the point of call.

x86-64 interrupt entry state:
  The CPU pushes 5 qwords before giving control to our stub:
    SS, RSP, RFLAGS, CS, RIP  (40 bytes)
  So RSP at stub entry = (pre-interrupt aligned RSP) - 40
  40 mod 16 = 8  →  RSP is misaligned by 8 bytes at stub entry.

Our stub then:
  1. Pushes 9 caller-saved GPRs  (9 × 8 = 72 bytes)
  2. Executes  sub $128, %rsp    (128 bytes, for XMM0-XMM7)
  3. Executes  call handler_fn

Total bytes pushed/subtracted by stub before call: 72 + 128 = 200 bytes.
"""

# ── Configurable stub parameters ─────────────────────────────────────────────
CPU_PUSH_BYTES  = 5 * 8       # CPU pushes on interrupt: SS, RSP, RFLAGS, CS, RIP
GPR_PUSH_BYTES  = 9 * 8       # stub pushes: rax rcx rdx rsi rdi r8 r9 r10 r11
XMM_SUB_BYTES   = 8 * 16      # sub $128 for XMM0-XMM7
PADDING_BYTES   = 0           # explicit alignment pad qword (0 = none)
# ─────────────────────────────────────────────────────────────────────────────

def check_alignment():
    rsp = 0  # assume pre-interrupt RSP was 16-byte aligned (= 0 mod 16)

    rsp -= CPU_PUSH_BYTES
    print(f"After CPU entry pushes  ({CPU_PUSH_BYTES:3d} bytes): RSP mod 16 = {rsp % 16}")

    rsp -= GPR_PUSH_BYTES
    print(f"After {GPR_PUSH_BYTES//8} GPR pushes       ({GPR_PUSH_BYTES:3d} bytes): RSP mod 16 = {rsp % 16}")

    if PADDING_BYTES:
        rsp -= PADDING_BYTES
        print(f"After alignment pad     ({PADDING_BYTES:3d} bytes): RSP mod 16 = {rsp % 16}")

    rsp -= XMM_SUB_BYTES
    print(f"After sub ${XMM_SUB_BYTES} (XMM)   ({XMM_SUB_BYTES:3d} bytes): RSP mod 16 = {rsp % 16}")

    print()
    stub_overhead = GPR_PUSH_BYTES + PADDING_BYTES + XMM_SUB_BYTES
    print(f"Stub overhead before call: {stub_overhead} bytes ({stub_overhead//8} qwords)")

    if rsp % 16 == 0:
        print("PASS: RSP is 16-byte aligned at CALL instruction.")
        return True
    else:
        misalign = rsp % 16
        needed_pad = 16 - misalign
        print(f"FAIL: RSP misaligned by {misalign} bytes at CALL.")
        print(f"      Add {needed_pad} bytes of padding (e.g. 'sub ${needed_pad}, %rsp' before sub $128).")
        print(f"      Then set PADDING_BYTES = {needed_pad} in this script and re-run.")
        return False

if __name__ == "__main__":
    print("SAM OS IRQ stub alignment check")
    print("=" * 40)
    ok = check_alignment()
    raise SystemExit(0 if ok else 1)
