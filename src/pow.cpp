// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core / Darkcoin developers
// Copyright (c) 2026 The BitcoinII Core developers
// Copyright (c) 2026 KvantaMechanic
//
// Portions of this file derived from Bitcoin Core and Dash/Darkcoin are
// distributed under the MIT software license and remain subject to their
// respective copyright notices and license terms. See the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Original ShockWave implementation code authored in 2026 is proprietary
// software and is NOT distributed under the MIT license. The proprietary
// terms applicable to ShockWave are stated below.
//
// -----------------------------------------------------------------------------
// ShockWave Difficulty Adjustment Algorithm
// -----------------------------------------------------------------------------
//
// ShockWave is a BitcoinII difficulty-adjustment algorithm developed in 2026.
//
// Its rolling difficulty baseline is derived in part from concepts and code
// originating in Dark Gravity Wave v3, originally developed for
// Darkcoin/Dash. Those inherited portions remain subject to their applicable
// MIT license terms.
//
// ShockWave substantially extends that rolling baseline with independent
// consensus mechanisms developed for BitcoinII, including rapid hashrate
// response, overshoot control, timestamp uncertainty handling, emergency stall
// recovery, recovery management, and post-recovery stabilization.
//
// ShockWave development and BitcoinII integration:
//   KvantaMechanic / The BitcoinII Core developers
//
// ShockWave uses:
//   - MedianTimePast-based 25-block / 24-interval rolling difficulty baseline
//   - true +/-4x per-block final difficulty-adjustment bounds
//   - difficulty-normalized six-interval raw tightening sensor
//   - aggressive-ratchet continuation for unmistakably fast post-step blocks
//   - deterministic overshoot-regime reset on the first real post-ratchet stall
//   - retention of the overshoot-triggering interval as new-regime evidence
//   - one-way newest-block tightening brake against stale rolling-history overshoot
//   - trusted-history tightening veto while the newest raw clock is untrusted
//   - dual-confirmation trusted-history easing across an untrusted-clock handoff
//   - one-way newest-block easing veto while the latest trusted block is fast
//   - dual raw/MTP timestamp-consistency checks
//   - deterministic candidate-time emergency stall recovery
//   - Bitcoin Core MAX_FUTURE_BLOCK_TIME as the timestamp uncertainty budget
//   - 25% emergency difficulty reductions every five minutes once recovery begins
//   - deterministic recovery-regime reset after an emergency block
//   - immediate per-block +/-4x authority while the post-recovery window refills
//   - per-interval raw refill timing capped at 4x target spacing
//   - integer-only consensus arithmetic
//
// Emergency recovery does not alter MAX_FUTURE_BLOCK_TIME. The complete
// future-time allowance is subtracted before a candidate timestamp can justify
// any emergency reduction in required work.
//
// -----------------------------------------------------------------------------
// ShockWave Proprietary Source-Review License
// -----------------------------------------------------------------------------
//
// Copyright (c) 2026 KvantaMechanic.
// All rights reserved.
//
// Except for portions independently subject to the MIT license as described
// above, the original ShockWave source code and implementation contained in
// this file are proprietary software.
//
// The ShockWave source is made publicly available solely for:
//   - security review;
//   - technical and consensus audit;
//   - interoperability analysis;
//   - academic or technical evaluation; and
//   - testing of the BitcoinII implementation.
//
// Permission is granted to view the ShockWave source code and to make temporary
// copies strictly as necessary to inspect, compile, execute, and test it for
// the review and evaluation purposes listed above.
//
// NO LICENSE IS GRANTED to incorporate, deploy, reuse, redistribute, or exploit
// the proprietary ShockWave implementation, in whole or in substantial part,
// in another blockchain, cryptocurrency, distributed-ledger system, software
// product, service, protocol, or other implementation.
//
// Without prior express written permission from KvantaMechanic, you may not:
//
//   - incorporate proprietary ShockWave source code, or any substantial portion
//     of it, into another project;
//
//   - copy, adapt, modify, translate, port, or create derivative works from the
//     proprietary ShockWave implementation except as strictly necessary for
//     permitted review and testing;
//
//   - redistribute, republish, sublicense, sell, license, or commercially
//     exploit the proprietary ShockWave implementation;
//
//   - deploy the proprietary ShockWave implementation, or a derivative of it,
//     on another blockchain, cryptocurrency, distributed-ledger network,
//     software product, service, or protocol;
//
//   - use the proprietary ShockWave implementation as the basis for another
//     production difficulty-adjustment implementation; or
//
//   - remove, obscure, or alter this copyright or license notice.
//
// The limited permissions granted for review, audit, compilation, execution,
// and testing do not constitute an open-source license and grant no rights
// except those expressly stated.
//
// Nothing in this notice restricts rights independently granted under the MIT
// license with respect to Bitcoin Core, Dash/Darkcoin, or other pre-existing
// MIT-licensed code. The proprietary restrictions above apply only to original
// ShockWave material for which KvantaMechanic holds the applicable copyright.
//
// Any use of proprietary ShockWave material outside the expressly permitted
// review and evaluation purposes requires prior express written authorization
// from KvantaMechanic.
//
// THE PROPRIETARY SHOCKWAVE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES, OR
// OTHER LIABILITY ARISING FROM, OUT OF, OR IN CONNECTION WITH THE PROPRIETARY
// SHOCKWAVE SOFTWARE OR ITS USE.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/check.h>

#include <cassert>
#include <limits>

