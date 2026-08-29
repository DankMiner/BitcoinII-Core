BitcoinII v31.1.1 includes the parallel block-input prevout fetching
optimization developed for BitcoinII Core after the BitcoinII Core v31.1 release.

The feature originates from BitcoinII Core pull request #35295:

`validation: fetch block input prevouts in parallel during ConnectBlock`

It was merged into BitcoinII Core's development branch after v31.1 and is
expected to become part of a later BitcoinII Core release. BitcoinII
backports the feature into its v31.1.1 codebase.

## What It Does

When validating a block, the node must retrieve the previous transaction
outputs ("prevouts") referenced by the block's transaction inputs.

Traditionally, cache misses may require these prevouts to be retrieved from
the chainstate database sequentially. During Initial Block Download this can
cause storage latency to accumulate while `ConnectBlock` waits for individual
database reads.

Parallel prevout fetching introduces a `CoinsViewOverlay` which collects the
block's inputs before block connection and uses a worker thread pool to
prefetch required prevouts from the chainstate database.

Validation semantics are unchanged. The optimization changes how required
chainstate data is retrieved, not which transactions or blocks are considered
valid.

## Configuration

BitcoinII exposes the upstream configuration option:

prevoutfetchthreads=8

The command-line form is:

bitcoinII-d -prevoutfetchthreads=8

## Scope

Parallel prevout fetching is primarily a synchronization and block-validation
performance optimization. It does not alter BitcoinII consensus rules, block
serialization, transaction serialization, wallet behavior, or network
protocol compatibility.

BitcoinII includes this optimization ahead of the BitcoinII Core v31.1
release baseline from which BitcoinII v31.1.1 is derived.
