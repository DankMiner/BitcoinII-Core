// Copyright (c) 2022-present The Bitcoin Core developers
// Copyright (c) 2026 The BitcoinII Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <headerssync.h>

#include <logging.h>
#include <pow.h>
#include <util/check.h>
#include <util/time.h>
#include <util/vector.h>

#include <algorithm>
#include <memory>
#include <vector>

// BitcoinII ShockWave rolling baseline examines 25 prior targets. The MTP of the oldest target can
// depend on ten additional predecessors, so exact reproduction requires
// 35 CBlockIndex entries.
constexpr size_t SHOCKWAVE_LOOKBACK_BLOCKS{25};

constexpr size_t SHOCKWAVE_MTP_EXTRA_BLOCKS{
    CBlockIndex::nMedianTimeSpan - 1
};

constexpr size_t SHOCKWAVE_HISTORY_SIZE{
    SHOCKWAVE_LOOKBACK_BLOCKS + SHOCKWAVE_MTP_EXTRA_BLOCKS
};

// Our memory analysis in headerssync-params.py assumes this many bytes for a
// CompressedHeader (we should re-calculate parameters if we compress further).
static_assert(sizeof(CompressedHeader) == 48);

HeadersSyncState::HeadersSyncState(NodeId id,
                                   const Consensus::Params& consensus_params,
                                   const HeadersSyncParams& params,
                                   const CBlockIndex& chain_start,
                                   const arith_uint256& minimum_required_work)
    : m_commit_offset((assert(params.commitment_period > 0), // HeadersSyncParams field must be initialized to non-zero.
                       FastRandomContext().randrange(params.commitment_period))),
      m_id(id),
      m_consensus_params(consensus_params),
      m_params(params),
      m_chain_start(chain_start),
      m_minimum_required_work(minimum_required_work),
      m_current_chain_work(chain_start.nChainWork),
      m_last_header_received(m_chain_start.GetBlockHeader()),
      m_current_height(chain_start.nHeight)
{
    /*
     * Seed the rolling temporary CBlockIndex chain from the real chain.
     *
     * Pre-fork headers are appended to this history as PRESYNC progresses so
     * that when nShockWaveActivationHeight is reached, the first ShockWave calculation has
     * the complete 25-target / MedianTimePast history available.
     */
    ResetShockWaveHistory(m_presync_shockwave_history);

    // Estimate the number of blocks that could possibly exist on the peer's
    // chain *right now* using 6 blocks/second (fastest blockrate given the MTP
    // rule) times the number of seconds from the last allowed block until
    // today. This serves as a memory bound on how many commitments we might
    // store from this peer, and we can safely give up syncing if the peer
    // exceeds this bound, because it's not possible for a consensus-valid
    // chain to be longer than this (at the current time -- in the future we
    // could try again, if necessary, to sync a longer chain).
    const auto max_seconds_since_start{
        (Ticks<std::chrono::seconds>(
             NodeClock::now() -
             NodeSeconds{
                 std::chrono::seconds{
                     chain_start.GetMedianTimePast()
                 }
             }
         ) + MAX_FUTURE_BLOCK_TIME)
    };

    m_max_commitments =
        6 * max_seconds_since_start /
        m_params.commitment_period;

    LogDebug(
        BCLog::NET,
        "Initial headers sync started with peer=%d: "
        "height=%i, max_commitments=%i, min_work=%s\n",
        m_id,
        m_current_height,
        m_max_commitments,
        m_minimum_required_work.ToString()
    );
}

/** Free any memory in use, and mark this object as no longer usable. This is
 * required to guarantee that we won't reuse this object with the same
 * SaltedUint256Hasher for another sync. */
void HeadersSyncState::Finalize()
{
    Assume(m_download_state != State::FINAL);

    ClearShrink(m_header_commitments);
    ClearShrink(m_redownloaded_headers);

    m_last_header_received.SetNull();

    m_presync_shockwave_history.clear();
    m_redownload_shockwave_history.clear();

    m_redownload_buffer_last_hash.SetNull();
    m_redownload_buffer_first_prev_hash.SetNull();

    m_process_all_remaining_headers = false;
    m_current_height = 0;

    m_download_state = State::FINAL;
}

/**
 * Build a bounded temporary CBlockIndex history ending at m_chain_start.
 *
 * ShockWave rolling baseline examines 25 targets. Because each sampled block uses
 * GetMedianTimePast(), the oldest target can require ten additional
 * predecessors to reproduce its MTP exactly.
 *
 * Therefore:
 *
 *     25 ShockWave rolling targets
 *   + 10 additional MTP predecessors
 *   = 35 retained CBlockIndex entries
 *
 * These indexes are private to HeadersSyncState and never enter the global
 * block index.
 */
