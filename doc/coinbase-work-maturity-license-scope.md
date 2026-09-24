# Work-based maturity license scope

Copyright (c) 2026 1Miner.net. This record accompanies the
[1Miner.net BitcoinII Work-Based Maturity License](../LICENSE-1MINER-BC2-MATURITY.md).

The feature was introduced by commit
`3dbafb568d73458406dd8b821c4815648c2e4233`, relative to baseline
`8daaf7b12e71d3646eed787f040bf2899a69dc1c`. Those revisions identify the
contribution; they do not retroactively change its original licensing.

## New feature files

The following files were introduced for the maturity proposal. Only original
material for which 1Miner.net holds the applicable rights is covered:

- `src/consensus/coinbase_maturity.cpp`
- `src/consensus/coinbase_maturity.h`
- `src/test/coinbase_maturity_tests.cpp`
- `test/functional/feature_coinbase_work_maturity.py`
- `doc/coinbase-work-maturity.md`
- `doc/coinbase-work-maturity-baseline.md`
- `doc/coinbase-work-maturity-validation.md`

## Additions within existing files

Only original maturity additions and modifications in the feature diff are
covered in the files below. Existing contents, removed lines, and unchanged
context are excluded. An added line is not, by itself, evidence of originality
or ownership; copied upstream material and material not subject to copyright
remain excluded.

- `src/CMakeLists.txt`
- `src/bench/mempool_stress.cpp`
- `src/chainparams.cpp`
- `src/chainparamsbase.cpp`
- `src/consensus/params.h`
- `src/consensus/tx_verify.cpp`
- `src/consensus/tx_verify.h`
- `src/interfaces/chain.h`
- `src/interfaces/wallet.h`
- `src/kernel/CMakeLists.txt`
- `src/kernel/chainparams.cpp`
- `src/kernel/chainparams.h`
- `src/node/interfaces.cpp`
- `src/qt/transactiondesc.cpp`
- `src/qt/transactionrecord.cpp`
- `src/qt/transactionrecord.h`
- `src/qt/transactiontablemodel.cpp`
- `src/test/CMakeLists.txt`
- `src/test/fuzz/coins_view.cpp`
- `src/test/fuzz/package_eval.cpp`
- `src/test/fuzz/tx_pool.cpp`
- `src/test/transaction_tests.cpp`
- `src/txmempool.cpp`
- `src/txmempool.h`
- `src/validation.cpp`
- `src/wallet/interfaces.cpp`
- `src/wallet/rpc/transactions.cpp`
- `src/wallet/test/wallet_tests.cpp`
- `src/wallet/wallet.cpp`
- `src/wallet/wallet.h`
- `test/functional/test_runner.py`

## Exclusions and prior grants

Upstream copyrights and licenses are preserved. The scope excludes ShockWave,
third-party libraries, the existing test framework, factual blockchain data, and
independent implementations of the proposal's mathematical rule.

The earlier feature commit was published with MIT notices. The new terms do not
cancel rights granted by that distribution. See the license's "Earlier
distributions" section before making any claim about prohibited use.

Later original changes to these feature files or feature-specific portions are
covered only where their rightsholder expressly applies the 1Miner.net license.
The list does not assign anyone else's contributions to 1Miner.net.
