// Copyright (c) 2026 1Miner.net
// Licensed under LICENSE-1MINER-BC2-MATURITY.md, including its prior-grant exception.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/coinbase_maturity.h>
#include <consensus/params.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t REFERENCE_BITS{0x181cebcd};
constexpr int WARMUP_BLOCKS{600};

enum class HashSchedule { STEADY, PRE_REWARD_500, POST_REWARD_500, POST_REWARD_HOUR,
                          POST_REWARD_DAY, PERIODIC_100_OF_1000, EXIT_AT_REWARD, OUTAGE_DAY };
enum class ClockSchedule { HONEST, FUTURE_LIMIT, BEHIND_TWO_HOURS, ALTERNATE_FUTURE };

struct Scenario {
    std::string name;
    uint64_t base_milli{1000};
    uint64_t burst_milli{1000000};
    HashSchedule schedule{HashSchedule::STEADY};
    bool stochastic{false};
    uint64_t seed{0};
    ClockSchedule clock{ClockSchedule::HONEST};
};

struct Observation {
    int age{0};
    double elapsed_days{0};
    arith_uint256 work{};
    arith_uint256 work_during_rental{};
    int rental_blocks{0};
    double estimated_rental_hashes{0};
    uint64_t emergency_changes{0};
    uint64_t modeled_blocks{0};
};

/**
 * Synthetic, unmined header history; this is not a proof-of-work mining test.
 * The consensus DAA and maturity arithmetic are real. Only arrivals are modeled.
 *
 * The idealized arrival hazard is H / GetBitsProof(candidate_bits). Unit hazard
 * gives deterministic arrivals; -log(U) gives reproducible stochastic arrivals.
 * Hashrate is constant between the specified schedule boundaries. For a fixed
 * parent, ShockWave's candidate-time target is constant until emergency easing,
 * and then only gets easier. When a proposed arrival crosses a target change,
 * find its exact integer timestamp and integrate each constant-rate segment.
 * Thus an old target is never used to sample a block across emergency easing.
 *
 * Double precision is used only for the timing model, never consensus work.
 * Expected hashes use GetBitsProof's integer rounding. Header timestamps are
 * integer seconds. Clock scenarios idle until MTP permits their chosen clock;
 * they always remain within MAX_FUTURE_BLOCK_TIME of the modeled node clock.
 * Network propagation, selfish mining, orphan races, and hardware are unmodeled.
 */
class HashrateModel {
public:
    HashrateModel(const Scenario& scenario, Consensus::Params params)
        : m_scenario{scenario}, m_params{std::move(params)}, m_random{scenario.seed},
          m_reference_work{GetBitsProof(REFERENCE_BITS)},
          m_base_hashes_per_second{m_reference_work.getdouble() / 600.0}
    {
        BOOST_REQUIRE(!m_params.fPowNoRetargeting);
        BOOST_REQUIRE(!m_params.fPowAllowMinDifficultyBlocks);
        BOOST_REQUIRE_EQUAL(m_params.nPowTargetSpacing, 600);
        BOOST_REQUIRE_EQUAL(m_params.nCoinbaseMaturityMin, 4200);
        BOOST_REQUIRE_EQUAL(m_params.nCoinbaseMaturityMax, 12960);
        BOOST_REQUIRE(UintToArith256(m_params.nCoinbaseMaturityWork) == m_reference_work * 8579);
        m_reward_height = m_params.nShockWaveActivationHeight + WARMUP_BLOCKS - 1;
        m_params.nCoinbaseWorkMaturityActivationHeight = m_reward_height;

        // Seed a synthetic, steady pre-ShockWave history. These seed headers are
        // scaffolding, not claims about the historical BC2 blockchain.
        arith_uint256 initial_target;
        initial_target.SetCompact(REFERENCE_BITS);
        const uint64_t initial_rate{scenario.schedule == HashSchedule::EXIT_AT_REWARD
                                       ? scenario.burst_milli : scenario.base_milli};
        initial_target *= 1000;
        initial_target /= initial_rate;
        const uint32_t initial_bits{initial_target.GetCompact()};
        BOOST_REQUIRE(DeriveTarget(initial_bits, m_params.powLimit).has_value());
        for (int height = 0; height < m_params.nShockWaveActivationHeight; ++height) {
            Append(initial_bits, 1700000000 + height * 600);
        }
        m_time = m_chain.back().GetBlockTime();
    }

