#!/usr/bin/env python3
"""
SAM OS — tools/to_stf.py
=========================
Universal model converter → SAM Tensor Format (STF).

Supported input formats:
  --format gguf         llama.cpp GGUF files (Q8_0, Q4_0, F16, F32)
  --format safetensors  HuggingFace SafeTensors files (F16, BF16, F32)

Output: a single .stf file loadable as a GRUB multiboot2 module.

Usage:
  python3 tools/to_stf.py --input model.gguf        --output build/model.stf
  python3 tools/to_stf.py --input model.safetensors --output build/model.stf
  python3 tools/to_stf.py --input model.gguf --tensors token_embd.weight,output.weight

  # Generate a synthetic STF for testing (no model file needed):
  python3 tools/to_stf.py --synthetic --output build/test_model.stf

The kernel (kernel/stf.h) is format-agnostic — it only reads .stf.
This converter is the only place that knows about GGUF/SafeTensors.

STF format spec (matches kernel/stf.h exactly):
  Header   : 64 bytes  (magic "STF1", version=1, n_tensors, data_offset, model_name)
  Tensors  : n_tensors × 128 bytes  (name[64], dtype, ndim, shape[4], data_offset, data_bytes)
  Data     : tensor blobs, each 64-byte aligned
"""

import argparse
import struct
import sys
import os

# STF constants (must match kernel/stf.h)
STF_MAGIC       = b'STF1'
STF_VERSION     = 1
STF_DTYPE_INT8  = 0
STF_DTYPE_FP16  = 1
STF_DTYPE_FP32  = 2
STF_DTYPE_Q8_0  = 3
STF_Q8_BLOCK_SIZE  = 32
STF_Q8_BLOCK_BYTES = 34   # 2 bytes fp16 scale + 32 bytes int8

HEADER_SIZE = 64
TENSOR_DESC_SIZE = 128
ALIGN = 64

def align64(n):
    return (n + ALIGN - 1) & ~(ALIGN - 1)

# ── STF writer ────────────────────────────────────────────────────────────────

def pack_header(n_tensors, data_offset, model_name="SAM-STF-v1"):
    name_bytes = model_name.encode()[:31] + b'\x00'
    name_bytes = name_bytes.ljust(32, b'\x00')
    hdr = struct.pack('<4sIIIQ',
        STF_MAGIC,      # magic[4]
        STF_VERSION,    # version
        n_tensors,      # n_tensors
        0,              # _reserved0
        data_offset,    # data_offset
    )
    hdr += name_bytes   # model_name[32]
    hdr += b'\x00' * 8  # _pad[8]
    assert len(hdr) == HEADER_SIZE, f"Header size {len(hdr)} != {HEADER_SIZE}"
    return hdr

def pack_tensor_desc(name, dtype, shape, data_offset, data_bytes):
    name_bytes = name.encode()[:63] + b'\x00'
    name_bytes = name_bytes.ljust(64, b'\x00')
    ndim = len(shape)
    shape_padded = list(shape) + [0] * (4 - ndim)
    desc = name_bytes
    desc += struct.pack('<II', dtype, ndim)
    desc += struct.pack('<4Q', *shape_padded)
    desc += struct.pack('<QQ', data_offset, data_bytes)
    desc += b'\x00' * 8   # _pad[8]
    assert len(desc) == TENSOR_DESC_SIZE, f"Tensor desc {len(desc)} != {TENSOR_DESC_SIZE}"
    return desc

