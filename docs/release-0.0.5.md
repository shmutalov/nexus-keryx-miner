# Nexus v0.0.5

This release introduces native Dual Independent Multi-GPU mining, Hardfork 16 (H16) consensus readiness, the Stratum v4 multipool protocol with verifiable inference and audit openings, upstream 2-decimal power efficiency telemetry, and desktop UI v0.0.5 with responsive table parity.

The Windows x64 and Linux x86-64 downloads are unsigned experimental CLI packages for NVIDIA RTX 30/40/50 (SM 8.6, 8.9 and 12.0) and AMD Vulkan 1.3 devices, with external models, trees and keys.

## Major Features in v0.0.5

### 1. Dual Independent Multi-GPU Architecture
- **Independent Mining Lanes:** NVIDIA RTX (CUDA SM 8.6, 8.9, 12.0) and AMD Radeon (Vulkan 1.3 / BDA / Polaris & RDNA3) run as completely independent mining lanes.
- **Zero Cross-Interference:** Device workloads, VRAM staging, and inference queues operate without contention; disabling or toggling one GPU does not interrupt or restart the other.
- **Granular Admission Controls:** Dynamic workload admission allows instant switching between execution backends.

### 2. Hardfork 16 (H16) Consensus Readiness
- **Mainnet Activation:** Pre-configured for H16 activation at DAA `147_000_000`.
- **Dynamic Context Pad Sizing:** Implements the -204,800 context pad tile reduction per tier upon H16 activation.
- **Context Headroom:** Dynamic context laddering with safety margins prevents VRAM saturation and Out-Of-Memory crashes on 8 GB cards.

### 3. Stratum v4 Multipool & Protocol v0.6.0 Upgrade
- **Multipool Failover:** Native Stratum v4 client with automatic zero-stale failover between pools (Suprnova, KeryxPool, and custom stratum endpoints).
- **Standalone Pool Mining:** Operates in pool mode with zero local node or IPFS daemon requirements.
- **Stratum Verifiable Inference & Audit Openings:** Fully compatible with upstream v0.6.0 (`d434fdb`):
  - `mining.ai_request`: Dispatches inference tasks to workers with verifiable tracing.
  - `mining.ai_response`: Reports results alongside cryptographic trace commitments (`trace_commitment_hex`).
  - `mining.ai_draw`: Handles 7-parameter cryptographic audit draws relayed by pools.
  - `mining.ai_opening`: Assembles and returns audit opening proofs (`PIECE_TRACE | PIECE_ATTN | PIECE_RECUR | PIECE_RESID`) to prevent slashing and validate PoM shares.
  - Standard JSON-RPC error handling with `ErrorCode::Busy (46)` and `ErrorCode::UnknownResponse (41)`.

### 4. 2-Decimal Power Efficiency & Telemetry
- **Standardized Efficiency Metrics:** Integrated upstream format (`fc9f315d`) with 2-decimal precision across `kH/W`, `MH/W`, and `H/W`.
- **Fleet & Device Telemetry:** Real-time reporting of individual GPU efficiency and consolidated rig power draw.

### 5. Tauri 2 Desktop UI v0.0.5
- **Layout Parity:** Full visual and functional consistency across Single GPU, Dual GPU, and Rig Fleet displays in both Solo and Pool modes.
- **Responsive 4-Column Grid:** Clean presentation with unblocked vertical scrolling (`overflow-y: auto !important`).

---

## Upstream Acknowledgments & Citations

Nexus v0.0.5 incorporates and builds upon protocols, specifications, and reference implementations developed by the following upstream projects:

- **[Keryx-Labs/keryx-miner](https://github.com/Keryx-Labs/keryx-miner):**
  - Stratum v0.6.0 protocol, verifiable inference tracing, and audit opening proofs (commit `d434fdb` by Slash).
  - High-precision 2-decimal power efficiency calculation and reporting (commit `fc9f315d`).
  - Dynamic context laddering and GPU memory headroom optimizations (commit `126dac97`).
  - Multi-rig request load distribution and sibling ranking (commits `ae8e38eb`, `5b4b88b1`, `b0d9d18e`).
- **[Keryx-Labs/keryx-node](https://github.com/Keryx-Labs/keryx-node):**
  - Hardfork 16 DAA activation rules and PoM consensus specifications (release `v1.6.7-PoM`, commit `bd2f35a7`).
- **Stratum Pool Ecosystem:**
  - Community pool operators and testing infrastructure provided by **[Suprnova](https://suprnova.cc)** and **[KeryxPool](https://keryxpool.com)**.