    Observation Run()
    {
        for (int i = 0; i < WARMUP_BLOCKS; ++i) NextBlock();
        BOOST_REQUIRE_EQUAL(m_chain.back().nHeight, m_reward_height);
        m_reward_time = m_time;
        const auto newborn{Consensus::GetCoinbaseMaturity(m_reward_height, m_chain.back(), m_params)};
        BOOST_REQUIRE(newborn.accumulated_work == 0);
        BOOST_REQUIRE(!newborn.mature);
        BOOST_REQUIRE(newborn.required_work == m_reference_work * 8579);

        Observation result;
        arith_uint256 independent_work{};
        for (int age = 1; age <= m_params.nCoinbaseMaturityMax; ++age) {
            const auto observed{Consensus::GetCoinbaseMaturity(m_reward_height, m_chain.back(), m_params)};
            const bool expected{age >= 4200 && (independent_work >= m_reference_work * 8579 || age >= 12960)};
            BOOST_REQUIRE_EQUAL(observed.mature, expected);
            BOOST_REQUIRE(observed.accumulated_work == independent_work);
            BOOST_REQUIRE_EQUAL(observed.blocks_to_minimum, std::max(0, 4200 - age));
            BOOST_REQUIRE_EQUAL(observed.blocks_to_maximum, std::max(0, 12960 - age));
            if (observed.mature) {
                result.age = age;
                result.elapsed_days = (m_time - m_reward_time) / 86400.0;
                result.work = independent_work;
                break;
            }
            NextBlock();
            const auto work{GetBlockProof(m_chain.back())};
            independent_work += work;
            if (m_last_rental) {
                result.work_during_rental += work;
                ++result.rental_blocks;
            }
        }
        BOOST_REQUIRE_GE(result.age, 4200);
        BOOST_REQUIRE_LE(result.age, 12960);
        if (result.age < 12960) BOOST_REQUIRE(result.work >= m_reference_work * 8579);
        result.estimated_rental_hashes = m_rental_hashes;
        result.emergency_changes = m_emergency_changes;
        result.modeled_blocks = m_modeled_blocks;
        return result;
    }

private:
    void Append(uint32_t bits, int64_t timestamp)
    {
        CBlockIndex* parent{m_chain.empty() ? nullptr : &m_chain.back()};
        auto& block{m_chain.emplace_back()};
        block.pprev = parent;
        block.nHeight = parent ? parent->nHeight + 1 : 0;
        block.nTime = timestamp;
        block.nBits = bits;
        block.nChainWork = (parent ? parent->nChainWork : arith_uint256{}) + GetBlockProof(block);
        block.BuildSkip();
    }

    int ClockOffset(int post_reward) const
    {
        if (post_reward <= 0) return 0;
        switch (m_scenario.clock) {
        case ClockSchedule::HONEST: return 0;
        case ClockSchedule::FUTURE_LIMIT: return MAX_FUTURE_BLOCK_TIME;
        case ClockSchedule::BEHIND_TWO_HOURS: return -MAX_FUTURE_BLOCK_TIME;
        case ClockSchedule::ALTERNATE_FUTURE: return (post_reward / 50) % 2 ? MAX_FUTURE_BLOCK_TIME : 0;
        }
        return 0;
    }