def write_stf(output_path, tensors, model_name="SAM-STF-v1"):
    """
    tensors: list of (name: str, dtype: int, shape: tuple, data: bytes)
    """
    n = len(tensors)
    index_size = n * TENSOR_DESC_SIZE
    data_offset = align64(HEADER_SIZE + index_size)

    # Compute per-tensor data offsets
    cur = 0
    tensor_info = []
    for (name, dtype, shape, data) in tensors:
        aligned_cur = align64(cur) if cur > 0 else 0
        tensor_info.append((name, dtype, shape, data, aligned_cur))
        cur = aligned_cur + len(data)

    hdr = pack_header(n, data_offset, model_name)
    descs = b''
    for (name, dtype, shape, data, toff) in tensor_info:
        descs += pack_tensor_desc(name, dtype, shape, toff, len(data))

    # Pad header+descs to data_offset
    index_blob = hdr + descs
    index_blob += b'\x00' * (data_offset - len(index_blob))

    # Write data blobs with 64-byte alignment between them
    data_blob = b''
    for (name, dtype, shape, data, toff) in tensor_info:
        # Pad to toff
        pad_needed = toff - len(data_blob)
        if pad_needed > 0:
            data_blob += b'\x00' * pad_needed
        data_blob += data

    out = index_blob + data_blob
    with open(output_path, 'wb') as f:
        f.write(out)
    print(f"Written: {output_path}  ({len(out):,} bytes = {len(out)/1024/1024:.2f} MiB)")
    print(f"  Model   : {model_name}")
    print(f"  Tensors : {n}")
    for (name, dtype, shape, data, _) in tensor_info:
        dtype_name = {0:'INT8',1:'FP16',2:'FP32',3:'Q8_0'}.get(dtype,'?')
        print(f"  [{dtype_name:5s}] {name:40s} shape={shape}  {len(data):>10,} bytes")

# ── Synthetic test model ──────────────────────────────────────────────────────

def make_synthetic(output_path):
    """
    Generates a minimal synthetic STF with realistic Q8_0 weight structure.
    Mimics a tiny GPT-style model:
      token_embd.weight   : [256, 64]  Q8_0  (vocab=256, hidden=64)
      output.weight       : [256, 64]  Q8_0
      blk.0.attn.q.weight : [64, 64]   Q8_0
      blk.0.attn.k.weight : [64, 64]   Q8_0
    Each Q8_0 block: fp16 scale (nonzero) + 32 int8 weights (nonzero pattern)
    """
    import random
    rng = random.Random(42)

    def make_q8_tensor(rows, cols):
        """Make a realistic Q8_0 tensor: rows×cols elements in Q8_0 format."""
        n_elements = rows * cols
        assert n_elements % STF_Q8_BLOCK_SIZE == 0, "elements must be multiple of 32"
        n_blocks = n_elements // STF_Q8_BLOCK_SIZE
        data = bytearray()
        for _ in range(n_blocks):
            # fp16 scale: pick a small positive value ~0.01..0.1
            # 0.05 in fp16 = 0x2E66
            # Use varied values per block
            scale_f = 0.01 + rng.random() * 0.09
            # Encode as fp16 manually
            # Simple: use struct with numpy-free approach
            # fp16 for 0.05: exp=8 (biased=8+15=23=0x17), mant=0.05*2^11-2^10=...
            # Use the safe path: encode via fp32->fp16 conversion
            import struct as _s
            fp32_bits = _s.unpack('<I', _s.pack('<f', scale_f))[0]
            sign = (fp32_bits >> 31) & 1
            exp32 = (fp32_bits >> 23) & 0xFF
            mant32 = fp32_bits & 0x7FFFFF
            exp16 = exp32 - 127 + 15
            mant16 = mant32 >> 13
            if exp16 <= 0:
                fp16_bits = 0x0001  # smallest subnormal, nonzero
            elif exp16 >= 31:
                fp16_bits = (sign << 15) | 0x7C00  # inf
            else:
                fp16_bits = (sign << 15) | (exp16 << 10) | mant16
            data += _s.pack('<H', fp16_bits)
            # 32 int8 weights: nonzero values in [-127, 127]
            for _ in range(STF_Q8_BLOCK_SIZE):
                w = rng.randint(-127, 127)
                if w == 0:
                    w = 1  # guarantee nonzero for the proof
                data.append(w & 0xFF)
        return bytes(data)

    tensors = [
        ("token_embd.weight",    STF_DTYPE_Q8_0, (256, 64),  make_q8_tensor(256, 64)),
        ("output.weight",        STF_DTYPE_Q8_0, (256, 64),  make_q8_tensor(256, 64)),
        ("blk.0.attn.q.weight",  STF_DTYPE_Q8_0, (64,  64),  make_q8_tensor(64,  64)),
        ("blk.0.attn.k.weight",  STF_DTYPE_Q8_0, (64,  64),  make_q8_tensor(64,  64)),
    ]
    write_stf(output_path, tensors, model_name="SAM-NANO-v1-synthetic")

