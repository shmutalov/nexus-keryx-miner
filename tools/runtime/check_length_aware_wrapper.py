#!/usr/bin/env python3
"""Verify the explicit local wrapper + pinned runtime-core composition.

This is not the monolithic distribution contract checked by check_runtime.py.
No model, device probe or inference is started here.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
from check_runtime import WRAPPER_EXPORTS, CANONICAL_EXPORTS, SEQUENCE_EXPORTS


def regular_hash(path: Path, expected: str):
    if path.is_symlink() or not path.is_file():
        raise ValueError("wrapper and core must be regular files")
    actual = hashlib.file_digest(path.open("rb"), "sha256").hexdigest()
    if actual != expected:
        raise ValueError("wrapper/core content differs from its explicit pin")


def symbols(path):
    text = subprocess.check_output(["nm", "-D", "--defined-only", str(path)], text=True)
    return {line.split()[-1] for line in text.splitlines() if line.split()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wrapper", type=Path, required=True)
    parser.add_argument("--wrapper-sha256", required=True)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--core-sha256", required=True)
    args = parser.parse_args()
    regular_hash(args.wrapper, args.wrapper_sha256)
    regular_hash(args.core, args.core_sha256)
    env = dict(os.environ, LC_ALL="C")
    closure = subprocess.check_output(["ldd", "-r", str(args.wrapper)], text=True, env=env)
    if "not found" in closure or "undefined symbol" in closure or "libcudart.so.13" in closure:
        raise ValueError("wrapper/core dependency closure failed")
    if f"{args.core.name} => {args.core.resolve()} " not in closure:
        raise ValueError("wrapper did not resolve the exact pinned core")
    wrapper_names = {"keryx_llama_" + name for name in WRAPPER_EXPORTS}
    wrapper_names.update("keryx_llama_" + name for name in ("seq_configure", "seq_roots", "seq_replay"))
    core_names = {"ggml_cuda_q8c_" + name for name in CANONICAL_EXPORTS}
    core_names.update("ggml_cuda_q8s_" + name for name in SEQUENCE_EXPORTS)
    if wrapper_names - symbols(args.wrapper) or core_names - symbols(args.core):
        raise ValueError("wrapper/core export ownership failed")
    runtime = c.CDLL(str(args.wrapper.resolve()))
    runtime.keryx_llama_abi.restype = c.c_int
    if runtime.keryx_llama_abi() != 9:
        raise ValueError("expected runtime ABI9")
    out = (c.c_ubyte * 16)(*([0xCC] * 16))
    n_prompt, n_fed = c.c_int(111), c.c_int(222)
    traced = runtime.keryx_llama_generate_traced_n
    traced.restype = c.c_int
    traced.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.c_void_p, c.c_int, c.c_int,
                      c.c_char_p, c.c_size_t, c.c_int, c.c_void_p, c.c_int, c.c_void_p,
                      c.c_int, c.POINTER(c.c_int), c.POINTER(c.c_int), c.c_int, c.c_int]
    for size in (5, 1 << 63):
        rc = traced(None, b"ab\0cd", size, None, 0, 0, b"x\0y", 3, 1,
                    out, len(out), None, 0, c.byref(n_prompt), c.byref(n_fed), 0, 0)
        if rc != -1 or bytes(out) != b"\xcc" * 16 or (n_prompt.value, n_fed.value) != (111, 222):
            raise ValueError("sized traced call violated null/bounds canaries")
    plain = runtime.keryx_llama_generate_n
    plain.argtypes = [c.c_void_p, c.c_char_p, c.c_size_t, c.c_int, c.c_void_p, c.c_int]
    plain.restype = c.c_int
    if plain(None, b"a\0b", 3, 1, out, len(out)) != -1 or bytes(out) != b"\xcc" * 16:
        raise ValueError("sized ordinary call violated null canary")
    print(json.dumps({"contract": "length_aware_wrapper_with_pinned_core", "application_abi": 9,
                      "wrapper_sha256": args.wrapper_sha256, "core_sha256": args.core_sha256,
                      "length_aware_exports": True, "null_bounds_checks": 3,
                      "gpu_validation": "NOT_RUN"}))


if __name__ == "__main__":
    main()
