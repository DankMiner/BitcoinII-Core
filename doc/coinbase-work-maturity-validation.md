# Prototype validation — updated 2026-09-24 UTC

Copyright (c) 2026 1Miner.net. See the
[maturity license](../LICENSE-1MINER-BC2-MATURITY.md) and its prior-grant exception.

This is a local, tested prototype of the 4,200–12,960-block proposal. Mainnet,
testnet, signet, and ordinary regtest retain the existing maturity rule. The
experimental regtest rule requires both explicit options. The draft mainnet work
threshold now targets age 8,580 at the hardest sustained legacy epoch difficulty;
activation remains disabled. See the [baseline note](coinbase-work-maturity-baseline.md).

For the subsequent 33 hashrate scenarios, expanded arithmetic checks, and wallet
regression fix, see the [September 24 follow-up](coinbase-work-maturity-hashrate-tests.md).

## Source

Based on the supplied `BitcoinII-Core-main.zip` (version 31.1.0 in CMake).

Archive SHA-256:

```text
ca21ee97044a6e089b8fe755fb538144a9d0d42c4210aa497467fa946b822b76
```

The patch changes maturity validation, mempool reorganization handling, wallet
eligibility, RPC/GUI reporting, and their tests. It does not change the ShockWave
difficulty-adjustment implementation or the block/UTXO serialization format.

## Build and checks

Built natively in Ubuntu 24.04 under WSL using GCC 13.3.0, CMake 3.28.3, Ninja,
SQLite 3.45.1, and Qt 6.4.2. Debug builds enabled lock-order assertions and RPC
result-schema checking. These are Linux build results; a Windows binary has not
been built or tested.

Configuration used an out-of-source build:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DENABLE_IPC=OFF \
  -DBUILD_GUI=ON -DBUILD_TESTS=ON -DBUILD_FUZZ_BINARY=ON \
  -DBUILD_BENCH=OFF -DWITH_USDT=OFF