    // Returns the hashrate in thousandths of the reference rate, and the next
    // wall-time schedule boundary. Block-count schedules change between blocks.
    std::pair<uint64_t, double> Rate(int post_reward, double now) const
    {
        const double never{std::numeric_limits<double>::infinity()};
        const auto base{m_scenario.base_milli};
        const auto burst{m_scenario.burst_milli};
        switch (m_scenario.schedule) {
        case HashSchedule::STEADY: return {base, never};
        case HashSchedule::PRE_REWARD_500: return {post_reward >= -500 && post_reward < 0 ? burst : base, never};
        case HashSchedule::POST_REWARD_500: return {post_reward > 0 && post_reward <= 500 ? burst : base, never};
        case HashSchedule::PERIODIC_100_OF_1000: return {post_reward > 0 && (post_reward - 1) % 1000 < 100 ? burst : base, never};
        case HashSchedule::EXIT_AT_REWARD: return {post_reward <= 0 ? burst : base, never};
        case HashSchedule::POST_REWARD_HOUR:
        case HashSchedule::POST_REWARD_DAY:
        case HashSchedule::OUTAGE_DAY: {
            const double duration{m_scenario.schedule == HashSchedule::POST_REWARD_HOUR ? 3600.0 : 86400.0};
            const double end{m_reward_time + duration};
            if (post_reward > 0 && now < end) {
                return {m_scenario.schedule == HashSchedule::OUTAGE_DAY ? 0 : burst, end};
            }
            return {base, never};
        }
        }
        return {base, never};
    }

    uint32_t RequiredAt(double now, int offset) const
    {
        const int64_t stamp{static_cast<int64_t>(std::floor(now)) + offset};
        BOOST_REQUIRE_GT(stamp, m_chain.back().GetMedianTimePast());
        BOOST_REQUIRE_LE(stamp, static_cast<int64_t>(std::floor(now)) + MAX_FUTURE_BLOCK_TIME);
        BOOST_REQUIRE_LE(stamp, std::numeric_limits<uint32_t>::max());
        CBlockHeader candidate;
        candidate.nTime = stamp;
        return GetNextWorkRequired(&m_chain.back(), &candidate, m_params);
    }

    void AccountRental(double duration, uint64_t rate, int post_reward)
    {
        if (post_reward > 0 && rate > m_scenario.base_milli) {
            m_rental_hashes += duration * m_base_hashes_per_second * (rate - m_scenario.base_milli) / 1000.0;
        }
    }

    void NextBlock()
    {
        const int post_reward{m_chain.back().nHeight + 1 - m_reward_height};
        const int offset{ClockOffset(post_reward)};
        double now{std::max(m_time, static_cast<double>(m_chain.back().GetMedianTimePast() + 1 - offset))};
        const double uniform{(static_cast<double>(m_random() >> 12) + 0.5) / 4503599627370496.0};
        double hazard{m_scenario.stochastic ? -std::log(uniform) : 1.0};
        uint32_t final_bits{0};
        uint64_t final_rate{0};

        for (int segment = 0; segment < 1000; ++segment) {
            const auto [rate, schedule_end]{Rate(post_reward, now)};
            if (rate == 0) {
                BOOST_REQUIRE(std::isfinite(schedule_end));
                now = schedule_end;
                continue;
            }
            const uint32_t bits{RequiredAt(now, offset)};
            const double per_second{m_base_hashes_per_second * rate / 1000.0 / GetBitsProof(bits).getdouble()};
            BOOST_REQUIRE(std::isfinite(per_second) && per_second > 0);
            const double arrival{now + hazard / per_second};
            // A one-day search horizon keeps even extreme low-rate waits within
            // timestamp range; hazard is retained across horizon boundaries.
            double end{std::min({arrival, schedule_end, now + 86400.0})};
            const uint32_t end_bits{RequiredAt(end, offset)};
            bool target_changed{end_bits != bits};
            if (target_changed) {
                BOOST_REQUIRE(GetBitsProof(end_bits) <= GetBitsProof(bits));
                int64_t low{static_cast<int64_t>(std::floor(now)) + 1};
                int64_t high{static_cast<int64_t>(std::floor(end))};
                while (low < high) {
                    const int64_t mid{low + (high - low) / 2};
                    if (RequiredAt(static_cast<double>(mid), offset) == bits) low = mid + 1;
                    else high = mid;
                }
                end = static_cast<double>(low);
                ++m_emergency_changes;
            }
            AccountRental(end - now, rate, post_reward);
            if (!target_changed && arrival <= end && arrival < schedule_end) {
                now = arrival;
                final_bits = bits;
                final_rate = rate;
                break;
            }
            hazard = std::max(0.0, hazard - (end - now) * per_second);
            now = end;
        }
        BOOST_REQUIRE_NE(final_bits, 0U);
        const int64_t stamp{static_cast<int64_t>(std::floor(now)) + offset};
        BOOST_REQUIRE_GT(stamp, m_chain.back().GetMedianTimePast());
        BOOST_REQUIRE_LE(stamp, static_cast<int64_t>(std::floor(now)) + MAX_FUTURE_BLOCK_TIME);
        BOOST_REQUIRE(DeriveTarget(final_bits, m_params.powLimit).has_value());
        BOOST_REQUIRE(PermittedDifficultyTransition(m_params, m_chain.back().nHeight + 1, m_chain.back().nBits, final_bits));
        BOOST_REQUIRE_EQUAL(final_bits, RequiredAt(now, offset));
        Append(final_bits, stamp);
        m_time = now;
        m_last_rental = final_rate > m_scenario.base_milli;
        ++m_modeled_blocks;
    }

