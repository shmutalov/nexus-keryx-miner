# Nexus v0.0.5

Experimental source and unsigned native CLI packages for Windows x64 and Linux
x86-64. This refresh adds AI recovery, escrow claim handling, pool protocol
compatibility and durable submission tracking to the H16 source baseline.

## Changes in this refresh

- AI request admission checks canonical assignment and deadlines before inference.
  H15 restoration commits validated page progress across local RPC budgets;
  ambiguous duties retain their journals until chain evidence resolves them.
- Length-aware inference entry points preserve embedded NUL bytes in prompts.
  Legacy runtime paths reject unsupported input rather than silently truncate it.
  The matching ABI9 runtime is rebuilt with the wrapper transformation.
- Native NVIDIA tier-0 pool AI answers and audit openings use durable traces,
  bounded dispatch and Rust proof checks. Duplicate requests reuse the captured
  answer; restart and nodeless recovery are covered by local GPU/mock tests.
- Pool metadata and periodic telemetry identify Nexus truthfully. Telemetry and
  AI acknowledgements remain separate from share receipts. The UI connection
  probe checks subscribe/hello and TLS identity without authorizing a wallet.
- Escrow claim policy reconciles current spendability and maturity, while
  retaining ownership, signing and duplicate-spend checks.
- Fsynced submission intent/receipt journals preserve uncertain sends across
  restart. SOLO chain observation remains distinct from an RPC acknowledgement.
  Stratum targets stay bound to the job that introduced them.
- TLS authentication, trace upload destination checks, bounded observer tasks,
  nonce interval checks and CUDA canonical-layout admission are tightened.

The source retains separate CUDA and Vulkan workers and the desktop interface.
The downloadable CLI archives include the NVIDIA worker, independent verifier,
matching ABI9 inference runtime, CUDA dependencies, licenses and checksums.
They do not include the desktop, AMD/Vulkan worker, models, trees, keys or node.

## Validation and limits

CPU/mock tests cover persistence, deadline policy, TLS, pool dispatch and escrow
claim reconciliation. Local NVIDIA tier-0 GPU/mock qualification covers traced
responses, embedded NUL prompts, worker restart, nodeless opening recovery and
same-operator pool aliases. Those results do not certify GPU execution of the
Windows/Linux CI archives or public-pool payment/escrow acceptance.

KeryxPool escrow requires explicit operator approval covering Nexus. A successful
connection test or telemetry frame does not grant eligibility. Optional Supr
cross-rig custody/import and legacy untraced pool chat are not implemented.
Live pool alias/failover reconciliation, durable nonce allocation across devices
and restart, broader hardware tiers and controlled performance comparisons remain
open. No production-readiness or relative-hashrate guarantee is made.

See [validation scope](validation-scope.md), [pool compatibility](pool-compatibility.md)
and [durable submissions](durable-submissions.md) for precise boundaries.

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