void HeadersSyncState::ResetShockWaveHistory(
    ShockWaveHistory& history
) const
{
    history.clear();

    std::vector<const CBlockIndex*> ancestors;
    ancestors.reserve(SHOCKWAVE_HISTORY_SIZE);

    const CBlockIndex* pindex = &m_chain_start;

    while (pindex != nullptr &&
           ancestors.size() < SHOCKWAVE_HISTORY_SIZE) {
        ancestors.push_back(pindex);
        pindex = pindex->pprev;
    }

    /*
     * We walked newest -> oldest. Reverse so the temporary pprev chain can
     * be constructed oldest -> newest.
     */
    std::reverse(
        ancestors.begin(),
        ancestors.end()
    );

    for (const CBlockIndex* ancestor : ancestors) {
        auto index{
            std::make_unique<CBlockIndex>(
                ancestor->GetBlockHeader()
            )
        };

        index->nHeight = ancestor->nHeight;

        index->pprev =
            history.empty()
                ? nullptr
                : history.back().get();

        history.push_back(
            std::move(index)
        );
    }
}

/**
 * Validate a ShockWave-era header using the exact production difficulty algorithm.
 *
 * A simple comparison between the previous target and candidate target cannot
 * correctly validate ShockWave because ShockWave rolling baseline depends on a moving window of historical
 * targets and MedianTimePast values.
 *
 * The bounded synthetic CBlockIndex chain gives GetNextWorkRequired() the same
 * information it would receive during normal contextual header validation.
 */
bool HeadersSyncState::ValidateShockWaveHeader(
    const CBlockHeader& header,
    const int64_t next_height,
    const ShockWaveHistory& history,
    const char* phase
) const
{
    if (history.empty()) {
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "missing ShockWave history at height=%i (%s phase)\n",
            m_id,
            next_height,
            phase
        );

        return false;
    }

    const CBlockIndex* const previous{
        history.back().get()
    };

    /*
     * Make sure our synthetic chain and the headers-sync height counter have
     * not diverged.
     */
    if (previous->nHeight + 1 != next_height) {
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "inconsistent ShockWave history at height=%i (%s phase)\n",
            m_id,
            next_height,
            phase
        );

        return false;
    }

    /*
     * ShockWave rolling baseline uses MedianTimePast internally. Enforce the normal MTP relationship
     * here as well so an untrusted PRESYNC chain cannot manufacture timing
     * history that would never be accepted by contextual validation.
     */
    const int64_t previous_mtp{
        previous->GetMedianTimePast()
    };

    if (static_cast<int64_t>(header.nTime) <= previous_mtp) {
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "timestamp %u is not greater than previous MTP %i "
            "at height=%i (%s phase)\n",
            m_id,
            header.nTime,
            previous_mtp,
            next_height,
            phase
        );

        return false;
    }

    /*
     * Because this function is only called when next_height is at or above
     * nShockWaveActivationHeight, GetNextWorkRequired() will select ShockWave.
     */
    const uint32_t expected_nbits{
        GetNextWorkRequired(
            previous,
            &header,
            m_consensus_params
        )
    };

    if (header.nBits != expected_nbits) {
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "invalid rolling target at height=%i, "
            "expected=%08x received=%08x (%s phase)\n",
            m_id,
            next_height,
            expected_nbits,
            header.nBits,
            phase
        );

        return false;
    }

    return true;
}

/**
 * Append a validated header to the temporary ShockWave history.
 *
 * The history remains bounded at SHOCKWAVE_HISTORY_SIZE entries. When the oldest
 * entry is removed, the new oldest entry's pprev pointer is cleared so that
 * it does not retain a dangling pointer.
 */
void HeadersSyncState::AppendShockWaveHeader(
    ShockWaveHistory& history,
    const CBlockHeader& header,
    const int64_t height
)
{
    auto index{
        std::make_unique<CBlockIndex>(header)
    };

    index->nHeight = height;

    index->pprev =
        history.empty()
            ? nullptr
            : history.back().get();

    history.push_back(
        std::move(index)
    );

    while (history.size() > SHOCKWAVE_HISTORY_SIZE) {
        history.pop_front();
    }

    /*
     * The oldest retained index is never required to walk farther backward
     * than the retained 35-index window.
     */
    if (!history.empty()) {
        history.front()->pprev = nullptr;
    }
}

