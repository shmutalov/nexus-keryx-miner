# Vulkan runtime for AMD GPUs

`nexus-live-vulkan-worker` contains no GPU code of its own. It loads a
`keryx_llama` runtime library (`src/network/opoi.cpp`) and calls it for
inference and for the PoM walk. `tools/runtime-vulkan` builds that library:
`keryx-llama-vk.dll` on Windows, `libkeryx-llama-vk.so` on Linux.

The runtime combines llama.cpp's Vulkan backend, in-process, with the PoM v4/v5
walk of keryx-miner-rdna3 (`vendor/keryx-miner-rdna3`, unmodified and pinned in
its `UPSTREAM.json`). It exports ABI 3: plain generation, which answers OPoI
challenges, and the PoM functions. The traced-inference symbols of ABI 6/8/9
are not exported, so this runtime does not serve H15 AI requests, and the Vulkan
worker declares no trace-capable model. Pools that assign H15 AI work to every
rig count that against the rig's work: Suprnova, for example, flags it
`TRACE_INCAPABLE`, and the pool is penalized for the shares it produces. Do not
mine on such pools with this runtime until traced inference is supported.

The worker re-walks every GPU winner on the host and verifies the proof with
the independent Rust verifier before submitting it. A GPU fault therefore costs
shares, never validity. A winner the host walk rejects is logged as
`AMD_POOL_HOST_REWALK_REJECTED` (pool) or `AMD_HOST_REWALK_REJECTED` (solo).

## How the walk reaches the weights

| Device | Walk | Extra VRAM |
| --- | --- | --- |
| llama.cpp enabled integer dot products (expected on RDNA2–RDNA4) | Reads llama.cpp's resident weight buffers in place | Context pad only |
| No accelerated integer dot product (expected on RDNA1) | Streams the canonical layout into a device of its own | A second copy of the weights |

A borrowed `VkDevice` cannot report which features it was created with, so
`tools/runtime-vulkan/src/device.rs` repeats ggml-vulkan's rules. The
matrix-core kernel (int8 16x16x16 cooperative matrices at wave32) runs only
where llama.cpp enabled cooperative matrices: RDNA3 and RDNA4 on AMD's drivers.
Everywhere else the walk uses the `v_dot4` kernel. Before mining from
llama.cpp's buffers, the runtime checks that the walk addresses exactly the
GGUF's canonical chunks: the count must match, and 256 sampled chunks must match
the file byte for byte.

The tier-0 model needs about 6.5 GiB for weights and 424 MiB for the H16 context
pad, plus llama.cpp's buffers. The dedicated-copy path doubles the weights, so
it does not fit 8 GiB cards.

## Validation

Hardware coverage is one device. On an RX 7900 XT (RDNA3), Windows 10 21H2 and
AMD driver 26.10.1, `cargo run --release --example selftest` produced the
following with the tier-0 GGUF:

| Path | Rate |
| --- | --- |
| Matrix cores over llama.cpp's weights | 1.69 M nonces/s |
| `v_dot4` over llama.cpp's weights (`KERYX_POM_V4_KERNEL=dot`) | 1.11 M nonces/s |
| `v_dot4` over a dedicated copy (`GGML_VK_DISABLE_INTEGER_DOT_PRODUCT=1 GGML_VK_DISABLE_COOPMAT=1`) | 1.22 M nonces/s |

- All three paths returned identical lowest winners for the same synthetic jobs.
- The resident walk addressed 203,469,888 chunks, the tier-0 anchor count.
- Filling or refilling the context pad took 0.12 s.

`nexus-vulkan-runtime-parity-test` checks the runtime against Nexus's own
consensus code (`consensus/pom_v5.cpp`). It sends synthetic H16-era jobs
through `keryx_llama_pom_mine_v5`, then re-walks every nonce up to each GPU
winner on the host. The winner must meet the target and no earlier nonce may.
It passed for all three paths: 8 jobs and 2,180 host walks each. The test uses
the GPU, so it is not part of `ctest`. Build and run it explicitly:

```sh
cmake --build <build dir> --target nexus-vulkan-runtime-parity-test
nexus-vulkan-runtime-parity-test <runtime library> <tier-0 model.gguf> [jobs]
```

A live run of `nexus-live-vulkan-worker` on Suprnova (`krx.suprnova.cc:4401`)
had 26 shares accepted and none rejected. The Rust verifier checked every proof
before submission, and the host re-walk rejected no GPU winner. The worker
completed about 1.55 M nonces/s. Suprnova flagged the rig `TRACE_INCAPABLE`, as
described above.

RDNA1 and RDNA2 have not been run on their own hardware. Their paths were
exercised only by forcing them on RDNA3.

## Build

Requirements:

- Rust 1.88+.
- `glslc` from the Vulkan SDK, found through `GLSLC`, `VULKAN_SDK` or `PATH`.
- libclang for bindgen, through `LIBCLANG_PATH`.
- CMake and Ninja for llama.cpp.

On Windows:

- Use the MSVC Rust toolchain (`+stable-x86_64-pc-windows-msvc`) and build from a
  Developer Prompt, so bindgen finds the C headers.
- Set `BINDGEN_EXTRA_CLANG_ARGS=--target=x86_64-pc-windows-msvc`.
- Keep the Cargo target directory short. llama.cpp's nested shader build
  otherwise exceeds MSVC's 260-character path limit.

```sh
CARGO_TARGET_DIR=<short dir> cargo build --release --locked \
  --manifest-path tools/runtime-vulkan/Cargo.toml
```

From CMake, `-DNEXUS_ENABLE_VULKAN_RUNTIME=ON` runs the same build and copies the
library next to the workers. `NEXUS_VULKAN_RUNTIME_TARGET_DIR` sets the Cargo
target directory.

## Run

The worker loads the official tier-0 model with its sparse tree and
`pom-tree.json` sidecar. In pool mode it needs no node and no IPFS daemon.
Submissions require the independent verifier and explicit authorization:

```sh
nexus-live-vulkan-worker --keryxd-address stratum+tcp://<pool>:<port> \
  --address keryx:<payout> --worker <name> \
  --model <model.gguf> --tree <pom-tree.bin> --meta <pom-tree.json> \
  --vulkan-lib <keryx-llama-vk library> --oracle <nexus-m21-oracle> --enable-submit
```

`scripts/nexus_launcher.py --device amd --pool-url ... --live` builds the same
command. It takes the runtime library from next to the worker unless
`--vulkan-lib` is given.

On Windows the worker's data directory defaults to
`%LOCALAPPDATA%\nexus-keryx-miner` when `NEXUS_DATA_DIR` is unset.

Environment variables:

| Variable | Effect |
| --- | --- |
| `KERYX_INFER_GPU` | Raw Vulkan device index to use. The default is the first discrete GPU. |
| `KERYX_POM_V4_KERNEL=dot` | Forces the `v_dot4` kernel. |
| `KERYX_VK_LOG` | Runtime log level on stderr. The default is `info`. |