namespace {

static constexpr int64_t SHOCKWAVE_ROLLING_BLOCKS{25};
static constexpr int64_t SHOCKWAVE_ROLLING_INTERVALS{SHOCKWAVE_ROLLING_BLOCKS - 1};

// The short controller is tightening-focused. Six completed raw intervals are
// enough to react quickly, while each interval is normalized by the target at
// which that block was actually mined.
static constexpr int64_t FAST_INTERVALS{6};
static constexpr int64_t FAST_BLOCKS{FAST_INTERVALS + 1};

// Easing while the newest raw clock is untrusted requires stronger historical
// confirmation than a one-way tightening veto. Only independently trusted
// intervals may contribute, and at least four of the newest six are required.
static constexpr int64_t TRUSTED_EASING_MIN_INTERVALS{4};

// Emergency stall recovery deliberately keeps Bitcoin Core's existing
// MAX_FUTURE_BLOCK_TIME unchanged. The full future-time allowance is
// subtracted before any emergency easing is permitted.
static constexpr int64_t STALL_RECOVERY_TRIGGER{30 * 60};
static constexpr int64_t STALL_RECOVERY_STEP{5 * 60};

// An 11-block MTP is effectively centered about five blocks behind the tip.
// After an emergency recovery block, normal MTP history is not fully clean
// until the tip reaches reset+29: the ordinary 25/24 window then begins at
// reset+5. Until that point V9 uses only post-reset raw intervals, each
// individually bounded so one future-dated header cannot buy a long cheap run.
static constexpr int64_t RECOVERY_MTP_LAG{5};
static constexpr int64_t RECOVERY_REFILL_DISTANCE{
    RECOVERY_MTP_LAG + SHOCKWAVE_ROLLING_INTERVALS};
static constexpr int64_t RECOVERY_RAW_INTERVAL_CAP_MULTIPLIER{4};

// A full 4x ratchet may continue from one aggressively tightened block to the
// next when the newest block is still unmistakably fast. 1/8 of target spacing
// is deliberately much stricter than merely "faster than 600 seconds": at a
// healthy equilibrium an isolated lucky block must not routinely authorize a
// further 4x jump.
static constexpr int64_t RATCHET_FAST_DIVISOR{8};

// A regime-break reset is only armed by an aggressive prior increase. This
// prevents ordinary small ShockWave fluctuations from constantly resetting history.
static constexpr int64_t AGGRESSIVE_TIGHTENING_FACTOR{2};

uint32_t PowLimitBits(const Consensus::Params& params)
{
    return UintToArith256(params.powLimit).GetCompact();
}

bool IsShockWaveEnabledForNextBlock(const CBlockIndex* pindexLast,
                              const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    // If activation height is H, block H is the first block
    // whose required work is calculated using ShockWave.
    return pindexLast->nHeight + 1 >= params.nShockWaveActivationHeight;
}

int64_t BlockTimeForDA(const CBlockIndex* pindex)
{
    assert(pindex != nullptr);
    return pindex->GetMedianTimePast();
}


/**
 * Scale a target by nActualTimespan / nTargetTimespan using integer-only
 * arithmetic, saturating at powLimit.
 *
 * The quotient/remainder decomposition computes:
 *
 *   floor(target * actual / expected)
 *
 * without constructing the potentially overflowing target * actual product.
 * No timespan adjustment bound is imposed inside this arithmetic helper.
 * Callers decide whether a window-level governor is appropriate, and the
 * final normal target is still bounded relative to the immediately previous
 * block.
 */
arith_uint256 ScaleTargetByTimespan(
    const arith_uint256& bnTarget,
    uint64_t nActualTimespan,
    uint64_t nTargetTimespan,
    const arith_uint256& bnPowLimit)
{
    assert(nTargetTimespan > 0);

    if (bnTarget == 0 || nActualTimespan == 0) {
        return arith_uint256{1};
    }

    const arith_uint256 bnDivisor{nTargetTimespan};
    const arith_uint256 bnMultiplier{nActualTimespan};

    const arith_uint256 bnQuotient = bnTarget / bnDivisor;
    const arith_uint256 bnRemainder =
        bnTarget - (bnQuotient * bnDivisor);

    if (bnQuotient != 0 &&
        bnQuotient > bnPowLimit / bnMultiplier) {
        return bnPowLimit;
    }

    arith_uint256 bnResult = bnQuotient * bnMultiplier;

    // bnRemainder < nTargetTimespan. For Bitcoin-style target spacings and
    // timestamp ranges this intermediate is tiny relative to 256 bits; still,
    // guard it explicitly against the selected network powLimit.
    if (bnRemainder != 0 &&
        bnRemainder > bnPowLimit / bnMultiplier) {
        return bnPowLimit;
    }

    arith_uint256 bnFraction = bnRemainder * bnMultiplier;
    bnFraction /= bnDivisor;

    if (bnResult > bnPowLimit - bnFraction) {
        return bnPowLimit;
    }

    bnResult += bnFraction;

    if (bnResult == 0) {
        bnResult = 1;
    }

    if (bnResult > bnPowLimit) {
        bnResult = bnPowLimit;
    }

    return bnResult;
}

/**
 * Original Bitcoin 2016-block difficulty adjustment calculation.
 *
 * This is intentionally kept separate from ShockWave so that every block before
 * nShockWaveActivationHeight continues using the historical Bitcoin rules.
 */
unsigned int BitcoinCalculateNextWorkRequired(
    const CBlockIndex* pindexLast,
    int64_t nFirstBlockTime,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    // Limit adjustment step.
    int64_t nActualTimespan =
        pindexLast->GetBlockTime() - nFirstBlockTime;

    if (nActualTimespan < params.nPowTargetTimespan / 4) {
        nActualTimespan = params.nPowTargetTimespan / 4;
    }

    if (nActualTimespan > params.nPowTargetTimespan * 4) {
        nActualTimespan = params.nPowTargetTimespan * 4;
    }

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    arith_uint256 bnNew;

    // Special difficulty rule for Testnet4.
    if (params.enforce_BIP94) {
        // Use the first block of the difficulty period so that the real
        // difficulty is preserved even when min-difficulty blocks occur.
        const int nHeightFirst =
            pindexLast->nHeight -
            (params.DifficultyAdjustmentInterval() - 1);

        const CBlockIndex* pindexFirst =
            pindexLast->GetAncestor(nHeightFirst);

        assert(pindexFirst);

        bnNew.SetCompact(pindexFirst->nBits);
    } else {
        bnNew.SetCompact(pindexLast->nBits);
    }

    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

/**
 * Original Bitcoin GetNextWorkRequired implementation.
 */
unsigned int BitcoinGetNextWorkRequired(
    const CBlockIndex* pindexLast,
    const CBlockHeader* pblock,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    assert(pblock != nullptr);

    const unsigned int nProofOfWorkLimit =
        PowLimitBits(params);

    // Only change once per difficulty adjustment interval.
    if ((pindexLast->nHeight + 1) %
            params.DifficultyAdjustmentInterval() != 0) {
        if (params.fPowAllowMinDifficultyBlocks) {
            // Special difficulty rule for testnet:
            //
            // If the new block's timestamp is more than
            // 2 * target spacing after the previous block,
            // allow a minimum-difficulty block.
            if (pblock->GetBlockTime() >
                pindexLast->GetBlockTime() +
                    params.nPowTargetSpacing * 2) {
                return nProofOfWorkLimit;
            }

            // Return the last non-special-min-difficulty block.
            const CBlockIndex* pindex = pindexLast;

            while (pindex->pprev &&
                   pindex->nHeight %
                           params.DifficultyAdjustmentInterval() != 0 &&
                   pindex->nBits == nProofOfWorkLimit) {
                pindex = pindex->pprev;
            }

            return pindex->nBits;
        }

        return pindexLast->nBits;
    }

    // Go back one complete Bitcoin difficulty adjustment interval.
    const int nHeightFirst =
        pindexLast->nHeight -
        (params.DifficultyAdjustmentInterval() - 1);

    assert(nHeightFirst >= 0);

    const CBlockIndex* pindexFirst =
        pindexLast->GetAncestor(nHeightFirst);

    assert(pindexFirst);

    return BitcoinCalculateNextWorkRequired(
        pindexLast,
        pindexFirst->GetBlockTime(),
        params);
}

/**
 * BitcoinII rolling difficulty adjustment.
 *
 * Normal operation uses a corrected ShockWave-style controller:
 *
 *   - 25 sampled blocks
 *   - 24 completed intervals
 *   - MedianTimePast timing
 *   - rolling arithmetic mean of the sampled targets
 *   - ordinary +/-4x rolling window-timespan governor for stability
 *   - true +/-4x FINAL target bounds relative to the previous block
 *   - separate six-interval normalized raw panic/capture controller
 *   - newest-block one-way tightening veto against stale-window overshoot
 *
 * Emergency stall recovery is intentionally separate from normal ShockWave. It is
 * evaluated only when GetNextWorkRequired() has the candidate header.
 */
arith_uint256 ShockWaveRollingTarget(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    bool fNegative{false};
    bool fOverflow{false};

    arith_uint256 bnPreviousTarget;
    bnPreviousTarget.SetCompact(
        pindexLast->nBits,
        &fNegative,
        &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnPreviousTarget == 0 ||
        bnPreviousTarget > bnPowLimit) {
        return bnPowLimit;
    }

    if (params.fPowNoRetargeting) {
        return bnPreviousTarget;
    }

    if (pindexLast->nHeight < SHOCKWAVE_ROLLING_BLOCKS - 1) {
        return bnPowLimit;
    }

    const CBlockIndex* pindex = pindexLast;

    arith_uint256 bnPastTargetAvg{0};
    int64_t nBlockCount{0};
    int64_t nNewestMTP{0};
    int64_t nOldestMTP{0};

    while (pindex != nullptr &&
           nBlockCount < SHOCKWAVE_ROLLING_BLOCKS) {
        arith_uint256 bnTarget;
        fNegative = false;
        fOverflow = false;

        bnTarget.SetCompact(
            pindex->nBits,
            &fNegative,
            &fOverflow);

        // Invalid historical work must never make the next block easier.
        // Hold the last valid target instead.
        if (fNegative ||
            fOverflow ||
            bnTarget == 0 ||
            bnTarget > bnPowLimit) {
            return bnPreviousTarget;
        }

        ++nBlockCount;

        if (nBlockCount == 1) {
            bnPastTargetAvg = bnTarget;
            nNewestMTP = BlockTimeForDA(pindex);
        } else {
            // Incremental arithmetic mean. Every operation is integer-only.
            bnPastTargetAvg *=
                arith_uint256{static_cast<uint64_t>(nBlockCount - 1)};
            bnPastTargetAvg += bnTarget;
            bnPastTargetAvg /=
                arith_uint256{static_cast<uint64_t>(nBlockCount)};
        }

        if (nBlockCount == SHOCKWAVE_ROLLING_BLOCKS) {
            nOldestMTP = BlockTimeForDA(pindex);
        }

        pindex = pindex->pprev;
    }

    if (nBlockCount < SHOCKWAVE_ROLLING_BLOCKS ||
        nNewestMTP <= 0 ||
        nOldestMTP <= 0) {
        return bnPreviousTarget;
    }

    const int64_t nTargetTimespan =
        SHOCKWAVE_ROLLING_INTERVALS * params.nPowTargetSpacing;

    int64_t nActualTimespan = nNewestMTP - nOldestMTP;

    /*
     * ShockWave remains the stable/landing controller. Preserve its ordinary +/-4x
     * WINDOW timespan governor here. The separate six-interval normalized raw
     * controller may bypass this dilution and request a true 4x harder NEXT
     * block when current difficulty is demonstrably far below observed work.
     */
    if (nActualTimespan < nTargetTimespan / 4) {
        nActualTimespan = nTargetTimespan / 4;
    }

    if (nActualTimespan > nTargetTimespan * 4) {
        nActualTimespan = nTargetTimespan * 4;
    }

    arith_uint256 bnNew = ScaleTargetByTimespan(
        bnPastTargetAvg,
        static_cast<uint64_t>(nActualTimespan),
        static_cast<uint64_t>(nTargetTimespan),
        bnPowLimit);

    if (bnNew == 0) {
        bnNew = 1;
    }

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    /*
     * True per-block bounds.
     *
     * The ShockWave timespan clamp limits the window calculation, but because the
     * moving-average target can itself be changing, it is not sufficient to
     * guarantee a true +/-4x transition relative to the immediately previous
     * block. Clamp the final normal target explicitly.
     */
    arith_uint256 bnHardestTarget = bnPreviousTarget / 4;

    if (bnHardestTarget == 0) {
        bnHardestTarget = 1;
    }

    arith_uint256 bnEasiestTarget = bnPreviousTarget;

    if (bnEasiestTarget > bnPowLimit / 4) {
        bnEasiestTarget = bnPowLimit;
    } else {
        bnEasiestTarget *= 4;

        if (bnEasiestTarget > bnPowLimit) {
            bnEasiestTarget = bnPowLimit;
        }
    }

    if (bnNew < bnHardestTarget) {
        bnNew = bnHardestTarget;
    }

    if (bnNew > bnEasiestTarget) {
        bnNew = bnEasiestTarget;
    }

    return bnNew;
}


/**
 * Return the greatest raw header timestamp in the recent ShockWave horizon.
 *
 * Using a recent maximum rather than only pindexLast->nTime makes it much
 * harder for several deliberately backdated blocks to manufacture an
 * artificial stall. The function uses only committed chain data and is fully
 * deterministic on every validating node.
 */
int64_t ProjectedMTPTime(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const int64_t nMTPNow = pindexLast->GetMedianTimePast();
    int64_t nProjection = RECOVERY_MTP_LAG * params.nPowTargetSpacing;

    /*
     * If the chain is genuinely moving more slowly than target, a fixed
     * five*spacing projection can sit hours behind the newest honest header.
     * Use the larger of expected five-block time and the ACTUAL recent MTP
     * movement across five heights. Fast/compressed histories still receive
     * at least the expected-spacing floor.
     */
    if (pindexLast->nHeight >= RECOVERY_MTP_LAG) {
        const CBlockIndex* pindexLag =
            pindexLast->GetAncestor(
                pindexLast->nHeight - RECOVERY_MTP_LAG);

        if (pindexLag != nullptr) {
            const int64_t nOldMTP = pindexLag->GetMedianTimePast();

            if (nMTPNow > nOldMTP) {
                const int64_t nObservedProjection = nMTPNow - nOldMTP;

                if (nObservedProjection > nProjection) {
                    nProjection = nObservedProjection;
                }
            }
        }
    }

    if (nProjection <= 0) {
        return nMTPNow;
    }

    if (nMTPNow >
        std::numeric_limits<int64_t>::max() - nProjection) {
        return std::numeric_limits<int64_t>::max();
    }

    return nMTPNow + nProjection;
}

/**
 * Conservative emergency timestamp anchor.
 *
 * raw_anchor = maximum raw header time in the newest 25 blocks
 * mtp_anchor = hardened MTP clock projected to the effective tip
 *
 * Emergency EASING always uses the later of the two. Timestamp disagreement
 * may delay easier work, but can never manufacture it.
 */
int64_t RecentTimestampAnchor(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const CBlockIndex* pindex = pindexLast;
    int64_t nRawAnchor = pindexLast->GetBlockTime();
    int64_t nCount{0};

    while (pindex != nullptr && nCount < SHOCKWAVE_ROLLING_BLOCKS) {
        if (pindex->GetBlockTime() > nRawAnchor) {
            nRawAnchor = pindex->GetBlockTime();
        }

        ++nCount;
        pindex = pindex->pprev;
    }

    const int64_t nMTPAnchor = ProjectedMTPTime(pindexLast, params);
    return nMTPAnchor > nRawAnchor ? nMTPAnchor : nRawAnchor;
}

/**
 * Raw timestamps are allowed to accelerate tightening, but a raw clock that
 * sits materially AHEAD of the projected MTP clock must not be allowed to
 * veto MTP tightening or authorize easing. That is the economically useful
 * direction for a profitability miner trying to keep difficulty cheap.
 */
bool RawClockTrustedForModeration(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const int64_t nRawTip = pindexLast->GetBlockTime();
    const int64_t nProjectedMTP = ProjectedMTPTime(pindexLast, params);
    const int64_t nTolerance = params.nPowTargetSpacing;

    if (nRawTip <= nProjectedMTP) {
        return true;
    }

    if (nTolerance <= 0) {
        return false;
    }

    if (nProjectedMTP >
        std::numeric_limits<int64_t>::max() - nTolerance) {
        return true;
    }

    return nRawTip <= nProjectedMTP + nTolerance;
}


/**
 * One-way stale rolling-history tightening veto for an untrusted newest raw clock.
 *
 * A future-ahead newest timestamp must not gain moderation authority. However,
 * discarding the complete recent raw window when that happens can let stale rolling-history
 * history resume tightening after a genuine hashrate loss. Preserve only the
 * recent intervals whose endpoint clocks were independently trusted when they
 * were mined, and normalize each interval by its assigned target.
 *
 * If at least two trusted intervals remain and their normalized estimate says
 * the current target is already hard enough, stale rolling-history tightening is vetoed for
 * one block. Untrusted intervals contribute nothing, so future-dating cannot
 * manufacture the veto. This function can only hold difficulty flat; it never
 * authorizes easier work.
 */
bool RecentTrustedWindowVetoesTightening(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params,
    const arith_uint256& bnPreviousTarget)
{
    assert(pindexLast != nullptr);

    if (pindexLast->nHeight < FAST_INTERVALS ||
        params.nPowTargetSpacing <= 0) {
        return false;
    }

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    const CBlockIndex* pindex = pindexLast;
    uint64_t nTrustedIntervals{0};

    // First pass: count only intervals whose endpoint clock was trusted.
    for (int64_t n = 0; n < FAST_INTERVALS; ++n) {
        if (pindex == nullptr || pindex->pprev == nullptr) {
            return false;
        }

        if (RawClockTrustedForModeration(pindex, params)) {
            ++nTrustedIntervals;
        }

        pindex = pindex->pprev;
    }

    // One historical interval is too little evidence to suppress ShockWave.
    if (nTrustedIntervals < 2) {
        return false;
    }

    const uint64_t nExpectedTimespan =
        nTrustedIntervals * static_cast<uint64_t>(params.nPowTargetSpacing);

    if (nExpectedTimespan == 0) {
        return false;
    }

    // Second pass: build the same difficulty-normalized target estimate used by
    // the fast controller, but from trusted intervals only.
    pindex = pindexLast;
    arith_uint256 bnEstimate{0};

    for (int64_t n = 0; n < FAST_INTERVALS; ++n) {
        if (pindex == nullptr || pindex->pprev == nullptr) {
            return false;
        }

        if (!RawClockTrustedForModeration(pindex, params)) {
            pindex = pindex->pprev;
            continue;
        }

        bool fNegative{false};
        bool fOverflow{false};
        arith_uint256 bnTarget;

        bnTarget.SetCompact(
            pindex->nBits,
            &fNegative,
            &fOverflow);

        // Invalid history must never gain authority to suppress tightening.
        if (fNegative ||
            fOverflow ||
            bnTarget == 0 ||
            bnTarget > bnPowLimit) {
            return false;
        }

        int64_t nDelta =
            pindex->GetBlockTime() - pindex->pprev->GetBlockTime();

        if (nDelta < 0) {
            nDelta = 0;
        }

        const arith_uint256 bnContribution = ScaleTargetByTimespan(
            bnTarget,
            static_cast<uint64_t>(nDelta),
            nExpectedTimespan,
            bnPowLimit);

        if (bnContribution >= bnPowLimit - bnEstimate) {
            return true;
        }

        bnEstimate += bnContribution;
        pindex = pindex->pprev;
    }

    if (bnEstimate == 0) {
        return false;
    }

    // Larger target means easier work. If trusted recent work supports the
    // current target or something easier, another stale rolling-history tightening step is
    // not justified.
    return bnEstimate >= bnPreviousTarget;
}


/**
 * Conservative easing confirmation for an untrusted newest raw clock.
 *
 * The newest untrusted interval itself is never allowed to authorize easier
 * work. Instead, inspect only recent intervals whose BOTH endpoint clocks were
 * independently trusted when committed. Requiring both endpoints prevents an
 * untrusted predecessor timestamp from manufacturing a large positive delta.
 *
 * At least four trusted intervals are required. Each accepted interval is
 * difficulty-normalized by its assigned target and individually capped at
 * 4x target spacing, matching the refill controller's protection against one
 * large timestamp jump financing excessive easing.
 *
 * This helper only returns an easing estimate when trusted history says the
 * current target is too hard. The caller must also require MTP rolling baseline to agree and
 * must choose the HARDER of the two easing targets.
 */
bool RecentTrustedWindowSupportsEasing(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params,
    const arith_uint256& bnPreviousTarget,
    arith_uint256& bnEstimate)
{
    assert(pindexLast != nullptr);

    if (pindexLast->nHeight < FAST_INTERVALS ||
        params.nPowTargetSpacing <= 0) {
        return false;
    }

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    const CBlockIndex* pindex = pindexLast;
    uint64_t nTrustedIntervals{0};

    // Count only intervals for which both committed endpoint clocks were
    // independently trusted. The newest untrusted interval is therefore
    // automatically excluded.
    for (int64_t n = 0; n < FAST_INTERVALS; ++n) {
        if (pindex == nullptr || pindex->pprev == nullptr) {
            return false;
        }

        if (RawClockTrustedForModeration(pindex, params) &&
            RawClockTrustedForModeration(pindex->pprev, params)) {
            ++nTrustedIntervals;
        }

        pindex = pindex->pprev;
    }

    if (nTrustedIntervals < TRUSTED_EASING_MIN_INTERVALS) {
        return false;
    }

    const uint64_t nExpectedTimespan =
        nTrustedIntervals * static_cast<uint64_t>(params.nPowTargetSpacing);

    if (nExpectedTimespan == 0) {
        return false;
    }

    pindex = pindexLast;
    bnEstimate = 0;

    for (int64_t n = 0; n < FAST_INTERVALS; ++n) {
        if (pindex == nullptr || pindex->pprev == nullptr) {
            return false;
        }

        if (!RawClockTrustedForModeration(pindex, params) ||
            !RawClockTrustedForModeration(pindex->pprev, params)) {
            pindex = pindex->pprev;
            continue;
        }

        bool fNegative{false};
        bool fOverflow{false};
        arith_uint256 bnTarget;

        bnTarget.SetCompact(
            pindex->nBits,
            &fNegative,
            &fOverflow);

        // Invalid history must never gain authority to make work easier.
        if (fNegative ||
            fOverflow ||
            bnTarget == 0 ||
            bnTarget > bnPowLimit) {
            return false;
        }

        int64_t nDelta =
            pindex->GetBlockTime() - pindex->pprev->GetBlockTime();

        if (nDelta < 0) {
            nDelta = 0;
        }

        const int64_t nIntervalCap =
            RECOVERY_RAW_INTERVAL_CAP_MULTIPLIER *
            params.nPowTargetSpacing;

        if (nDelta > nIntervalCap) {
            nDelta = nIntervalCap;
        }

        const arith_uint256 bnContribution = ScaleTargetByTimespan(
            bnTarget,
            static_cast<uint64_t>(nDelta),
            nExpectedTimespan,
            bnPowLimit);

        if (bnContribution >= bnPowLimit - bnEstimate) {
            bnEstimate = bnPowLimit;
            break;
        }

        bnEstimate += bnContribution;
        pindex = pindex->pprev;
    }

    if (bnEstimate == 0 || bnEstimate <= bnPreviousTarget) {
        return false;
    }

    return true;
}


/**
 * Return true when pindex itself was mined at a difficulty at least
 * AGGRESSIVE_TIGHTENING_FACTOR harder than its predecessor.
 *
 * This is inferred entirely from committed nBits values. It is used only to
 * identify an active ratchet episode; it does not itself set difficulty.
 */
bool WasAggressivelyTightened(
    const CBlockIndex* pindex,
    const Consensus::Params& params)
{
    if (pindex == nullptr || pindex->pprev == nullptr) {
        return false;
    }

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    bool fNegative{false};
    bool fOverflow{false};
    arith_uint256 bnTarget;
    bnTarget.SetCompact(pindex->nBits, &fNegative, &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnTarget == 0 ||
        bnTarget > bnPowLimit) {
        return false;
    }

    fNegative = false;
    fOverflow = false;

    arith_uint256 bnPreviousTarget;
    bnPreviousTarget.SetCompact(
        pindex->pprev->nBits,
        &fNegative,
        &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnPreviousTarget == 0 ||
        bnPreviousTarget > bnPowLimit) {
        return false;
    }

    arith_uint256 bnAggressiveThreshold =
        bnPreviousTarget / AGGRESSIVE_TIGHTENING_FACTOR;

    if (bnAggressiveThreshold == 0) {
        bnAggressiveThreshold = 1;
    }

    return bnTarget <= bnAggressiveThreshold;
}

/**
 * During an already-active aggressive ratchet, allow the newest block to
 * request another exact 4x step without waiting for a six-block estimator to
 * forget older low-difficulty observations.
 *
 * A trusted raw interval must be less than 1/8 of target spacing. If the raw
 * clock is suspiciously far AHEAD of projected MTP, that suspicion is not
 * allowed to suppress tightening: future-dating is the economically useful
 * manipulation for a miner trying to keep difficulty cheap.
 *
 * Compressed/backdated time can only make this path tighten harder; it cannot
 * make work easier.
 */
bool RecentBlockDemandsRatchet(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (!WasAggressivelyTightened(pindexLast, params) ||
        pindexLast->pprev == nullptr ||
        params.nPowTargetSpacing <= 0) {
        return false;
    }

    if (!RawClockTrustedForModeration(pindexLast, params)) {
        return true;
    }

    int64_t nRawInterval =
        pindexLast->GetBlockTime() -
        pindexLast->pprev->GetBlockTime();

    if (nRawInterval < 0) {
        nRawInterval = 0;
    }

    int64_t nFastThreshold =
        params.nPowTargetSpacing / RATCHET_FAST_DIVISOR;

    if (nFastThreshold < 1) {
        nFastThreshold = 1;
    }

    return nRawInterval < nFastThreshold;
}

/**
 * Identify the first genuine slowdown after an aggressive tightening step.
 *
 * This is the key stale rolling-history momentum brake. If a block that was itself mined
 * at >=2x the previous difficulty then requires at least one full target
 * spacing, the old pre-stall fast blocks are no longer allowed to keep pushing
 * the next targets upward. This block becomes a deterministic regime-reset
 * baseline.
 *
 * The raw slowdown is accepted only when the raw clock is consistent with the
 * projected MTP clock. A suspiciously future-dated header therefore cannot
 * create a reset merely to suppress difficulty.
 */
bool IsOvershootRegimeBreakBlock(
    const CBlockIndex* pindex,
    const Consensus::Params& params)
{
    if (pindex == nullptr ||
        pindex->pprev == nullptr ||
        params.nPowTargetSpacing <= 0) {
        return false;
    }

    if (pindex->nHeight < params.nShockWaveActivationHeight) {
        return false;
    }

    if (!WasAggressivelyTightened(pindex, params)) {
        return false;
    }

    const int64_t nRawInterval =
        pindex->GetBlockTime() -
        pindex->pprev->GetBlockTime();

    if (nRawInterval < params.nPowTargetSpacing) {
        return false;
    }

    return RawClockTrustedForModeration(pindex, params);
}

/**
 * One-way newest-block tightening brake.
 *
 * The dominant rolling-window overshoot failure is stale fast history: difficulty has
 * already caught the current hashrate, the newest block finally takes about a
 * target interval, but the 24-interval rolling window still contains many cheap
 * fast blocks and tries to tighten for several more blocks.
 *
 * A newest raw interval of at least one full target spacing is direct evidence
 * that the immediately previous difficulty no longer needs another increase.
 * When that raw timestamp is consistent enough to receive moderation
 * authority, it may VETO tightening for the next block. It may never make
 * difficulty easier by itself; ordinary ShockWave/recovery logic retains exclusive
 * authority over easing.
 *
 * Requiring a complete target-spacing advance also gives the existing
 * MAX_FUTURE_BLOCK_TIME a useful bound against abuse: a profitability miner
 * attempting to fake repeated slowdown must consume roughly one target spacing
 * of future timestamp budget for each false veto.
 */
bool RecentBlockVetoesTightening(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (pindexLast->pprev == nullptr || params.nPowTargetSpacing <= 0) {
        return false;
    }

    const int64_t nRawInterval =
        pindexLast->GetBlockTime() - pindexLast->pprev->GetBlockTime();

    if (nRawInterval < params.nPowTargetSpacing) {
        return false;
    }

    return RawClockTrustedForModeration(pindexLast, params);
}

/**
 * One-way newest-block easing veto.
 *
 * If the newest trusted block was still found in less than one complete target
 * interval, the immediately previous difficulty has not demonstrated that it
 * is too hard. Older rolling-history/short-window history is therefore forbidden from
 * making the next block easier.
 *
 * This is deliberately asymmetric with the tightening brake above:
 *
 *   - newest trusted interval >= spacing: may veto another tightening step
 *   - newest trusted interval <  spacing: may veto an easing step
 *
 * Neither veto computes a replacement difficulty. It only prevents stale
 * history from moving in a direction contradicted by the newest observation.
 */
bool RecentBlockVetoesEasing(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (pindexLast->pprev == nullptr || params.nPowTargetSpacing <= 0) {
        return false;
    }

    if (!RawClockTrustedForModeration(pindexLast, params)) {
        return false;
    }

    int64_t nRawInterval =
        pindexLast->GetBlockTime() - pindexLast->pprev->GetBlockTime();

    if (nRawInterval < 0) {
        nRawInterval = 0;
    }

    return nRawInterval < params.nPowTargetSpacing;
}

/**
 * Six-interval difficulty-normalized raw-time target estimate.
 *
 * For interval i ending in block i:
 *
 *     expected normalized time contribution ~ delta_i * target_i
 *
 * Therefore the equilibrium target estimator is:
 *
 *     T_fast = sum(delta_i * target_i) / (N * target_spacing)
 *
 * This is the integer target-space form of normalizing every observed mining
 * interval by the difficulty at which that block was actually mined. Unlike a
 * moving target average, it does NOT mistake our own recent difficulty rises
 * for newly-arrived hashrate.
 *
 * The short estimate never directly makes work easier. Negative/backdated
 * deltas are treated as zero, which can only make the short path tighten.
 */
bool RecentRawTargetEstimate(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params,
    arith_uint256& bnEstimate)
{
    assert(pindexLast != nullptr);

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    if (pindexLast->nHeight < FAST_INTERVALS) {
        return false;
    }

    const uint64_t nExpectedTimespan = static_cast<uint64_t>(
        FAST_INTERVALS * params.nPowTargetSpacing);

    if (nExpectedTimespan == 0) {
        return false;
    }

    const CBlockIndex* pindex = pindexLast;
    arith_uint256 bnSum{0};

    for (int64_t n = 0; n < FAST_INTERVALS; ++n) {
        if (pindex == nullptr || pindex->pprev == nullptr) {
            return false;
        }

        bool fNegative{false};
        bool fOverflow{false};
        arith_uint256 bnTarget;

        bnTarget.SetCompact(
            pindex->nBits,
            &fNegative,
            &fOverflow);

        if (fNegative ||
            fOverflow ||
            bnTarget == 0 ||
            bnTarget > bnPowLimit) {
            return false;
        }

        int64_t nDelta =
            pindex->GetBlockTime() - pindex->pprev->GetBlockTime();

        if (nDelta < 0) {
            nDelta = 0;
        }

        const arith_uint256 bnContribution = ScaleTargetByTimespan(
            bnTarget,
            static_cast<uint64_t>(nDelta),
            nExpectedTimespan,
            bnPowLimit);

        if (bnContribution >= bnPowLimit - bnSum) {
            bnEstimate = bnPowLimit;
            return true;
        }

        bnSum += bnContribution;
        pindex = pindex->pprev;
    }

    if (bnSum == 0) {
        bnSum = 1;
    }

    bnEstimate = bnSum > bnPowLimit ? bnPowLimit : bnSum;
    return true;
}


/**
 * Return true when pindex was eligible for deterministic emergency recovery.
 *
 * This is intentionally inferred from committed chain data instead of being
 * stored as extra consensus state. If the block timestamp proves at least the
 * configured guaranteed stall against its predecessor's recent timestamp
 * anchor, GetNextWorkRequired() necessarily evaluated the emergency path.
 */
bool IsEmergencyRecoveryBlock(
    const CBlockIndex* pindex,
    const Consensus::Params& params)
{
    if (pindex == nullptr || pindex->pprev == nullptr) {
        return false;
    }

    if (pindex->nHeight < params.nShockWaveActivationHeight) {
        return false;
    }

    const int64_t nAnchor = RecentTimestampAnchor(pindex->pprev, params);
    const int64_t nBlockTime = pindex->GetBlockTime();

    if (nBlockTime <= nAnchor) {
        return false;
    }

    const int64_t nDelta = nBlockTime - nAnchor;

    if (nDelta <= MAX_FUTURE_BLOCK_TIME) {
        return false;
    }

    const int64_t nGuaranteedStall =
        nDelta - MAX_FUTURE_BLOCK_TIME;

    return nGuaranteedStall >= STALL_RECOVERY_TRIGGER;
}

/**
 * Locate the newest emergency recovery block whose post-recovery timing can
 * still affect the next target.
 *
 * Once the tip reaches recovery+29, the ordinary 25/24 MTP window begins at
 * recovery+5 and therefore contains no pre-recovery MTP endpoint. At that
 * point normal rolling ShockWave baseline is clean again and no special refill path is
 * necessary.
 */
const CBlockIndex* FindRecentRegimeResetBlock(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const CBlockIndex* pindex = pindexLast;
    int64_t nDistance{0};

    while (pindex != nullptr &&
           nDistance < RECOVERY_REFILL_DISTANCE) {
        if (IsEmergencyRecoveryBlock(pindex, params) ||
            IsOvershootRegimeBreakBlock(pindex, params)) {
            return pindex;
        }

        ++nDistance;
        pindex = pindex->pprev;
    }

    return nullptr;
}

/**
 * Clamp a normal/refill target to the true +/-4x per-block transition bounds.
 */
arith_uint256 ClampNormalTargetToPrevious(
    arith_uint256 bnTarget,
    const arith_uint256& bnPreviousTarget,
    const arith_uint256& bnPowLimit)
{
    arith_uint256 bnHardestTarget = bnPreviousTarget / 4;

    if (bnHardestTarget == 0) {
        bnHardestTarget = 1;
    }

    arith_uint256 bnEasiestTarget = bnPreviousTarget;

    if (bnEasiestTarget > bnPowLimit / 4) {
        bnEasiestTarget = bnPowLimit;
    } else {
        bnEasiestTarget *= 4;

        if (bnEasiestTarget > bnPowLimit) {
            bnEasiestTarget = bnPowLimit;
        }
    }

    if (bnTarget < bnHardestTarget) {
        bnTarget = bnHardestTarget;
    }

    if (bnTarget > bnEasiestTarget) {
        bnTarget = bnEasiestTarget;
    }

    if (bnTarget == 0) {
        bnTarget = 1;
    }

    if (bnTarget > bnPowLimit) {
        bnTarget = bnPowLimit;
    }

    return bnTarget;
}

/**
 * Post-reset expanding/rolling refill controller.
 *
 * Emergency-decay and overshoot resets deliberately treat their boundary
 * intervals differently:
 *
 *   - emergency reset: exclude the multi-hour outage interval; the emergency
 *     block itself becomes the clean new baseline
 *   - overshoot reset: KEEP the slowdown interval that triggered the reset; it
 *     is the first and most relevant observation of the new regime
 *
 * There is never a fixed-difficulty grace period. Difficulty is recalculated
 * every block with the full true +/-4x final transition authority as soon as
 * usable post-reset timing exists.
 *
 * Until recovery+24 the sampled target/timing window expands from the recovery
 * block. From recovery+24 through recovery+28 it becomes a rolling 25-block /
 * 24-interval window made entirely of post-recovery blocks. Raw interval time
 * is used during this refill because MTP still contains the stall/regime break.
 * Every raw interval is individually capped at 4 * target spacing, preventing
 * one large future timestamp jump from financing many cheap refill blocks.
 *
 * At recovery+29 the normal 25/24 MTP window begins at recovery+5, so both MTP
 * endpoints are in the post-recovery regime and ordinary ShockWave resumes.
 */
arith_uint256 RegimeRefillTarget(
    const CBlockIndex* pindexLast,
    const CBlockIndex* pindexRecovery,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    assert(pindexRecovery != nullptr);
    assert(pindexLast->nHeight >= pindexRecovery->nHeight);

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    bool fNegative{false};
    bool fOverflow{false};
    arith_uint256 bnPreviousTarget;

    bnPreviousTarget.SetCompact(
        pindexLast->nBits,
        &fNegative,
        &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnPreviousTarget == 0 ||
        bnPreviousTarget > bnPowLimit) {
        return bnPowLimit;
    }

    const int64_t nDistance =
        pindexLast->nHeight - pindexRecovery->nHeight;

    const bool fOvershootReset =
        IsOvershootRegimeBreakBlock(pindexRecovery, params);

    /*
     * The overshoot-triggering interval belongs to the NEW regime evidence.
     * Example: D=228k takes 894 seconds. That 894-second observation is exactly
     * why the old fast history was reset, so discarding it would make the first
     * lucky post-reset block disproportionately powerful.
     *
     * Emergency recovery is different: its boundary interval contains the
     * intentionally long outage and must not poison the fresh regime.
     */
    int nOldestHeight = pindexRecovery->nHeight;

    if (fOvershootReset && pindexRecovery->pprev != nullptr) {
        nOldestHeight = pindexRecovery->pprev->nHeight;
    } else if (nDistance == 0) {
        // No completed clean post-emergency interval exists yet.
        return bnPreviousTarget;
    }

    if (pindexLast->nHeight - nOldestHeight > SHOCKWAVE_ROLLING_INTERVALS) {
        nOldestHeight =
            pindexLast->nHeight - SHOCKWAVE_ROLLING_INTERVALS;
    }

    const CBlockIndex* pindexOldest =
        pindexLast->GetAncestor(nOldestHeight);

    const int nMinimumAllowedHeight =
        fOvershootReset && pindexRecovery->pprev != nullptr
            ? pindexRecovery->pprev->nHeight
            : pindexRecovery->nHeight;

    if (pindexOldest == nullptr ||
        pindexOldest->nHeight < nMinimumAllowedHeight) {
        return bnPreviousTarget;
    }

    const int64_t nIntervals =
        pindexLast->nHeight - pindexOldest->nHeight;

    if (nIntervals <= 0) {
        return bnPreviousTarget;
    }

    const int64_t nTargetTimespan =
        nIntervals * params.nPowTargetSpacing;

    /*
     * Refill uses the same normalized-interval mathematics as the short
     * controller instead of average_target * total_time. This prevents older
     * low-difficulty refill blocks from diluting a legitimate 4x response.
     */
    arith_uint256 bnWeightedTarget{0};
    const CBlockIndex* pindexInterval = pindexLast;

    while (pindexInterval != nullptr &&
           pindexInterval != pindexOldest) {
        const CBlockIndex* pindexPrev = pindexInterval->pprev;

        if (pindexPrev == nullptr) {
            return bnPreviousTarget;
        }

        bool fIntervalNegative{false};
        bool fIntervalOverflow{false};
        arith_uint256 bnIntervalTarget;

        bnIntervalTarget.SetCompact(
            pindexInterval->nBits,
            &fIntervalNegative,
            &fIntervalOverflow);

        if (fIntervalNegative ||
            fIntervalOverflow ||
            bnIntervalTarget == 0 ||
            bnIntervalTarget > bnPowLimit) {
            return bnPreviousTarget;
        }

        int64_t nDelta =
            pindexInterval->GetBlockTime() -
            pindexPrev->GetBlockTime();

        if (nDelta < 0) {
            nDelta = 0;
        }

        const int64_t nIntervalCap =
            RECOVERY_RAW_INTERVAL_CAP_MULTIPLIER *
            params.nPowTargetSpacing;

        if (nDelta > nIntervalCap) {
            nDelta = nIntervalCap;
        }

        const arith_uint256 bnContribution = ScaleTargetByTimespan(
            bnIntervalTarget,
            static_cast<uint64_t>(nDelta),
            static_cast<uint64_t>(nTargetTimespan),
            bnPowLimit);

        if (bnContribution >= bnPowLimit - bnWeightedTarget) {
            bnWeightedTarget = bnPowLimit;
            break;
        }

        bnWeightedTarget += bnContribution;
        pindexInterval = pindexPrev;
    }

    if (bnWeightedTarget == 0) {
        bnWeightedTarget = 1;
    }

    arith_uint256 bnNew = ClampNormalTargetToPrevious(
        bnWeightedTarget,
        bnPreviousTarget,
        bnPowLimit);

    /*
     * A newest trusted fast block may veto an EASING move during refill too.
     * This prevents a stale/expanding average from repeating the V8 failure
     * where a 110-second block at D65k was followed by a 4x easier target.
     */
    if (bnNew > bnPreviousTarget &&
        RecentBlockVetoesEasing(pindexLast, params)) {
        return bnPreviousTarget;
    }

    return bnNew;
}

/**
 * Select normal rolling ShockWave baseline or the expanding post-recovery controller.
 */
arith_uint256 ShockWaveTarget(
    const CBlockIndex* pindexLast,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    const CBlockIndex* pindexReset =
        FindRecentRegimeResetBlock(pindexLast, params);

    if (pindexReset != nullptr) {
        return RegimeRefillTarget(
            pindexLast,
            pindexReset,
            params);
    }

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    bool fNegative{false};
    bool fOverflow{false};
    arith_uint256 bnPreviousTarget;

    bnPreviousTarget.SetCompact(
        pindexLast->nBits,
        &fNegative,
        &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnPreviousTarget == 0 ||
        bnPreviousTarget > bnPowLimit) {
        return bnPowLimit;
    }

    const arith_uint256 bnRollingTarget =
        ShockWaveRollingTarget(pindexLast, params);

    arith_uint256 bnFastTarget;

    if (!RecentRawTargetEstimate(
            pindexLast,
            params,
            bnFastTarget)) {
        return bnRollingTarget;
    }

    const bool fRecentTighteningBrake =
        RecentBlockVetoesTightening(pindexLast, params);
    const bool fRecentEasingBrake =
        RecentBlockVetoesEasing(pindexLast, params);

    arith_uint256 bnHardestTarget = bnPreviousTarget / 4;

    if (bnHardestTarget == 0) {
        bnHardestTarget = 1;
    }

    /*
     * ACTIVE RATCHET.
     *
     * Once an aggressive >=2x tightening episode is underway, an
     * unmistakably-fast newest block gets priority over the six-interval
     * estimator. This prevents old difficulty-1/4 observations from diluting
     * a legitimate 1 -> 4 -> 16 -> 64 -> ... response.
     *
     * The ratchet is intentionally strict: the newest trusted interval must be
     * < 1/8 target spacing, or the raw clock must be suspiciously future-ahead
     * (which is not allowed to suppress tightening).
     */
    if (!fRecentTighteningBrake &&
        RecentBlockDemandsRatchet(pindexLast, params)) {
        return bnHardestTarget;
    }

    /*
     * PANIC GEAR.
     *
     * If the difficulty-normalized newest six intervals say equilibrium is at
     * least 4x harder than CURRENT difficulty, use the complete legal 4x move.
     * This is what V5 failed to do: the 25-block target average diluted the
     * signal into 16 -> 45.96 instead of 16 -> 64.
     */
    if (!fRecentTighteningBrake &&
        bnFastTarget <= bnHardestTarget) {
        return bnHardestTarget;
    }

    /*
     * CAPTURE GEAR.
     *
     * Once less than 4x away but still at least 2x too easy, move directly to
     * the six-interval normalized estimate. That estimate already accounts for
     * each sampled block's actual assigned difficulty, so it naturally stops
     * tightening as the chain approaches equilibrium instead of recursively
     * treating our own harder blocks as new hashrate.
     */
    arith_uint256 bnTwoTimesHarder = bnPreviousTarget / 2;

    if (bnTwoTimesHarder == 0) {
        bnTwoTimesHarder = 1;
    }

    if (!fRecentTighteningBrake &&
        bnFastTarget <= bnTwoTimesHarder) {
        return bnFastTarget < bnHardestTarget
            ? bnHardestTarget
            : bnFastTarget;
    }

    const bool fRawClockTrusted =
        RawClockTrustedForModeration(pindexLast, params);

    /*
     * Near equilibrium, ShockWave is the stable controller and the raw estimate is
     * only a consistency bound.
     *
     * - A raw clock suspiciously far AHEAD of projected MTP cannot itself veto
     *   tightening or authorize easier work.
     * - While the newest raw clock is untrusted, recent intervals that were
     *   independently trusted may still veto stale rolling-history tightening.
     * - The same untrusted-clock state may ease only when MTP rolling baseline AND at least
     *   four fully trusted recent intervals independently agree that current
     *   difficulty is too hard; the harder of those two targets wins.
     * - With a consistent raw clock, stale rolling-history history may not tighten beyond
     *   what the newest normalized intervals support.
     * - rolling-baseline easing requires the newest normalized intervals to agree that
     *   current difficulty is actually too hard.
     */
    if (bnRollingTarget < bnPreviousTarget) {
        /*
         * OVERSHOOT BRAKE.
         *
         * The newest block was mined at the immediately previous difficulty.
         * If that block already required at least one complete target interval,
         * stale cheap blocks elsewhere in the rolling window are forbidden from
         * demanding still-harder work. Hold exactly flat for this block.
         *
         * This is intentionally one-way: the short raw signal can cancel an
         * increase but cannot create an easier target.
         */
        if (fRecentTighteningBrake) {
            return bnPreviousTarget;
        }

        if (!fRawClockTrusted) {
            if (RecentTrustedWindowVetoesTightening(
                    pindexLast,
                    params,
                    bnPreviousTarget)) {
                return bnPreviousTarget;
            }

            return bnRollingTarget;
        }

        if (bnFastTarget >= bnPreviousTarget) {
            return bnPreviousTarget;
        }

        // Smaller target is harder. Do not tighten past the fast estimate.
        return bnRollingTarget < bnFastTarget
            ? bnFastTarget
            : bnRollingTarget;
    }

    if (bnRollingTarget > bnPreviousTarget) {
        /*
         * FAST-BLOCK EASING VETO.
         *
         * If the newest trusted block still arrived faster than target, stale
         * rolling-history/short history is not permitted to make difficulty easier. The
         * newest interval does not select a harder target here; it only says
         * that easing is currently unsupported.
         */
        if (fRecentEasingBrake) {
            return bnPreviousTarget;
        }

        if (!fRawClockTrusted) {
            arith_uint256 bnTrustedEasingTarget;

            if (!RecentTrustedWindowSupportsEasing(
                    pindexLast,
                    params,
                    bnPreviousTarget,
                    bnTrustedEasingTarget)) {
                return bnPreviousTarget;
            }

            // Both independent signals support easier work. The untrusted newest
            // raw interval contributes nothing; do not ease past either MTP rolling baseline
            // or the fully trusted recent-history estimate.
            return bnRollingTarget > bnTrustedEasingTarget
                ? bnTrustedEasingTarget
                : bnRollingTarget;
        }

        if (bnFastTarget <= bnPreviousTarget) {
            return bnPreviousTarget;
        }

        // Larger target is easier. Do not ease past the fast estimate.
        return bnRollingTarget > bnFastTarget
            ? bnFastTarget
            : bnRollingTarget;
    }

    return bnPreviousTarget;
}
/**
 * Multiply a target by 4/3 using integer arithmetic only.
 *
 * This is one 25% difficulty reduction. The result is saturated at powLimit.
 * The quotient/remainder formulation avoids an overflow-prone target * 4
 * intermediate even if a network selected a very large powLimit.
 */
arith_uint256 EaseTargetByTwentyFivePercent(
    const arith_uint256& bnTarget,
    const arith_uint256& bnPowLimit)
{
    if (bnTarget >= bnPowLimit) {
        return bnPowLimit;
    }

    // If floor(4 * target / 3) would reach or exceed powLimit, saturate.
    const arith_uint256 bnThreeQuarterPowLimit =
        bnPowLimit - (bnPowLimit / 4);

    if (bnTarget >= bnThreeQuarterPowLimit) {
        return bnPowLimit;
    }

    const arith_uint256 bnQuotient = bnTarget / 3;
    const arith_uint256 bnRemainder =
        bnTarget - (bnQuotient * 3);

    arith_uint256 bnNew = bnQuotient * 4;
    bnNew += bnRemainder;

    if (bnNew == 0) {
        bnNew = 1;
    }

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew;
}

/**
 * Apply deterministic emergency stall recovery to the normal ShockWave target.
 *
 * Consensus cannot use when a particular node first received a block. The
 * candidate header timestamp is deterministic, but Bitcoin Core permits it to
 * be up to MAX_FUTURE_BLOCK_TIME ahead of node time. Therefore the complete
 * future-time allowance is subtracted before any stall time is trusted.
 *
 * guaranteed_stall = candidate_time
 *                  - recent_timestamp_anchor
 *                  - MAX_FUTURE_BLOCK_TIME
 *
 * No emergency easing is permitted until 30 minutes of guaranteed stall have
 * accumulated. At 30 minutes the first 25% difficulty reduction is permitted,
 * followed by another 25% reduction for every additional 5 minutes.
 *
 * The emergency base is the easier of the previous block's target and normal
 * ShockWave's target. Consequently this path can never tighten difficulty.
 */
arith_uint256 ApplyEmergencyStallRecovery(
    const CBlockIndex* pindexLast,
    const CBlockHeader* pblock,
    const Consensus::Params& params,
    const arith_uint256& bnNormalTarget)
{
    assert(pindexLast != nullptr);
    assert(pblock != nullptr);

    const arith_uint256 bnPowLimit =
        UintToArith256(params.powLimit);

    bool fNegative{false};
    bool fOverflow{false};

    arith_uint256 bnPreviousTarget;
    bnPreviousTarget.SetCompact(
        pindexLast->nBits,
        &fNegative,
        &fOverflow);

    if (fNegative ||
        fOverflow ||
        bnPreviousTarget == 0 ||
        bnPreviousTarget > bnPowLimit) {
        return bnNormalTarget;
    }

    const int64_t nAnchor = RecentTimestampAnchor(pindexLast, params);
    const int64_t nCandidateTime = pblock->GetBlockTime();

    // Avoid overflow and reject any candidate that cannot possibly prove a
    // stall beyond the full future-time uncertainty budget.
    if (nCandidateTime <= nAnchor ||
        nCandidateTime - nAnchor <= MAX_FUTURE_BLOCK_TIME) {
        return bnNormalTarget;
    }

    const int64_t nGuaranteedStall =
        nCandidateTime - nAnchor - MAX_FUTURE_BLOCK_TIME;

    if (nGuaranteedStall < STALL_RECOVERY_TRIGGER) {
        return bnNormalTarget;
    }

    uint64_t nSteps = 1 + static_cast<uint64_t>(
        (nGuaranteedStall - STALL_RECOVERY_TRIGGER) /
        STALL_RECOVERY_STEP);

    arith_uint256 bnEmergencyTarget = bnNormalTarget;

    if (bnPreviousTarget > bnEmergencyTarget) {
        bnEmergencyTarget = bnPreviousTarget;
    }

    // The loop is naturally bounded by powLimit. Even a candidate timestamp
    // years after the tip cannot force unbounded consensus work.
    while (nSteps > 0 && bnEmergencyTarget < bnPowLimit) {
        bnEmergencyTarget = EaseTargetByTwentyFivePercent(
            bnEmergencyTarget,
            bnPowLimit);
        --nSteps;
    }

    return bnEmergencyTarget;
}

} // namespace

unsigned int GetNextWorkRequired(
    const CBlockIndex* pindexLast,
    const CBlockHeader* pblock,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    assert(pblock != nullptr);

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    /*
     * Consensus fork boundary.
     *
     * If nShockWaveActivationHeight == H:
     *
     *     block H - 1  -> legacy Bitcoin difficulty rules
     *     block H      -> first ShockWave-calculated difficulty
     *     block H + 1  -> ShockWave
     */
    if (IsShockWaveEnabledForNextBlock(pindexLast, params)) {
        const arith_uint256 bnNormalTarget =
            ShockWaveTarget(pindexLast, params);

        return ApplyEmergencyStallRecovery(
            pindexLast,
            pblock,
            params,
            bnNormalTarget).GetCompact();
    }

    return BitcoinGetNextWorkRequired(
        pindexLast,
        pblock,
        params);
}

unsigned int CalculateNextWorkRequired(
    const CBlockIndex* pindexLast,
    int64_t nFirstBlockTime,
    const Consensus::Params& params)
{
    assert(pindexLast != nullptr);

    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }

    /*
     * CalculateNextWorkRequired() has no candidate block header and therefore
     * cannot evaluate a NEW candidate-time stall event. It can, however,
     * deterministically infer an already-mined recovery block from committed
     * history and therefore returns the same post-recovery refill target as
     * GetNextWorkRequired() before candidate-time emergency easing is applied.
     */
    if (IsShockWaveEnabledForNextBlock(pindexLast, params)) {
        return ShockWaveTarget(pindexLast, params).GetCompact();
    }

    return BitcoinCalculateNextWorkRequired(
        pindexLast,
        nFirstBlockTime,
        params);
}

// Check that on difficulty adjustments, the new difficulty does not increase
// or decrease beyond the permitted limits.
bool PermittedDifficultyTransition(
    const Consensus::Params& params,
    int64_t height,
    uint32_t old_nbits,
    uint32_t new_nbits)
{
    if (params.fPowAllowMinDifficultyBlocks) {
        return true;
    }

    /*
     * ShockWave and post-recovery refill calculations require contextual chain
     * history, and emergency recovery may legitimately ease by more than the
     * normal per-block 4x bound. PermittedDifficultyTransition() is also used
     * where that complete context is unavailable, so after the ShockWave fork this
     * function performs only target-range validation.
     *
     * Full contextual block/header validation still verifies that nBits
     * exactly equals GetNextWorkRequired().
     */
    if (height >= params.nShockWaveActivationHeight) {
        return DeriveTarget(
            new_nbits,
            params.powLimit).has_value();
    }

    /*
     * Pre-fork Bitcoin difficulty-transition validation.
     */
    if (height % params.DifficultyAdjustmentInterval() == 0) {
        const int64_t smallest_timespan =
            params.nPowTargetTimespan / 4;

        const int64_t largest_timespan =
            params.nPowTargetTimespan * 4;

        const arith_uint256 pow_limit =
            UintToArith256(params.powLimit);

        arith_uint256 observed_new_target;
        observed_new_target.SetCompact(new_nbits);

        // Calculate the largest target permitted.
        arith_uint256 largest_difficulty_target;
        largest_difficulty_target.SetCompact(old_nbits);
        largest_difficulty_target *= largest_timespan;
        largest_difficulty_target /=
            params.nPowTargetTimespan;

        if (largest_difficulty_target > pow_limit) {
            largest_difficulty_target = pow_limit;
        }

        // Compact representation rounds the target, so round before comparing.
        arith_uint256 maximum_new_target;
        maximum_new_target.SetCompact(
            largest_difficulty_target.GetCompact());

        if (maximum_new_target < observed_new_target) {
            return false;
        }

        // Calculate the smallest target permitted.
        arith_uint256 smallest_difficulty_target;
        smallest_difficulty_target.SetCompact(old_nbits);
        smallest_difficulty_target *= smallest_timespan;
        smallest_difficulty_target /=
            params.nPowTargetTimespan;

        if (smallest_difficulty_target > pow_limit) {
            smallest_difficulty_target = pow_limit;
        }

        // Compact representation rounds the target, so round before comparing.
        arith_uint256 minimum_new_target;
        minimum_new_target.SetCompact(
            smallest_difficulty_target.GetCompact());

        if (minimum_new_target > observed_new_target) {
            return false;
        }
    } else if (old_nbits != new_nbits) {
        return false;
    }

    return true;
}