    const Scenario& m_scenario;
    Consensus::Params m_params;
    std::mt19937_64 m_random;
    const arith_uint256 m_reference_work;
    const double m_base_hashes_per_second;
    std::deque<CBlockIndex> m_chain;
    int m_reward_height{0};
    double m_time{0};
    double m_reward_time{0};
    double m_rental_hashes{0};
    bool m_last_rental{false};
    uint64_t m_emergency_changes{0};
    uint64_t m_modeled_blocks{0};
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(coinbase_maturity_hashrate_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(shockwave_hashrate_scenarios)
{
    ArgsManager args;
    const auto mainnet{CreateChainParams(args, ChainType::MAIN)};
    const auto params{mainnet->GetConsensus()};
    BOOST_REQUIRE_EQUAL(params.nCoinbaseWorkMaturityActivationHeight, std::numeric_limits<int>::max());
    std::vector<Scenario> scenarios;
    for (const uint64_t rate : {1, 10, 100, 1000, 2000, 10000, 1000000}) {
        scenarios.push_back({"steady_" + std::to_string(rate) + "_milli", rate});
    }
    scenarios.push_back({"rental_1000x_500_before", 1000, 1000000, HashSchedule::PRE_REWARD_500});
    scenarios.push_back({"rental_1000x_500_after", 1000, 1000000, HashSchedule::POST_REWARD_500});
    scenarios.push_back({"rental_1000x_one_hour", 1000, 1000000, HashSchedule::POST_REWARD_HOUR});
    scenarios.push_back({"rental_1000x_one_day", 1000, 1000000, HashSchedule::POST_REWARD_DAY});
    // The reference rate is about 0.27214 EH/s. An extra 3.675 reference
    // units models approximately 1 EH/s, rounded to this schedule's 0.001 unit.
    scenarios.push_back({"rental_about_1EH_500_after", 1000, 4675, HashSchedule::POST_REWARD_500});
    scenarios.push_back({"rental_about_1EH_one_hour", 1000, 4675, HashSchedule::POST_REWARD_HOUR});
    scenarios.push_back({"rental_about_1EH_one_day", 1000, 4675, HashSchedule::POST_REWARD_DAY});
    scenarios.push_back({"low_base_about_1EH_500_after", 100, 3775, HashSchedule::POST_REWARD_500});
    scenarios.push_back({"rental_1000x_periodic", 1000, 1000000, HashSchedule::PERIODIC_100_OF_1000});
    scenarios.push_back({"exit_1000x_to_0_001x", 1, 1000000, HashSchedule::EXIT_AT_REWARD});
    scenarios.push_back({"outage_one_day", 1000, 1000000, HashSchedule::OUTAGE_DAY});
    scenarios.push_back({"clock_future_limit", 1000, 1000000, HashSchedule::STEADY, false, 0, ClockSchedule::FUTURE_LIMIT});
    scenarios.push_back({"clock_behind_two_hours", 1000, 1000000, HashSchedule::STEADY, false, 0, ClockSchedule::BEHIND_TWO_HOURS});
    scenarios.push_back({"clock_alternating_future", 1000, 1000000, HashSchedule::STEADY, false, 0, ClockSchedule::ALTERNATE_FUTURE});
    for (const uint64_t seed : {20260924, 42008580, 12960500}) {
        scenarios.push_back({"stochastic_low", 100, 1000000, HashSchedule::STEADY, true, seed});
        scenarios.push_back({"stochastic_baseline", 1000, 1000000, HashSchedule::STEADY, true, seed});
        scenarios.push_back({"stochastic_high", 10000, 1000000, HashSchedule::STEADY, true, seed});
        scenarios.push_back({"stochastic_rental_after", 1000, 1000000, HashSchedule::POST_REWARD_500, true, seed});
    }

    std::ofstream csv;
    if (const char* path = std::getenv("BC2_HASHRATE_RESULTS")) {
        csv.open(path);
        BOOST_REQUIRE_MESSAGE(csv.is_open(), "Cannot open BC2_HASHRATE_RESULTS");
        csv << "scenario,base_reference_ratio,seed,stochastic,first_spendable_age,elapsed_days_to_eligible_parent,credited_work_hex,credited_work_over_threshold,post_reward_blocks_during_rental,post_reward_work_during_rental_hex,estimated_extra_hashes_post_reward,emergency_target_changes_including_warmup,modeled_blocks_including_warmup\n";
        csv.flush();
    }
    bool reached_minimum{false}, reached_interior{false}, reached_cap{false};
    bool exercised_rental{false}, exercised_emergency{false};
    for (const auto& scenario : scenarios) {
        BOOST_TEST_CONTEXT(scenario.name << " seed=" << scenario.seed) {
            BOOST_TEST_MESSAGE("Running " << scenario.name << " seed=" << scenario.seed);
            const auto result{HashrateModel{scenario, params}.Run()};
            reached_minimum |= result.age == 4200;
            reached_interior |= result.age > 4200 && result.age < 12960;
            reached_cap |= result.age == 12960;
            exercised_rental |= result.rental_blocks > 0;
            exercised_emergency |= result.emergency_changes > 0;
            const double work_ratio{result.work.getdouble() / UintToArith256(params.nCoinbaseMaturityWork).getdouble()};
            BOOST_TEST_MESSAGE(scenario.name << " seed=" << scenario.seed << " first_age=" << result.age
                               << " elapsed_days=" << result.elapsed_days << " work/threshold=" << work_ratio);
            if (csv.is_open()) {
                csv << scenario.name << ',' << scenario.base_milli / 1000.0 << ',' << scenario.seed << ',' << scenario.stochastic
                    << ',' << result.age << ',' << std::setprecision(12) << result.elapsed_days << ',' << result.work.GetHex()
                    << ',' << work_ratio << ',' << result.rental_blocks << ',' << result.work_during_rental.GetHex()
                    << ',' << result.estimated_rental_hashes << ',' << result.emergency_changes << ',' << result.modeled_blocks << '\n';
                csv.flush();
                BOOST_REQUIRE(csv.good());
            }
            if (scenario.schedule == HashSchedule::PRE_REWARD_500) {
                BOOST_REQUIRE_EQUAL(result.rental_blocks, 0);
                BOOST_REQUIRE(result.work_during_rental == 0);
            }
            if (scenario.schedule == HashSchedule::POST_REWARD_500) BOOST_REQUIRE_EQUAL(result.rental_blocks, 500);
            if (scenario.schedule == HashSchedule::EXIT_AT_REWARD) BOOST_REQUIRE_GT(result.emergency_changes, 0U);
            if (scenario.schedule == HashSchedule::OUTAGE_DAY) BOOST_REQUIRE_GE(result.elapsed_days, 1.0);
        }
    }
    BOOST_REQUIRE(reached_minimum);
    BOOST_REQUIRE(reached_interior);
    BOOST_REQUIRE(reached_cap);
    BOOST_REQUIRE(exercised_rental);
    BOOST_REQUIRE(exercised_emergency);
}

BOOST_AUTO_TEST_SUITE_END()