/** Process the next batch of headers received from our peer.
 *  Validate and store commitments, and compare total chainwork to our target to
 *  see if we can switch to REDOWNLOAD mode.  */
HeadersSyncState::ProcessingResult HeadersSyncState::ProcessNextHeaders(
        std::span<const CBlockHeader> received_headers,
        const bool full_headers_message)
{
    ProcessingResult ret;

    Assume(!received_headers.empty());
    if (received_headers.empty()) return ret;

    Assume(m_download_state != State::FINAL);
    if (m_download_state == State::FINAL) return ret;

    if (m_download_state == State::PRESYNC) {
        // During PRESYNC, we minimally validate block headers and
        // occasionally add commitments to them, until we reach our work
        // threshold (at which point m_download_state is updated to REDOWNLOAD).
        ret.success =
            ValidateAndStoreHeadersCommitments(
                received_headers
            );

        if (ret.success) {
            if (full_headers_message ||
                m_download_state == State::REDOWNLOAD) {
                // A full headers message means the peer may have more to give us;
                // also if we just switched to REDOWNLOAD then we need to re-request
                // headers from the beginning.
                ret.request_more = true;
            } else {
                Assume(m_download_state == State::PRESYNC);

                // If we're in PRESYNC and we get a non-full headers
                // message, then the peer's chain has ended and definitely doesn't
                // have enough work, so we can stop our sync.
                LogDebug(
                    BCLog::NET,
                    "Initial headers sync aborted with peer=%d: "
                    "incomplete headers message at height=%i "
                    "(presync phase)\n",
                    m_id,
                    m_current_height
                );
            }
        }
    } else if (m_download_state == State::REDOWNLOAD) {
        // During REDOWNLOAD, we compare our stored commitments to what we
        // receive, and add headers to our redownload buffer. When the buffer
        // gets big enough (meaning that we've checked enough commitments),
        // we'll return a batch of headers to the caller for processing.
        ret.success = true;

        for (const auto& hdr : received_headers) {
            if (!ValidateAndStoreRedownloadedHeader(hdr)) {
                // Something went wrong -- the peer gave us an unexpected chain.
                // We could consider looking at the reason for failure and
                // punishing the peer, but for now just give up on sync.
                ret.success = false;
                break;
            }
        }

        if (ret.success) {
            // Return any headers that are ready for acceptance.
            ret.pow_validated_headers =
                PopHeadersReadyForAcceptance();

            // If we hit our target blockhash, then all remaining headers will be
            // returned and we can clear any leftover internal state.
            if (m_redownloaded_headers.empty() &&
                m_process_all_remaining_headers) {
                LogDebug(
                    BCLog::NET,
                    "Initial headers sync complete with peer=%d: "
                    "releasing all at height=%i "
                    "(redownload phase)\n",
                    m_id,
                    m_redownload_buffer_last_height
                );
            } else if (full_headers_message) {
                // If the headers message is full, we need to request more.
                ret.request_more = true;
            } else {
                // For some reason our peer gave us a high-work chain, but is now
                // declining to serve us that full chain again. Give up.
                // Note that there's no more processing to be done with these
                // headers, so we can still return success.
                LogDebug(
                    BCLog::NET,
                    "Initial headers sync aborted with peer=%d: "
                    "incomplete headers message at height=%i "
                    "(redownload phase)\n",
                    m_id,
                    m_redownload_buffer_last_height
                );
            }
        }
    }

    if (!(ret.success && ret.request_more)) {
        Finalize();
    }

    return ret;
}

