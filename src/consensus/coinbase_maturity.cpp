// Copyright (c) 2026 1Miner.net
// Licensed under LICENSE-1MINER-BC2-MATURITY.md, including its prior-grant exception.

#include <consensus/coinbase_maturity.h>

#include <chain.h>
#include <consensus/consensus.h>
#include <consensus/params.h>

#include <algorithm>
#include <limits>

Consensus::CoinbaseMaturity Consensus::GetCoinbaseMaturity(int coin_height, const CBlockIndex& prev_block, const Params& params)
{
    CoinbaseMaturity result;
    // Do not overflow the candidate height or subtract unsigned chainwork for
    // a reward that cannot exist in the supplied parent's UTXO view.
    if (coin_height < 0 || prev_block.nHeight < 0 ||
        prev_block.nHeight == std::numeric_limits<int>::max() || coin_height > prev_block.nHeight) {
        return result;
    }

    const int age{prev_block.nHeight + 1 - coin_height};
    if (coin_height < params.nCoinbaseWorkMaturityActivationHeight) {
        result.blocks_to_minimum = result.blocks_to_maximum = std::max(0, COINBASE_MATURITY - age);
        result.mature = age >= COINBASE_MATURITY;
        return result;
    }

    result.work_based = true;
    result.required_work = UintToArith256(params.nCoinbaseMaturityWork);
    if (params.nCoinbaseWorkMaturityActivationHeight < 0 || params.nCoinbaseMaturityMin <= 0 ||
        params.nCoinbaseMaturityMax < params.nCoinbaseMaturityMin || result.required_work == 0) {
        return result;
    }

    result.blocks_to_minimum = std::max(0, params.nCoinbaseMaturityMin - age);
    result.blocks_to_maximum = std::max(0, params.nCoinbaseMaturityMax - age);
    const CBlockIndex* origin{prev_block.GetAncestor(coin_height)};
    if (!origin || origin->nChainWork > prev_block.nChainWork) return result;
    result.accumulated_work = prev_block.nChainWork - origin->nChainWork;
    result.mature = age >= params.nCoinbaseMaturityMin &&
        (result.accumulated_work >= result.required_work || age >= params.nCoinbaseMaturityMax);
    return result;
}
