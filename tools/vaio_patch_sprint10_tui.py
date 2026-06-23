#!/usr/bin/env python3
"""
Sprint 10 TUI patch — copy boot_config.h and main.c from Windows repo to VAIO
via SCP then trigger a clean build and ISO generation.

SECURITY: password read from PVM_VM_PASSWORD env var only — never hardcoded.

Run on VAIO with:
    python3 ~/sam_os/tools/vaio_patch_sprint10_tui.py
(files are already in the repo after 'git pull' or manual SCP)

Or run on Windows PowerShell to push + build remotely:
    $env:PVM_VM_PASSWORD = "your_password"
    python tools\vaio_patch_sprint10_tui.py --push 192.168.29.191
"""

import os
import sys
import subprocess
import pathlib

VAIO_USER = "praveen"
VAIO_REPO = "~/sam_os"

def run(cmd, check=True):
    print(f"$ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=False, check=check)
    return result.returncode

def push_and_build(vaio_ip: str):
    pw = os.environ.get("PVM_VM_PASSWORD")
    if not pw:
        print("ERROR: PVM_VM_PASSWORD env var not set.")
        sys.exit(1)

    repo = pathlib.Path(__file__).parent.parent
    files = [
        ("kernel/boot_config.h", f"{VAIO_REPO}/kernel/boot_config.h"),
        ("kernel/main.c",        f"{VAIO_REPO}/kernel/main.c"),
    ]

    for local_rel, remote in files:
        local = str(repo / local_rel)
        dest = f"{VAIO_USER}@{vaio_ip}:{remote}"
        rc = run(["sshpass", "-p", pw, "scp",
                  "-o", "StrictHostKeyChecking=no",
                  local, dest])
        if rc != 0:
            print(f"ERROR: SCP failed for {local}")
            sys.exit(1)
        print(f"  OK: {local_rel} -> {vaio_ip}")

    print("\n=== Building on VAIO ===")
    build_cmd = f"cd {VAIO_REPO} && make clean && make"
    run(["sshpass", "-p", pw, "ssh",
         "-o", "StrictHostKeyChecking=no",
         f"{VAIO_USER}@{vaio_ip}",
         build_cmd])

    print("\n=== SCP ISO back to Windows is manual — run: ===")
    print(f"  scp {VAIO_USER}@{vaio_ip}:{VAIO_REPO}/sam_os.iso .")

def local_build():
    """Just rebuild locally on VAIO."""
    repo = pathlib.Path.home() / "sam_os"
    os.chdir(repo)
    run(["make", "clean"])
    run(["make"])

if __name__ == "__main__":
    if "--push" in sys.argv:
        idx = sys.argv.index("--push")
        ip = sys.argv[idx + 1] if idx + 1 < len(sys.argv) else "192.168.29.191"
        push_and_build(ip)
    else:
        local_build()