bool HeadersSyncState::ValidateAndStoreHeadersCommitments(
    std::span<const CBlockHeader> headers)
{
    // The caller should not give us an empty set of headers.
    Assume(headers.size() > 0);
    if (headers.size() == 0) return true;

    Assume(m_download_state == State::PRESYNC);
    if (m_download_state != State::PRESYNC) return false;

    if (headers[0].hashPrevBlock !=
        m_last_header_received.GetHash()) {
        // Somehow our peer gave us a header that doesn't connect.
        // This might be benign -- perhaps our peer reorged away from the chain
        // they were on. Give up on this sync for now (likely we will start a
        // new sync with a new starting point).
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "non-continuous headers at height=%i "
            "(presync phase)\n",
            m_id,
            m_current_height
        );

        return false;
    }

    // If it does connect, (minimally) validate and occasionally store
    // commitments.
    for (const auto& hdr : headers) {
        if (!ValidateAndProcessSingleHeader(hdr)) {
            return false;
        }
    }

    if (m_current_chain_work >=
        m_minimum_required_work) {
        m_redownloaded_headers.clear();

        m_redownload_buffer_last_height =
            m_chain_start.nHeight;

        m_redownload_buffer_first_prev_hash =
            m_chain_start.GetBlockHash();

        m_redownload_buffer_last_hash =
            m_chain_start.GetBlockHash();

        m_redownload_chain_work =
            m_chain_start.nChainWork;

        /*
         * REDOWNLOAD starts again from m_chain_start rather than continuing
         * from the end of PRESYNC, so it requires its own reconstructed ShockWave
         * history.
         */
        ResetShockWaveHistory(
            m_redownload_shockwave_history
        );

        m_download_state =
            State::REDOWNLOAD;

        LogDebug(
            BCLog::NET,
            "Initial headers sync transition with peer=%d: "
            "reached sufficient work at height=%i, "
            "redownloading from height=%i\n",
            m_id,
            m_current_height,
            m_redownload_buffer_last_height
        );
    }

    return true;
}

bool HeadersSyncState::ValidateAndProcessSingleHeader(
    const CBlockHeader& current)
{
    Assume(m_download_state == State::PRESYNC);
    if (m_download_state != State::PRESYNC) return false;

    const int64_t next_height{
        m_current_height + 1
    };

    /*
     * Before the hard fork, retain Bitcoin's existing lightweight
     * difficulty-transition validation.
     *
     * Beginning with nShockWaveActivationHeight, reproduce the exact ShockWave
     * calculation using our bounded synthetic CBlockIndex history.
     */
    if (next_height >=
        m_consensus_params.nShockWaveActivationHeight) {
        if (!ValidateShockWaveHeader(
                current,
                next_height,
                m_presync_shockwave_history,
                "presync")) {
            return false;
        }
    } else {
        // Verify that the difficulty isn't growing too fast; an adversary with
        // limited hashing capability has a greater chance of producing a high
        // work chain if they compress the work into as few blocks as possible,
        // so don't let anyone give a chain that would violate the difficulty
        // adjustment maximum.
        if (!PermittedDifficultyTransition(
                m_consensus_params,
                next_height,
                m_last_header_received.nBits,
                current.nBits)) {
            LogDebug(
                BCLog::NET,
                "Initial headers sync aborted with peer=%d: "
                "invalid difficulty transition at height=%i "
                "(presync phase)\n",
                m_id,
                next_height
            );

            return false;
        }
    }

    if (next_height %
            m_params.commitment_period ==
        m_commit_offset) {
        // Add a commitment.
        m_header_commitments.push_back(
            m_hasher(current.GetHash()) & 1
        );

        if (m_header_commitments.size() >
            m_max_commitments) {
            // The peer's chain is too long; give up.
            // It's possible the chain grew since we started the sync; so
            // potentially we could succeed in syncing the peer's chain if we
            // try again later.
            LogDebug(
                BCLog::NET,
                "Initial headers sync aborted with peer=%d: "
                "exceeded max commitments at height=%i "
                "(presync phase)\n",
                m_id,
                next_height
            );

            return false;
        }
    }

    m_current_chain_work +=
        GetBlockProof(current);

    /*
     * Append every header, including pre-fork headers. This guarantees that
     * the complete historical window is already available when the very first
     * ShockWave block is encountered.
     */
    AppendShockWaveHeader(
        m_presync_shockwave_history,
        current,
        next_height
    );

    m_last_header_received = current;
    m_current_height = next_height;

    return true;
}

