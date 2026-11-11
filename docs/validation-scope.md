# Validation scope

This source is experimental. An implemented function, a CPU test, GPU execution,
a node/pool acknowledgement and chain observation are separate evidence classes.
Testing one source/binary/runtime combination does not certify another archive.
No sustained rate or relative performance claim follows from synthetic fixtures.

| Area | Source behavior | Remaining acceptance |
| --- | --- | --- |
| TLS | CA and DNS/IP identity checks enabled by default; no automatic downgrade | Platform trust-store and Windows execution |
| TLS lifetime | Reader retains its connection; SSL operations serialized; stop interrupts and joins | ThreadSanitizer on supported configurations |
| H15 restore scheduling | Validated page prefixes commit before local RPC budget yields; only complete coverage permits replay; reorg/node failures retain failure backoff | Sustained live pause/resume latency on the new artifact |
| Pool with local node | Node block ingress starts alongside Stratum when the node is admitted; real worker shadow startup and subscription regression verified locally | Newly assigned live AI duty during pool mining; native NVIDIA nodeless restart/opening is covered by the isolated GPU pool test |
| AI request recovery | H15 request windows/closure queried from the node; matching missed-duty receipt can close an ambiguous duty while preserving txid/history; absence remains unknown | Live acceptance of a newly assigned AI request and broader node-version interoperability |
| Integer fields | Exact decimal u64 strings; JSON numbers limited to safe integer range | Wider protocol fuzzing |
| SOLO observation | Timeout without evidence preserves UNKNOWN; observer tasks bounded and joined | Restart/reorg and full transport/chain state separation |
| Trace upload | HTTPS, destination admission, actual socket address filtering, no redirects/proxy; bounded synchronous receipt | Public endpoint, interrupted request and deadline interoperability |
| Proof policy | CUDA v5 and Vulkan verify with independent Rust before sending; counters follow verification | Exact worker acceptance with real model, H16 and all supported tiers |
| Nonce intervals | Truncated tails, checked winner offsets, explicit exhaustion; completed stale Vulkan batches consumed | Allocation across devices, reconnects and restart |
| Submission recovery | Fsynced intent/receipt journal; restart suppresses uncertain resends; SOLO chain observation remains distinct from RPC ACK; CPU and TCP ACK-loss fixtures | Live pool alias/failover reconciliation, archival/capacity policy and Windows fault execution |
| Suprnova H16 login | Explicit compatibility token restricted to published Suprnova endpoints and an opted-in NVIDIA worker; native identity retained in hello; real shares and metadata acknowledged locally | Public-pool AI completion, payments, broader hardware and execution of each CI archive |
| Pool difficulty | Active job retains its notify-bound target; pending difficulty applies to the next notify; mismatched candidates are dropped before nonce dedup | Live pool acceptance and broader protocol interoperability |
| CUDA route | Completed-batch strategy/reason counters; optional engine-owned canonical stream, validated for order/context/bytes; host tensors fill existing slots | Controlled performance comparison, AI trace acceptance and VRAM/deadline stability |
| Hardware | Capability gates and separate worker implementations | Each device/driver/backend combination needs its own parity and stability evidence |

## Pool AI method matrix

| Method | Current evidence boundary |
| --- | --- |
| `mining.ai_request` | Native NVIDIA tier-0 H15 request parser, exact NUL bytes and bounded worker dispatch; actual worker/GPU exercised against an isolated pool |
| `mining.ai_response` | Traced answer and durable capture before sending; duplicate/restart do not regenerate; mock pool receipt is separate from share accounting |
| `mining.ai_draw` | Typed pool dispatcher/relay and end-to-end acceptance remain open |
| `mining.ai_opening` | Persisted native capture replay with Rust proof gates; response identity bound before returning; actual worker restart/opening exercised against a mock pool |
| `mining.ai_opening_result` | Unsigned verified bundles/attention phases; bounded queue/record retention and pool/worker isolation; public pool acceptance remains unobserved |

See [native pool compatibility](pool-compatibility.md) for the exact supported
contract and optional extensions. Pool support must not be described as fully compliant until those flows pass
against a faithful simulator and the actual bridge. Mock responses are not live
acceptance. ACK is not chain confirmation or payment. Missing metrics are N/A.

## Promotion gates

Before promotion, retain exact source, worker, kernel, oracle, runtime ABI and
model identities. Require applicable negative protocol tests, full proof parity,
exclusive device validation, AI preemption/recovery, rollback and privacy gates.
The outstanding nonce/share/AI protocol items above block production promotion.
Publication and installation are separate actions from a source build.
