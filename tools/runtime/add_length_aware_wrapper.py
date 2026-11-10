#!/usr/bin/env python3
"""Add bounded, length-aware generation entry points to a verified wrapper.

The upstream source stays unchanged. The generated source retains the legacy
exports and all upstream notices; only the tokenizer byte lengths differ.
"""
import argparse
import hashlib
from pathlib import Path


def transform(source: str) -> str:
    for name, traced in (("generate", False), ("generate_traced", True)):
        marker = "KERYX_EXPORT int keryx_llama_" + name + "("
        if source.count(marker) != 1:
            raise ValueError("unrecognized wrapper generation entry point")
        start = source.index(marker)
        end = source.find("KERYX_EXPORT", start + len(marker))
        if end < 0:
            raise ValueError("missing wrapper function boundary")
        body = source[start:end]
        if body.count("strlen(prompt)") != 2 or (traced and body.count("strlen(rest)") != 2):
            raise ValueError("unrecognized tokenizer byte-length contract")
        body = body.replace(marker, marker[:-1] + "_n(", 1)
        body = body.replace("const char* prompt,", "const char* prompt, size_t prompt_len,", 1)
        body = body.replace("strlen(prompt)", "prompt_len")
        if traced:
            body = body.replace("const char* rest,", "const char* rest, size_t rest_len,", 1)
            body = body.replace("strlen(rest)", "rest_len")
        at = body.index("{") + 1
        bound = "prompt_len > 60U * 1024" + (" || rest_len > 60U * 1024" if traced else "")
        body = body[:at] + "\n    if (" + bound + ") return -1;" + body[at:]
        if traced:
            legacy = '''
KERYX_EXPORT int keryx_llama_generate_traced(KeryxLlama* h, const char* prompt,
    const int32_t* prefix, int n_prefix, int head_tokens, const char* rest,
    int max_tokens, char* out, int cap, int32_t* tokens, int tokens_cap,
    int* n_prompt, int* n_fed, int min_prompt, int answer_bytes_per_position) {
    return keryx_llama_generate_traced_n(h, prompt, prompt ? strlen(prompt) : 0,
        prefix, n_prefix, head_tokens, rest, rest ? strlen(rest) : 0,
        max_tokens, out, cap, tokens, tokens_cap, n_prompt, n_fed,
        min_prompt, answer_bytes_per_position);
}
'''
        else:
            legacy = '''
KERYX_EXPORT int keryx_llama_generate(KeryxLlama* h, const char* prompt,
    int max_tokens, char* out, int cap) {
    return keryx_llama_generate_n(h, prompt, prompt ? strlen(prompt) : 0,
                                max_tokens, out, cap);
}
'''
        source = source[:start] + body + legacy + source[end:]
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    args = parser.parse_args()
    raw = args.input.read_bytes()
    if hashlib.sha256(raw).hexdigest() != args.expected_sha256:
        raise ValueError("base wrapper hash mismatch; original retained")
    if args.input.resolve() == args.output.resolve():
        raise ValueError("generated wrapper must not overwrite its upstream source")
    result = transform(raw.decode("utf-8"))
    args.output.write_text(result, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