bool HeadersSyncState::ValidateAndStoreRedownloadedHeader(
    const CBlockHeader& header)
{
    Assume(m_download_state == State::REDOWNLOAD);
    if (m_download_state != State::REDOWNLOAD) return false;

    const int64_t next_height{
        m_redownload_buffer_last_height + 1
    };

    // Ensure that we're working on a header that connects to the chain we're
    // downloading.
    if (header.hashPrevBlock !=
        m_redownload_buffer_last_hash) {
        LogDebug(
            BCLog::NET,
            "Initial headers sync aborted with peer=%d: "
            "non-continuous headers at height=%i "
            "(redownload phase)\n",
            m_id,
            next_height
        );

        return false;
    }

    /*
     * Use Bitcoin's original transition sanity check before activation and
     * exact ShockWave validation beginning with the activation block itself.
     */
    if (next_height >=
        m_consensus_params.nShockWaveActivationHeight) {
        if (!ValidateShockWaveHeader(
                header,
                next_height,
                m_redownload_shockwave_history,
                "redownload")) {
            return false;
        }
    } else {
        uint32_t previous_nBits{0};

        if (!m_redownloaded_headers.empty()) {
            previous_nBits =
                m_redownloaded_headers.back().nBits;
        } else {
            previous_nBits =
                m_chain_start.nBits;
        }

        if (!PermittedDifficultyTransition(
                m_consensus_params,
                next_height,
                previous_nBits,
                header.nBits)) {
            LogDebug(
                BCLog::NET,
                "Initial headers sync aborted with peer=%d: "
                "invalid difficulty transition at height=%i "
                "(redownload phase)\n",
                m_id,
                next_height
            );

            return false;
        }
    }

    // Track work on the redownloaded chain.
    m_redownload_chain_work +=
        GetBlockProof(header);

    if (m_redownload_chain_work >=
        m_minimum_required_work) {
        m_process_all_remaining_headers = true;
    }

    // If we're at a header for which we previously stored a commitment, verify
    // it is correct. Failure will result in aborting download.
    // Also, don't check commitments once we've gotten to our target blockhash;
    // it's possible our peer has extended its chain between our first sync and
    // our second, and we don't want to return failure after we've seen our
    // target blockhash just because we ran out of commitments.
    if (!m_process_all_remaining_headers &&
        next_height %
                m_params.commitment_period ==
            m_commit_offset) {
        if (m_header_commitments.size() == 0) {
            LogDebug(
                BCLog::NET,
                "Initial headers sync aborted with peer=%d: "
                "commitment overrun at height=%i "
                "(redownload phase)\n",
                m_id,
                next_height
            );

            // Somehow our peer managed to feed us a different chain and
            // we've run out of commitments.
            return false;
        }

        const bool commitment{
            static_cast<bool>(
                m_hasher(header.GetHash()) & 1
            )
        };

        const bool expected_commitment{
            m_header_commitments.front()
        };

        m_header_commitments.pop_front();

        if (commitment !=
            expected_commitment) {
            LogDebug(
                BCLog::NET,
                "Initial headers sync aborted with peer=%d: "
                "commitment mismatch at height=%i "
                "(redownload phase)\n",
                m_id,
                next_height
            );

            return false;
        }
    }

    /*
     * Keep the separate REDOWNLOAD ShockWave history moving forward regardless of
     * whether this particular header is pre- or post-fork. That gives the
     * activation block its complete historical lookback.
     */
    AppendShockWaveHeader(
        m_redownload_shockwave_history,
        header,
        next_height
    );

    // Store this header for later processing.
    m_redownloaded_headers.emplace_back(header);

    m_redownload_buffer_last_height =
        next_height;

    m_redownload_buffer_last_hash =
        header.GetHash();

    return true;
}

std::vector<CBlockHeader>
HeadersSyncState::PopHeadersReadyForAcceptance()
{
    std::vector<CBlockHeader> ret;

    Assume(m_download_state == State::REDOWNLOAD);
    if (m_download_state != State::REDOWNLOAD) return ret;

    while (m_redownloaded_headers.size() >
               m_params.redownload_buffer_size ||
           (m_redownloaded_headers.size() > 0 &&
            m_process_all_remaining_headers)) {
        ret.emplace_back(
            m_redownloaded_headers
                .front()
                .GetFullHeader(
                    m_redownload_buffer_first_prev_hash
                )
        );

        m_redownloaded_headers.pop_front();

        m_redownload_buffer_first_prev_hash =
            ret.back().GetHash();
    }

    return ret;
}

CBlockLocator
HeadersSyncState::NextHeadersRequestLocator() const
{
    Assume(m_download_state != State::FINAL);
    if (m_download_state == State::FINAL) return {};

    auto chain_start_locator{
        LocatorEntries(&m_chain_start)
    };

    std::vector<uint256> locator;

    if (m_download_state == State::PRESYNC) {
        // During pre-synchronization, we continue from the last header received.
        locator.push_back(
            m_last_header_received.GetHash()
        );
    }

    if (m_download_state == State::REDOWNLOAD) {
        // During redownload, we will download from the last received header
        // that we stored.
        locator.push_back(
            m_redownload_buffer_last_hash
        );
    }

    locator.insert(
        locator.end(),
        chain_start_locator.begin(),
        chain_start_locator.end()
    );

    return CBlockLocator{
        std::move(locator)
    };
}