// Bypasses the actual proof-of-work check during fuzz testing with a
// simplified validation checking whether the most significant bit of the
// last byte of the hash is set.
bool CheckProofOfWork(
    uint256 hash,
    unsigned int nBits,
    const Consensus::Params& params)
{
    if (EnableFuzzDeterminism()) {
        return (hash.data()[31] & 0x80) == 0;
    }

    return CheckProofOfWorkImpl(
        hash,
        nBits,
        params);
}

std::optional<arith_uint256> DeriveTarget(
    unsigned int nBits,
    const uint256 pow_limit)
{
    bool fNegative;
    bool fOverflow;

    arith_uint256 bnTarget;

    bnTarget.SetCompact(
        nBits,
        &fNegative,
        &fOverflow);

    // Check range.
    if (fNegative ||
        bnTarget == 0 ||
        fOverflow ||
        bnTarget > UintToArith256(pow_limit)) {
        return {};
    }

    return bnTarget;
}

bool CheckProofOfWorkImpl(
    uint256 hash,
    unsigned int nBits,
    const Consensus::Params& params)
{
    auto bnTarget{
        DeriveTarget(nBits, params.powLimit)
    };

    if (!bnTarget) {
        return false;
    }

    // Check proof of work matches claimed amount.
    if (UintToArith256(hash) > bnTarget) {
        return false;
    }

    return true;
}
