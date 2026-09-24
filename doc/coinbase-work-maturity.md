# Work-based coinbase maturity (draft prototype)

Copyright (c) 2026 1Miner.net. See the
[maturity license](../LICENSE-1MINER-BC2-MATURITY.md) and its prior-grant exception.

This prototype implements the proposed 4,200–12,960-block maturity range. It is
disabled on mainnet and every other network by default. The draft mainnet work
threshold targets age 8,580 at the hardest sustained legacy epoch difficulty.
No production activation height has been chosen.

See the [baseline calculation](coinbase-work-maturity-baseline.md) for the exact
constant, historical reference, and expected behavior at other work rates. The
selected threshold is fixed; future record difficulty does not change it.

See [the validation report](coinbase-work-maturity-validation.md) for completed
builds, passing tests, and the existing wallet test failure found during review.

## Rule

Let `H` be the coinbase's height, `S` the candidate spending block's height, `A`
the activation height, and `W` a fixed, positive required-work parameter. For
coinbases created at or above `A`:

```text
age = S - H
post_reward_work = chainwork(S - 1) - chainwork(H)

mature = age >= 4200 AND (post_reward_work >= W OR age >= 12960)
```

Chainwork is measured on the branch being validated. The reward's own block and
the candidate spending block contribute no credit. Mempool admission evaluates
the next block. Rewards created below `A` retain the existing 100-block consensus
rule, including after activation. Coinbase fees follow the same rule as subsidy.

`W` is expressed in integer chainwork units, not difficulty or hashes per second.
All nodes enforcing the proposal must use the same value. It is not recalculated
from a moving difficulty average. At the ten-minute target, the age bounds are
approximately 29.2 and 90 days; neither bound guarantees a calendar duration.

## What this addresses

Work performed before a reward exists cannot buy it a shorter waiting period.
Renting hashpower afterward can still contribute valid work toward maturity or
speed up block production. The network cannot distinguish rented equipment from
owned equipment. This proposal is not protection against majority-hash attacks.

The upper age limit intentionally permits spending even when `W` has not been
reached. A reorganization can reduce both a reward's age and its subsequent work,
making an otherwise mature reward immature again. Mempool entries spending such
rewards, including their descendants, must then be removed.

## Local regtest use

Both overrides are required together, and are valid only on regtest:

```sh
bitcoinII-d -regtest \
  -testcoinbasematurityheight=2 \
  -testcoinbasematuritywork=00000000000000000000000000000000000000000000000000000000000020d0
```

`20d0` is 8,400 in hexadecimal. Ordinary regtest blocks contribute two chainwork
units each, so this example requires 4,200 subsequent blocks in addition to the
age floor. A reward at height 2 is first spendable in block 4,203: its age is
4,201, and the 4,200 intervening blocks provide 8,400 work. This is an artificial
test value, not a proposed mainnet threshold.

Use a dedicated regtest data directory. Changing consensus test overrides on an
existing chain does not automatically revalidate its history. Start a fresh
regtest chain when comparing configurations.

## Wallet and pool integration

Wallet spendability must use the same branch and work calculation as consensus.
For new rewards, the lower and upper age bounds are known, but the exact unlock
height depends on future work. A block estimate is not a spendability decision.

`gettransaction` adds a `coinbase_maturity` object for confirmed coinbases:

| Field | Meaning |
| --- | --- |
| `mature` | Whether the wallet currently considers the reward spendable. |
| `work_based` | Whether the reward belongs to the activated cohort. |
| `blocks_to_minimum` | Blocks still needed to meet the minimum age. |
| `blocks_to_maximum` | Blocks still needed to reach the unconditional age limit. |
| `accumulated_work` | Work already credited, in hexadecimal. |
| `required_work` | Fixed work threshold, in hexadecimal. |

For work-based rewards, the remaining-block counts describe eligibility in the
next block. Zero `blocks_to_minimum` alone does not mean the reward is spendable.
Pools and exchanges should use actual eligibility, not a hardcoded block count.

## Tests and review

After building with wallet support, run the dedicated extended functional test:

```sh
python3 build/test/functional/test_runner.py feature_coinbase_work_maturity.py
```

The test uses three disconnected regtest chains and the real 4,200/12,960 bounds.
It exercises:

- The age floor with sufficient work, the exact work threshold, and the age cap
  with insufficient work.
- Exclusion of earlier work, the reward block, and the spending block.
- Preservation of pre-activation rewards.
- Mempool rejection and direct block-validation rejection of premature spends.
- Removal of immature mempool spends and descendants after reorganizations.
- Wallet balances, available coins, maturity RPC data, and chainstate rebuild.

The test is in the extended suite because it creates over 21,000 blocks. The
ordinary regtest suite retains the default fixed maturity unless both overrides
are supplied. Test results must be recorded separately; the existence of these
tests is not a claim that a build or run succeeded.

Before production use, the proposal still needs developer review, rental/exit
simulations using BC2's real difficulty adjustment, review of the selected `W`, and
integration testing with pool and exchange software. Pruned-node recovery and
alternative-branch validation also deserve explicit release testing.

The proposal remains Draft until a developer sponsors it. Tested code and
results must be posted before it becomes Ready. The community's minimum 14-day
discussion window must finish before any production activation height is set.
