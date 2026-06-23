#!/usr/bin/env python3
"""
make_gguf_header.py — Generate a minimal synthetic GGUF file for SAM OS Sprint 6.

Writes a valid GGUF v3 header with:
  - Magic:          "GGUF" (4 bytes, uint8[4])
  - Version:        3      (uint32 LE)
  - Tensor count:   2      (uint64 LE)
  - Metadata count: 3      (uint64 LE)

Followed by 3 metadata key-value pairs (string key, string value):
  - "general.architecture" = "qwen2"
  - "general.name"         = "SAM-OS-Test"
  - "general.description"  = "SAM OS bare-metal GGUF parse proof"

Followed by 2 tensor info entries:
  - "token_embd.weight"  shape=[512, 1024]  type=Q8_0  offset=0
  - "output_norm.weight" shape=[1024]        type=F32   offset=524288

This is NOT a runnable model — it is a structural proof that the SAM OS kernel
can locate and parse GGUF weight metadata on bare metal, with no libc, no OS,
no filesystem driver.

Output: build/test.gguf (~300 bytes)
Usage:  python3 tools/make_gguf_header.py
"""

import struct
import os

# ── GGUF type constants ──────────────────────────────────────
GGUF_TYPE_UINT8   = 0
GGUF_TYPE_INT8    = 1
GGUF_TYPE_UINT16  = 2
GGUF_TYPE_INT16   = 3
GGUF_TYPE_UINT32  = 4
GGUF_TYPE_INT32   = 5
GGUF_TYPE_FLOAT32 = 6
GGUF_TYPE_BOOL    = 7
GGUF_TYPE_STRING  = 8
GGUF_TYPE_ARRAY   = 9
GGUF_TYPE_UINT64  = 10
GGUF_TYPE_INT64   = 11
GGUF_TYPE_FLOAT64 = 12

# ── GGML tensor type constants ───────────────────────────────
GGML_TYPE_F32  = 0
GGML_TYPE_Q8_0 = 8

def gguf_string(s):
    """Encode a GGUF string: uint64 length + UTF-8 bytes (no null terminator)."""
    encoded = s.encode('utf-8')
    return struct.pack('<Q', len(encoded)) + encoded

def gguf_kv_string(key, value):
    """Encode a GGUF metadata key-value pair: key string + type(STRING) + value string."""
    return gguf_string(key) + struct.pack('<I', GGUF_TYPE_STRING) + gguf_string(value)

def make_gguf():
    buf = bytearray()

    # ── Header ───────────────────────────────────────────────
    buf += b'GGUF'                          # magic (4 bytes)
    buf += struct.pack('<I', 3)             # version = 3 (uint32)
    buf += struct.pack('<Q', 2)             # tensor_count = 2 (uint64)
    buf += struct.pack('<Q', 3)             # metadata_kv_count = 3 (uint64)

    # ── Metadata KV pairs ────────────────────────────────────
    buf += gguf_kv_string("general.architecture", "qwen2")
    buf += gguf_kv_string("general.name",         "SAM-OS-Test")
    buf += gguf_kv_string("general.description",  "SAM OS bare-metal GGUF parse proof")

    # ── Tensor info entries ──────────────────────────────────
    # Tensor 0: token_embd.weight — shape [512, 1024], Q8_0, offset 0
    buf += gguf_string("token_embd.weight")
    buf += struct.pack('<I', 2)             # n_dims = 2
    buf += struct.pack('<Q', 512)           # dim[0]
    buf += struct.pack('<Q', 1024)          # dim[1]
    buf += struct.pack('<I', GGML_TYPE_Q8_0)
    buf += struct.pack('<Q', 0)             # offset in data section

    # Tensor 1: output_norm.weight — shape [1024], F32, offset 524288
    buf += gguf_string("output_norm.weight")
    buf += struct.pack('<I', 1)             # n_dims = 1
    buf += struct.pack('<Q', 1024)          # dim[0]
    buf += struct.pack('<I', GGML_TYPE_F32)
    buf += struct.pack('<Q', 524288)        # offset (512*1024 bytes)

    return bytes(buf)

if __name__ == '__main__':
    os.makedirs('build', exist_ok=True)
    data = make_gguf()
    path = 'build/test.gguf'
    with open(path, 'wb') as f:
        f.write(data)
    print(f"Written: {path}  ({len(data)} bytes)")
    print(f"  Magic:          {data[0:4]}")
    print(f"  Version:        {struct.unpack_from('<I', data, 4)[0]}")
    print(f"  Tensor count:   {struct.unpack_from('<Q', data, 8)[0]}")
    print(f"  Metadata count: {struct.unpack_from('<Q', data, 16)[0]}")
    print("SAM OS Sprint 6: copy build/test.gguf to the ISO and load via multiboot2 module.")
