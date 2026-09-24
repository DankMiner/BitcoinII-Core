# Prototype validation — 2026-09-23

This is a local, tested prototype of the 4,200–12,960-block proposal. Mainnet,
testnet, signet, and ordinary regtest retain the existing maturity rule. The
experimental regtest rule requires both explicit options. The draft mainnet work
threshold now targets age 8,580 at the hardest sustained legacy epoch difficulty;
activation remains disabled. See the [baseline note](coinbase-work-maturity-baseline.md).

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

## Existing wallet test failure

`wallet_basic.py` failed at its `sendall` call (line 488), with a Debug lock
assertion: `BlockUntilSyncedToCurrentChain()` was called while `cs_wallet` was
already held.

The relevant code is unchanged from the supplied archive:

- `src/wallet/rpc/spend.cpp:1452` acquires the wallet lock in `sendall`.
- That scope calls `FinishTransaction` at line 1574.
- `FinishTransaction` calls `BlockUntilSyncedToCurrentChain` at line 100, which
  requires the wallet lock not to be held.

The baseline Git revision contains the same call sequence, and this patch does
not modify `spend.cpp`. A separate unmodified-baseline binary was not built.
This existing source-level lock defect needs its own fix and regression test;
the full legacy wallet functional suite is not being reported as passing.

## Still needed before production

The selected accumulated-work target needs economic simulation and review using
BC2's real difficulty adjustment. Independent consensus review, pool/exchange
integration, pruned-node recovery testing, and production deployment planning
remain outstanding. The maximum age intentionally permits spending without
meeting the work target; the minimum age is a block count, not a time guarantee.

The code and results still need to be posted through the BCP process. Developer
sponsorship and Ready status are community actions; this local implementation
does not set either. The required discussion window must complete before a
production activation height is set.