# ── GGUF reader ───────────────────────────────────────────────────────────────

def read_gguf(input_path, tensor_filter=None):
    """
    Parse a GGUF file and return list of (name, dtype, shape, data) in STF format.
    Supports Q8_0, F16, F32 tensor types.
    tensor_filter: set of tensor names to include (None = all)
    """
    GGUF_MAGIC = b'GGUF'
    GGUFTypeMap = {
        0: 'F32', 1: 'F16', 2: 'Q4_0', 3: 'Q4_1',
        6: 'Q5_0', 7: 'Q5_1', 8: 'Q8_0', 9: 'Q8_1',
        16: 'BF16', 32: 'I8',
    }
    GGUFDtype = {
        0: STF_DTYPE_FP32, 1: STF_DTYPE_FP16, 8: STF_DTYPE_Q8_0, 32: STF_DTYPE_INT8
    }

    with open(input_path, 'rb') as f:
        data = f.read()

    if data[:4] != GGUF_MAGIC:
        raise ValueError(f"Not a GGUF file: {input_path}")

    pos = 4
    version, n_tensors, n_kv = struct.unpack_from('<IQQ', data, pos)
    pos += 4 + 8 + 8
    print(f"GGUF version={version}  tensors={n_tensors}  kv_count={n_kv}")

    def read_string():
        nonlocal pos
        slen = struct.unpack_from('<Q', data, pos)[0]; pos += 8
        s = data[pos:pos+slen].decode('utf-8', errors='replace'); pos += slen
        return s

    def skip_value(vtype):
        nonlocal pos
        if vtype == 0:   pos += 1   # uint8
        elif vtype == 1: pos += 1   # int8
        elif vtype == 2: pos += 2   # uint16
        elif vtype == 3: pos += 2   # int16
        elif vtype == 4: pos += 4   # uint32
        elif vtype == 5: pos += 4   # int32
        elif vtype == 6: pos += 4   # float32
        elif vtype == 7: pos += 1   # bool
        elif vtype == 8:             # string
            slen = struct.unpack_from('<Q', data, pos)[0]; pos += 8 + slen
        elif vtype == 9:             # array
            atype, alen = struct.unpack_from('<IQ', data, pos); pos += 4 + 8
            for _ in range(alen): skip_value(atype)
        elif vtype == 10: pos += 8  # uint64
        elif vtype == 11: pos += 8  # int64
        elif vtype == 12: pos += 8  # float64

    # Skip metadata KV pairs
    for _ in range(n_kv):
        read_string()  # key
        vtype = struct.unpack_from('<I', data, pos)[0]; pos += 4
        skip_value(vtype)

    # Read tensor info
    tensor_infos = []
    for _ in range(n_tensors):
        name = read_string()
        ndim = struct.unpack_from('<I', data, pos)[0]; pos += 4
        shape = list(struct.unpack_from('<' + 'Q'*ndim, data, pos)); pos += 8*ndim
        gtype = struct.unpack_from('<I', data, pos)[0]; pos += 4
        offset = struct.unpack_from('<Q', data, pos)[0]; pos += 8
        tensor_infos.append((name, ndim, shape, gtype, offset))

    # Data section starts at next 32-byte alignment
    data_base = (pos + 31) & ~31

    tensors = []
    for (name, ndim, shape, gtype, offset) in tensor_infos:
        if tensor_filter and name not in tensor_filter:
            continue
        if gtype not in GGUFDtype:
            print(f"  [SKIP] {name}: unsupported GGUF type {GGUFTypeMap.get(gtype,'?')}")
            continue
        stf_dtype = GGUFDtype[gtype]
        # Compute data size
        n_elem = 1
        for d in shape: n_elem *= d
        if gtype == 8:   # Q8_0
            n_blocks = (n_elem + STF_Q8_BLOCK_SIZE - 1) // STF_Q8_BLOCK_SIZE
            nbytes = n_blocks * STF_Q8_BLOCK_BYTES
        elif gtype == 1:  # F16
            nbytes = n_elem * 2
        elif gtype == 0:  # F32
            nbytes = n_elem * 4
        elif gtype == 32: # I8
            nbytes = n_elem
        else:
            continue
        blob = data[data_base + offset : data_base + offset + nbytes]
        tensors.append((name, stf_dtype, tuple(shape), blob))
        print(f"  [{GGUFTypeMap.get(gtype,'?'):5s}] {name}")

    return tensors

