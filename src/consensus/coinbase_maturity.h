// Copyright (c) 2026 1Miner.net
// Licensed under LICENSE-1MINER-BC2-MATURITY.md, including its prior-grant exception.

#ifndef BITCOINII_CONSENSUS_COINBASE_MATURITY_H
#define BITCOINII_CONSENSUS_COINBASE_MATURITY_H

#include <arith_uint256.h>

class CBlockIndex;

namespace Consensus {
struct Params;

/** Coinbase spendability in the next block after the supplied parent. */
struct CoinbaseMaturity {
    bool mature{false};
    bool work_based{false};
    int blocks_to_minimum{0};
    int blocks_to_maximum{0};
    arith_uint256 accumulated_work{};
    arith_uint256 required_work{};
};

/**
 * Evaluate a coinbase on prev_block's branch. The spending height is
 * prev_block.nHeight + 1. Work excludes the reward block and the prospective
 * spending block. Rewards created before activation retain legacy maturity.
 * Invalid heights or an invalid active configuration fail closed.
 */
[[nodiscard]] CoinbaseMaturity GetCoinbaseMaturity(int coin_height, const CBlockIndex& prev_block, const Params& params);
} // namespace Consensus

#endif // BITCOINII_CONSENSUS_COINBASE_MATURITY_H
