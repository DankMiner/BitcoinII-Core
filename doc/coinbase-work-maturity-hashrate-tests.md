# Work-based maturity: hashrate and regression tests

Tested September 24, 2026 UTC.

Copyright (c) 2026 1Miner.net. See the
[maturity license](../LICENSE-1MINER-BC2-MATURITY.md) and its prior-grant exception.

The maturity checks passed across the work-rate vectors and all 33 modeled
hashrate scenarios. Rented hashpower can shorten maturity by contributing work
after a reward is created. It cannot bypass the 4,200-block minimum in these
tests. Work before creation, the reward block, and the candidate spending block
receive no credit. The 12,960-block limit permits spending even below the work
threshold, as designed.

Mainnet activation remains disabled. These results support developer review;
they are not a proof of resistance to every mining strategy or a deployment
decision.

## Work-rate vectors

The consensus unit suite now has 13 cases, including six new groups. An
independent, unbounded-integer oracle checks 42 schedules at every candidate age
from 1 through 13,000: **546,000 candidate-age states**. Additional assertions
cover exact `W-1`, `W`, and `W+1` boundaries, alternative branches, pre-reward
bursts, single large-work blocks, 500-block bursts, work ordering, stalls, and
timestamp independence. Eighteen schedules use fixed random seeds.

These vectors supply synthetic completed work directly to the consensus helper.
They test maturity arithmetic separately from difficulty adjustment. For fixed
work per block relative to the hardest-epoch reference:

| Work per block | First spendable age |
| --- | ---: |
| 0.01x, 0.05x, 0.1x, 0.25x, 0.5x | 12,960 |
| 2/3x | 12,870 |
| 0.75x | 11,440 |
| 1x | 8,580 |
| 1.5x | 5,721 |
| 2x | 4,291 |
| 3x, 10x, 100x, 1000x | 4,200 |

The 13-case suite passed in about 12 seconds in the Debug build.

## ShockWave and changing hashrate

The separate simulation calls the actual `GetNextWorkRequired`,
`GetBlockProof`, and `GetCoinbaseMaturity` functions. It uses mainnet difficulty
parameters and the exact proposed `W`, enabling maturity only in its local
parameter copy. Its headers are synthetic and unmined; it does not perform
mainnet proof-of-work or replay the historical blockchain.

The reference expected hashrate is the hardest-epoch block proof divided by
600 seconds: approximately **0.27214 EH/s**. This is a calibration reference,
not a measurement of current BC2 hashrate. Steady-rate scenarios begin with
difficulty scaled to that rate, followed by 600 modeled warm-up blocks.

The model integrates arrival hazard across candidate-time difficulty changes,
including emergency easing, rather than assuming difficulty stays fixed during
a stall. It checks MTP and future-timestamp limits, valid compact targets, and
independent post-reward work accumulation at every candidate age. Consensus
work arithmetic remains integer-only; floating point is used for modeled time.

All **33 scenarios passed**, covering **274,142 modeled headers** including
warm-up and **2,186 candidate-time emergency target changes**. They include
steady rates from 0.001x to 1000x, rentals before and after creation, timed
rentals, periodic bursts, abrupt exits, a one-day outage, clock offsets, and
three seeds each for stochastic low, baseline, high, and rental scenarios.

Selected deterministic results:

| Scenario | First spendable age | Modeled days to eligible parent |
| --- | ---: | ---: |
| Steady reference rate | 8,580 | 59.58 |
| Steady 2x reference rate | 4,291 | 29.79 |
| Steady 10x reference rate | 4,200 | 29.16 |
| Steady 0.1x reference rate | 12,960 | 89.99 |
| 1000x rate for 500 blocks strictly before reward | 8,580 | 59.58 |
| 1000x rate for 500 blocks after reward | 4,200 | 29.24 |
| About 1 EH/s extra for 500 blocks after reward | 6,719 | 46.93 |
| About 1 EH/s extra for one hour after reward | 8,550 | 59.43 |
| About 1 EH/s extra for one day after reward | 8,013 | 55.91 |
| 0.1x baseline plus about 1 EH/s extra for 500 blocks | 12,960 | 90.09 |

The approximate 1 EH/s scenarios round total rate to thousandths of the
reference: 4.675x on the 1x baseline and 3.775x on the 0.1x baseline. Extreme
1000x rates are stress inputs, not claims about rentable equipment. Rental-work
columns count all chainwork in blocks arriving during the rental interval; they
do not identify which miner won those blocks.

The three stochastic baseline runs matured at ages 6,829, 6,792, and 6,994,
around 59-61 modeled days. ShockWave changed their work per block and block
spacing. **Age 8,580 is calibrated to fixed reference difficulty, not fixed
hashrate.** Low-rate stochastic runs reached the 12,960 cap in roughly 110-114
modeled days. The age bounds are block counts, not calendar deadlines.

