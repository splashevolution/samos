#!/usr/bin/env bash
# Push updated files to VAIO and rebuild + package ISO
# Usage: bash push_and_build.sh <vaio_ip>
# Reads password from env: PVM_VM_PASSWORD
# SECURITY: password is NEVER hardcoded

set -euo pipefail

VAIO_IP="${1:-192.168.29.191}"
VAIO_USER="praveen"
VAIO_REPO="~/sam_os"

if [[ -z "${PVM_VM_PASSWORD:-}" ]]; then
    echo "ERROR: PVM_VM_PASSWORD env var not set. Aborting."
    exit 1
fi

SCP() {
    sshpass -p "$PVM_VM_PASSWORD" scp -o StrictHostKeyChecking=no "$@"
}
SSH() {
    sshpass -p "$PVM_VM_PASSWORD" ssh -o StrictHostKeyChecking=no "${VAIO_USER}@${VAIO_IP}" "$@"
}

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Pushing boot_config.h and main.c to VAIO ==="
SCP "${SCRIPT_DIR}/kernel/boot_config.h" "${VAIO_USER}@${VAIO_IP}:${VAIO_REPO}/kernel/boot_config.h"
SCP "${SCRIPT_DIR}/kernel/main.c"        "${VAIO_USER}@${VAIO_IP}:${VAIO_REPO}/kernel/main.c"

echo "=== Building on VAIO ==="
SSH "cd ${VAIO_REPO} && make clean && make 2>&1 | tail -20"

echo "=== Done. Copy the ISO to Windows and boot in VirtualBox ==="