cmake --build build -j 6
```

The daemon, CLI, wallet tool, GUI, unit-test executables, and fuzz executable
built successfully. The changed mempool benchmark source also compiled after
enabling `BUILD_BENCH`; the benchmark was not run. No fuzz campaign was run.

`git diff --check` passed.

After selecting the hardest-epoch baseline, the daemon and unit-test executable
were rebuilt successfully. Mainnet now contains the draft work constant while
its activation height remains disabled. A new test uses the real historical
target and checks the first spendable ages at half, equal, double, and triple
the reference work per block.

## Passing tests

Eight focused CTest suites passed again after the baseline update (19.95 seconds
with two suites running in parallel; the initial run took 46 seconds):

- `coinbase_maturity_tests` — seven new cases covering age/work boundaries,
  grandfathering, branch ancestry, excluded work, monotonicity, invalid inputs,
  regtest-only configuration, and the exact hardest-epoch midpoint calibration.
- `wallet_tests` — includes new tests for work-based maturity at a wallet's
  processed tip, rollback, missing/wrong-branch metadata, legacy behavior, and
  wallets without a chain interface.
- `transaction_tests`
- `mempool_tests`
- `validation_block_tests`
- `chain_tests`
- `arith_uint256_tests`
- `argsman_tests`

The Qt test suite also passed using `QT_QPA_PLATFORM=offscreen` (15 seconds).
That GUI run and the functional runs below were completed before the baseline
constant was selected; they were not repeated for this disabled mainnet parameter
change. The focused suites above were rerun against the updated source.

Functional results:

| Test | Result | Coverage |
| --- | --- | --- |
| `feature_coinbase_work_maturity.py` | Passed, about 258 seconds | Three isolated regtest chains, more than 21,000 blocks; actual 4,200 and 12,960 limits; work threshold; old rewards; wallet balances and RPC status; premature mempool/block rejection; reorganization removal including descendants; chainstate rebuild. |
| `mempool_reorg.py` | Passed, 7 seconds | Existing reorganization behavior with the new rule disabled. |
| `feature_block.py` | Passed, 181 seconds | Existing block-validation behavior with the new rule disabled. |

The maturity functional test's first run exceeded a 30-second RPC timeout while
generating a batch of 1,000 blocks during a concurrent Debug build. Its final
version uses batches of 100 and a 120-second RPC timeout; the complete rerun
passed. The consensus limits were not reduced for testing.

To reproduce the dedicated functional test after building:

```sh
python3 build/test/functional/test_runner.py feature_coinbase_work_maturity.py
```

## Continued local checks

The eight focused unit suites and the dedicated maturity functional test were
rerun against the final feature source in commit
`3dbafb568d73458406dd8b821c4815648c2e4233`; both passed (21.61 seconds and
169 seconds, respectively). The earlier Qt and legacy functional results above
remain separate runs.

Further checks completed on September 23 local time (September 24 UTC) using
two disconnected regtest nodes and the real 4,200/12,960 age limits. One used
`W=17,158`, which places the midpoint at age 8,580 with regtest work. The other
used the exact proposed mainnet work constant and reached the maximum-age path.

Both nodes passed these checks:

- Disconnect a confirmed reward spend and return it and its child to the mempool.
- Roll back below maturity and evict both transactions.
- Restart while immature and preserve wallet status and premature-spend rejection.
- Restore the chain and the confirmed spend.
- Create a fee-only reward containing 0.001 BC2 with zero credited work at birth.
- Add 500 blocks, credit exactly 1,000 subsequent work units, and keep that reward
  immature at age 501.
- Rebuild chainstate from stored blocks, preserve the chain tip, work, wallet
  maturity, and confirmations, and pass `verifychain 4 0`.

A separate copy of the midpoint node also passed an actual pruning test. Blocks
through height 8,580 were removed; the height-2 reward's block body was unavailable
while its header remained. Its unspent output retained maturity with 17,736 work
against the 17,158 requirement, including after restart. The signed spend was
accepted and confirmed at height 8,871, and survived a second restart. This covers
one pruned-history spending and restart scenario, not every pruning or recovery
failure mode.

The first fee-only test script incorrectly expected the top-level
`gettransaction.amount` to include immature credit. Checking the decoded output
instead confirmed the correct 0.001 BC2 amount; the complete sequence was rerun
successfully. No node code change was required.

These checks exercise local node behavior. They do not simulate production
hashpower or establish resistance to rented-hash attacks.

## Wallet test failure and subsequent fix

`wallet_basic.py` failed at its `sendall` call (line 488), with a Debug lock
assertion: `BlockUntilSyncedToCurrentChain()` was called while `cs_wallet` was
already held.

At the time of the initial test, the relevant code was unchanged from the
supplied archive:

- `src/wallet/rpc/spend.cpp:1452` acquires the wallet lock in `sendall`.
- That scope calls `FinishTransaction` at line 1574.
- `FinishTransaction` calls `BlockUntilSyncedToCurrentChain` at line 100, which
  requires the wallet lock not to be held.

The baseline Git revision contains the same call sequence. A separate
unmodified-baseline binary was not built.

The September 24 regression follow-up fixes this call sequence in `spend.cpp`.
`sendall` releases its input-selection lock before synchronization, and
`FinishTransaction` then holds the wallet lock through signing and commit.
Both failing scripts passed afterward, along with five related functional
scripts and thirteen relevant unit suites. See the
[hashrate and regression report](coinbase-work-maturity-hashrate-tests.md) for
the complete results and remaining limits.

## License and packaging update

The licensing follow-up changes feature-file comments, documentation, the
runtime license display, and packaging notices. A comparison against the feature
commit confirmed no executable maturity, wallet, mempool, or test logic changed.
The daemon, CLI, and Qt executable were incrementally rebuilt for the runtime
notice, and their version output was checked.

The normal Linux CMake configuration passed. A staged daemon-component install
included the upstream MIT notice, README with the ShockWave notice, 1Miner.net
license, and scope record. The installed daemon displayed the updated terms.
Separate CMake checks verified the component install and macOS resource layout,
including matching license-file contents. All seven edited manpages rendered
without new diagnostics, and the Debian license copy matched the root document.

The Windows installer and a complete macOS package were not built or tested.
No binary release was published. The consensus tests above were not repeated
for this licensing-only follow-up.

## Still needed before production

The [hashrate follow-up](coinbase-work-maturity-hashrate-tests.md) exercises the
selected work target using BC2's real difficulty-adjustment code and synthetic
unmined headers. Adversarial and economic review, independent consensus review,
pool/exchange
integration, broader pruning/recovery testing, and production deployment planning
remain outstanding. The maximum age intentionally permits spending without
meeting the work target; the minimum age is a block count, not a time guarantee.

The code and results still need to be posted through the BCP process. Developer
sponsorship and Ready status are community actions; this local implementation
does not set either. The required discussion window must complete before a
production activation height is set.