See [all 33 observations](coinbase-work-maturity-hashrate-results.csv). Elapsed
time ends at the parent that makes a spend eligible; it excludes mining the
future spending block. The model omits propagation, orphan races, selfish
mining, rental pricing and availability, and miner incentives. Three random
seeds are a regression sample, not a statistical security study.

## Build and execution

The base was commit `f1cddddd60d3affea6d7840873196b833cf26bd2`, followed by the
test and wallet fixes described here. The environment was Ubuntu 24.04 under
WSL, GCC 13.3, CMake 3.28, and Ninja, with wallet and Qt support.

The broad Debug CTest run recorded **156 passes, one optional-assets skip, and
one timeout**. `script_assets_tests` skipped because `DIR_UNIT_TEST_DATA` was
unset. The complete ShockWave model exceeded the imposed 600-second per-suite
limit in Debug; no consensus assertion failed before the timeout.

The full simulation then passed in **248.99 seconds** using a separate
executable with four translation units optimized at `-O2`: `pow.cpp`,
`chain.cpp`, `arith_uint256.cpp`, and the simulation test. The existing compile
flags, assertions, overflow traps, and debug lock checks were retained; `NDEBUG`
was not added. Primary build objects and executables were not replaced by this
optimized test build. Its first six scenarios matched the completed Debug
observations byte for byte.

The simulation source SHA-256 was:

```text
49487b1677ad03ea086bcc8d459d021b1cedc50b8a775807f8dc8541d8b8213a
```

The complete CSV SHA-256 is:

```text
9ffd2ebcfcdb32aa33aa43b6b400c294584a6f65076cb87ca351979d0743aec6
```

The hashrate suite carries the `extended` and `hashrate` CTest labels. Use an
optimized build for the complete model. Ordinary Debug runs can exclude it:

```sh
ctest --test-dir build --output-on-failure -LE extended -j 2
BC2_HASHRATE_RESULTS=/tmp/bc2-hashrate.csv \
  build/bin/test_bitcoinII --run_test=coinbase_maturity_hashrate_tests --log_level=message
```

The second command runs the full model and is slow with an unoptimized binary.
It does not start network nodes.

## Regression findings

The original retarget unit test selected ShockWave for a historical Bitcoin
fixture at height 68,543 while expecting Bitcoin's old retarget result. The
historical fixtures now explicitly select legacy rules in local parameter
copies, keeping their expected targets and rejection assertions. The new
simulation supplies explicit ShockWave coverage. The PoW transition replay
target now supplies the candidate header required by BitcoinII's API.

Seven replay targets each passed 1,024 deterministic inputs: integer
deserialization, chain indexes, PoW, PoW transitions, coins views, and the two
transaction-pool targets. This is **7,168 successful replay inputs**, not a
coverage-guided fuzzing campaign or sanitizer run.

The broader functional run reproduced the previously documented `sendall`
wallet-lock failure. The fix releases its input-selection lock before waiting
for wallet notifications, then holds the wallet lock through signing,
finalization, and commit. It preserves synchronization and lock assertions.

After that fix, all seven targeted functional scripts passed:
`wallet_basic.py`, `wallet_sendall.py`, `wallet_send.py`, `rpc_psbt.py`,
`wallet_multisig_descriptor_psbt.py`, `wallet_signer.py`, and
`feature_coinbase_work_maturity.py`. Thirteen relevant unit suites also passed
again. The maturity functional test still uses real 4,200/12,960 limits and
actual regtest proof-of-work.

The initial broader run finished with 19 passing functional variants and the
two wallet failures above. Across that run and the successful focused reruns,
all **25 selected functional variants** have passing final outcomes. This is
not a claim that every repository functional test ran, or that the initial
run was clean. Coverage includes block validation, maturity, mining templates,
prioritization, mempool reorganization and eviction, wallet accounting,
reindexing, pruning, chain selection, and both P2P transport variants.

The large-transaction mempool test also exposed a slow test helper. `bulk_vout`
repeatedly serialized the growing transaction after each padding output. It
now computes the number of outputs first, including CompactSize-prefix growth,
then appends them together. All 548 comparisons against the prior helper
produced identical successful transaction bytes, including witness transactions
and the 252/253 and 65,535/65,536 output-count boundaries. Exact funding now
succeeds without money for a discarded probe output; insufficient funding
leaves the input transaction untouched.

All three script-helper unit tests passed, including 70 boundary/witness
subcases. Padding a 100,000-vbyte transaction took about 0.006 seconds versus
4.6-4.8 seconds before the change in this local measurement. The end-to-end
rerun passed `mempool_updatefromblock.py`, `mempool_reorg.py`, and
`mining_prioritisetransaction.py`. This optimization changes test construction,
not transaction validation or consensus.

## Before production

Independent consensus review, realistic adversarial/economic analysis,
pool/exchange integration, cross-platform testing, and the BCP discussion and
activation process are still required. The tests make no production activation
change and do not establish rental-proof payouts.