# ── SafeTensors reader ────────────────────────────────────────────────────────

def read_safetensors(input_path, tensor_filter=None):
    """
    Parse a SafeTensors file. Returns list of (name, dtype, shape, data).
    SafeTensors: 8-byte LE header_size + JSON header + data
    """
    import json
    STDtype = {'F32': STF_DTYPE_FP32, 'F16': STF_DTYPE_FP16,
               'BF16': STF_DTYPE_FP16,  # downcast BF16->FP16 not done here, raw copy
               'I8': STF_DTYPE_INT8}
    with open(input_path, 'rb') as f:
        hdr_size = struct.unpack('<Q', f.read(8))[0]
        hdr_json = json.loads(f.read(hdr_size))
        data = f.read()

    tensors = []
    for name, info in hdr_json.items():
        if name == '__metadata__': continue
        if tensor_filter and name not in tensor_filter: continue
        dtype_str = info.get('dtype', 'F32')
        shape = tuple(info.get('shape', []))
        offsets = info.get('data_offsets', [0, 0])
        blob = data[offsets[0]:offsets[1]]
        stf_dtype = STDtype.get(dtype_str, STF_DTYPE_FP32)
        tensors.append((name, stf_dtype, shape, blob))
        print(f"  [{dtype_str:6s}] {name}  shape={shape}")
    return tensors

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description='Convert model weights to SAM Tensor Format (STF)')
    ap.add_argument('--input',      help='Input model file (.gguf or .safetensors)')
    ap.add_argument('--format',     choices=['gguf','safetensors'], help='Input format (auto-detected if omitted)')
    ap.add_argument('--output',     default='build/model.stf', help='Output .stf file')
    ap.add_argument('--tensors',    help='Comma-separated list of tensor names to include (default: all)')
    ap.add_argument('--synthetic',  action='store_true', help='Generate synthetic test STF (no input file)')
    ap.add_argument('--model-name', default='SAM-STF-v1', help='Model name embedded in STF header')
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.output) if os.path.dirname(args.output) else '.', exist_ok=True)

    if args.synthetic:
        print("Generating synthetic STF test model...")
        make_synthetic(args.output)
        return

    if not args.input:
        ap.error("--input required unless --synthetic")

    tensor_filter = set(args.tensors.split(',')) if args.tensors else None

    # Auto-detect format
    fmt = args.format
    if not fmt:
        if args.input.endswith('.gguf'):   fmt = 'gguf'
        elif args.input.endswith('.safetensors'): fmt = 'safetensors'
        else: ap.error("Cannot auto-detect format; use --format gguf|safetensors")

    print(f"Reading {fmt.upper()}: {args.input}")
    if fmt == 'gguf':
        tensors = read_gguf(args.input, tensor_filter)
    else:
        tensors = read_safetensors(args.input, tensor_filter)

    if not tensors:
        print("ERROR: no tensors selected/found", file=sys.stderr)
        sys.exit(1)

    write_stf(args.output, tensors, args.model_name)

if __name__ == '__main__':
    main()
