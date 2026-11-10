Nexus v0.0.5 — experimental native CLI refresh for Windows x64 and Linux x86-64
(Ubuntu 24.04 baseline), NVIDIA SM 8.6. WSL is not required for Windows.

This refresh adds length-aware NUL-safe prompt handling, native NVIDIA tier-0
pool AI response/opening recovery, truthful pool telemetry, escrow claim
reconciliation, durable submission journals and authenticated TLS checks.

Packages include the NVIDIA worker, independent verifier, explicitly selected
ABI9 inference runtime, CUDA redistributables, licenses, exact source and
checksums. ABI9 retains experimental candidate metadata. Models, trees, keys,
node/IPFS data and journals are external inputs. Desktop and AMD/Vulkan workers
are not included in these CLI archives.

Local CPU/mock and NVIDIA GPU/mock results are distinct from execution of these
exact CI archives. Windows GPU execution, public-pool AI acceptance, payment and
escrow approval remain unvalidated. KeryxPool approval must explicitly cover
Nexus; telemetry or a connection probe alone does not grant escrow access.

Unsigned experimental binaries. Source GPL-3.0-only with third-party notices.
Automatic updates remain disabled. Detailed changes and limits:
https://github.com/tonythewizard/nexus-keryx-miner/blob/v0.0.5/docs/release-0.0.5.md
