# Durable submission receipts

Production CUDA and Vulkan workers configure private journals for pool shares
and SOLO blocks. The transport components can omit the path in isolated mocks;
that mode provides no restart durability. The journal stores opaque input and
payload digests, states and receipt digests. It never stores a key, password,
private prompt, answer or proof body.

An intent is flushed before a wire write. A completed write without a correlated
reply remains unknown. A process restart, a pool job ID change, or a changed
payload cannot authorize another send for an already reserved input/nonce.
An explicit positive or negative RPC reply is flushed before receipt counters
change. A malformed reply supplies neither acceptance nor rejection.

SOLO chain observation records `ChainObserved` separately. Finding a block after
ACK loss does not insert a transport ACK into its history. Selected-chain depth
is an operational, reorgable observation, never finality. A pool's lost credit
receipt can remain unknown: suppressing a duplicate is not proof of credit,
payment or delivery. There is no automatic resend or invented reconciliation.

The pool reservation key binds the endpoint host/port, pre-PoW input, timestamp,
DAA and nonce. Session RPC IDs and cosmetic job IDs are excluded. This protects
submission from duplicates across restarts; it is not a durable search-range
allocator. Different pool aliases/endpoints have separate submission scopes.

The append-only binary format uses 177-byte versioned records, monotonic sequence
numbers and a SHA-256 checksum chain. File identity, ownership and private
permissions are checked. Writers coordinate across processes, with a two-second
lock bound. A partial write, failed sync, replaced file or corrupt/truncated
record prevents further sends and preserves the original evidence. Checksums
provide corruption detection, not authentication against the file's owner.

Capacity is bounded at 1,000,000 records and 250,000 reserved identities. Exhaustion
fails closed; it does not discard old deduplication history. Do not clear or
rotate the journal to bypass this limit. Long-running compaction/archival needs
its own protocol that preserves the lookup of historical reservations.
Outstanding pool RPC metadata is also bounded at 1,024 shares.

Acceptance evidence distinguishes these cases:

- CPU tests exercise process/thread reservation races, ACK loss, restart,
  receipt correlation, partial writes, sync failure and corrupt-file retention.
- A loopback TCP pool reads a complete submit, drops its ACK, reconnects with a
  different job ID, and receives no second submit.
- A mock gRPC node tests ACK/rejection, lost/wrong-ID replies and later read-only
  chain observation, without a fabricated ACK.
- Transport fixtures use synthetic proofs; they do not certify consensus or
  acceptance by a real pool/node. GPU proof validation remains the worker's
  independent Rust gate. Windows execution and its fault matrix remain untested.
