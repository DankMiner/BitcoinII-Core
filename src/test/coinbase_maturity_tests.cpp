// Copyright (c) 2026 1Miner.net
// Licensed under LICENSE-1MINER-BC2-MATURITY.md, including its prior-grant exception.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <consensus/coinbase_maturity.h>
#include <consensus/params.h>
#include <kernel/chainparams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <util/time.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <ios>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using TestChain = std::deque<CBlockIndex>;

void AppendBlocks(TestChain& chain, int count, const arith_uint256& work_per_block, CBlockIndex* parent = nullptr)
{
    if (!chain.empty()) parent = &chain.back();
    for (int i = 0; i < count; ++i) {
        auto& block = chain.emplace_back();
        block.pprev = parent;
        block.nHeight = parent ? parent->nHeight + 1 : 0;
        block.nChainWork = (parent ? parent->nChainWork : arith_uint256{}) + work_per_block;
        block.BuildSkip();
        parent = &block;
    }
}

Consensus::Params WorkParams(int activation_height = 10, uint64_t work = 41990)
{
    Consensus::Params params{};
    params.nCoinbaseWorkMaturityActivationHeight = activation_height;
    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{work});
    return params;
}

using OracleWork = boost::multiprecision::cpp_int;

// Decimal constants and unbounded arithmetic keep this oracle independent of
// arith_uint256, compact difficulty decoding, and the production predicate.
const OracleWork REFERENCE_PROOF{"163284487972206562362"};
const OracleWork MAINNET_REQUIRED_WORK{"1400817622313560098503598"};
constexpr int MATRIX_ORIGIN{10};
constexpr int MATRIX_LAST_AGE{13000};

arith_uint256 ChainWork(const OracleWork& work)
{
    return UintToArith256(uint256::FromUserHex(work.str(0, std::ios_base::hex)).value());
}

Consensus::Params MainnetWorkParams()
{
    ArgsManager args;
    auto params{CreateChainParams(args, ChainType::MAIN)->GetConsensus()};
    BOOST_REQUIRE(params.nCoinbaseMaturityWork == ArithToUint256(ChainWork(MAINNET_REQUIRED_WORK)));
    BOOST_REQUIRE_EQUAL(params.nCoinbaseMaturityMin, 4200);
    BOOST_REQUIRE_EQUAL(params.nCoinbaseMaturityMax, 12960);
    params.nCoinbaseWorkMaturityActivationHeight = MATRIX_ORIGIN;
    return params;
}

std::vector<OracleWork> ConstantSchedule(const OracleWork& per_block)
{
    return std::vector<OracleWork>(MATRIX_LAST_AGE - 1, per_block);
}

// Prefix zero is the moment the reward exists. Find the first prefix which
// reaches W, convert that position to a spending age, and apply the age bounds.
// No chain index or consensus result is used to derive the expected age.
int FirstSpendAge(const std::vector<OracleWork>& prefix)
{
    const auto reached{std::lower_bound(prefix.begin(), prefix.end(), MAINNET_REQUIRED_WORK)};
    const int work_age{reached == prefix.end() ? 12960 : static_cast<int>(reached - prefix.begin()) + 1};
    return std::clamp(work_age, 4200, 12960);
}

int CheckWorkSchedule(const std::vector<OracleWork>& work, const std::string& label)
{
    BOOST_REQUIRE_EQUAL(work.size(), MATRIX_LAST_AGE - 1);
    std::vector<OracleWork> prefix{OracleWork{0}};
    prefix.reserve(work.size() + 1);
    for (const auto& proof : work) {
        BOOST_REQUIRE(proof > 0);
        prefix.push_back(prefix.back() + proof);
    }
    const int first_age{FirstSpendAge(prefix)};
    const auto params{MainnetWorkParams()};
    TestChain chain;
    // A large pre-existing chain and reward proof must not enter the oracle's
    // post-reward sum, even when either already exceeds the entire threshold.
    AppendBlocks(chain, MATRIX_ORIGIN + 1, ChainWork(MAINNET_REQUIRED_WORK * 7));
    for (int age{1}; age <= MATRIX_LAST_AGE; ++age) {
        if (age > 1) AppendBlocks(chain, 1, ChainWork(work[age - 2]));
        BOOST_TEST_CONTEXT(label << "; candidate age " << age << "; first spend age " << first_age) {
            const auto state{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, chain.back(), params)};
            BOOST_CHECK(state.work_based);
            BOOST_CHECK_EQUAL(state.mature, age >= first_age);
            BOOST_CHECK(state.accumulated_work == ChainWork(prefix[age - 1]));
            BOOST_CHECK(state.required_work == ChainWork(MAINNET_REQUIRED_WORK));
            BOOST_CHECK_EQUAL(state.blocks_to_minimum, std::max(0, 4200 - age));
            BOOST_CHECK_EQUAL(state.blocks_to_maximum, std::max(0, 12960 - age));
        }
    }
    return first_age;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(coinbase_maturity_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(minimum_work_and_maximum_boundaries)
{
    TestChain chain;
    AppendBlocks(chain, 12970, 10);
    auto params{WorkParams()};

    // The 4,200-block age uses 4,199 completed post-creation blocks.
    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{1});
    const auto before_min{Consensus::GetCoinbaseMaturity(10, chain[4208], params)};
    BOOST_CHECK(!before_min.mature);
    BOOST_CHECK(before_min.work_based);
    BOOST_CHECK_EQUAL(before_min.blocks_to_minimum, 1);
    BOOST_CHECK_EQUAL(before_min.blocks_to_maximum, 8761);

    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{41991});
    auto at_min{Consensus::GetCoinbaseMaturity(10, chain[4209], params)};
    BOOST_CHECK(!at_min.mature); // accumulated work == threshold - 1
    BOOST_CHECK_EQUAL(at_min.accumulated_work.GetLow64(), 41990U);
    BOOST_CHECK_EQUAL(at_min.required_work.GetLow64(), 41991U);
    BOOST_CHECK_EQUAL(at_min.blocks_to_minimum, 0);
    BOOST_CHECK_EQUAL(at_min.blocks_to_maximum, 8760);
    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{41990});
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(10, chain[4209], params).mature);
    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{41989});
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(10, chain[4209], params).mature);

    params.nCoinbaseMaturityWork = ArithToUint256(arith_uint256{1000000});
    const auto before_max{Consensus::GetCoinbaseMaturity(10, chain[12968], params)};
    BOOST_CHECK(!before_max.mature);
    BOOST_CHECK_EQUAL(before_max.blocks_to_maximum, 1);
    const auto at_max{Consensus::GetCoinbaseMaturity(10, chain[12969], params)};
    BOOST_CHECK(at_max.mature);
    BOOST_CHECK_EQUAL(at_max.blocks_to_maximum, 0);
    BOOST_CHECK(at_max.accumulated_work < at_max.required_work);
}

BOOST_AUTO_TEST_CASE(activation_grandfathers_existing_rewards)
{
    TestChain chain;
    AppendBlocks(chain, 200, 10);
    const auto params{WorkParams(100)};
    const auto immature_legacy{Consensus::GetCoinbaseMaturity(99, chain[197], params)};
    BOOST_CHECK(!immature_legacy.mature);
    BOOST_CHECK(!immature_legacy.work_based);
    BOOST_CHECK_EQUAL(immature_legacy.blocks_to_minimum, 1);
    BOOST_CHECK_EQUAL(immature_legacy.blocks_to_maximum, 1);
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(99, chain[198], params).mature);
    const auto new_reward{Consensus::GetCoinbaseMaturity(100, chain[199], params)};
    BOOST_CHECK(new_reward.work_based);
    BOOST_CHECK(!new_reward.mature);

    const Consensus::Params disabled{};
    const auto legacy{Consensus::GetCoinbaseMaturity(100, chain[199], disabled)};
    BOOST_CHECK(legacy.mature);
    BOOST_CHECK(!legacy.work_based);
}

BOOST_AUTO_TEST_CASE(branch_work_and_excluded_blocks)
{
    TestChain common;
    AppendBlocks(common, 11, 1000000);
    const auto params{WorkParams(10, 6000)};
    const auto just_created{Consensus::GetCoinbaseMaturity(10, common.back(), params)};
    BOOST_CHECK(just_created.accumulated_work == 0);

    TestChain low_branch, high_branch;
    AppendBlocks(low_branch, 4199, 1, &common.back());
    AppendBlocks(high_branch, 4199, 2, &common.back());
    const auto low{Consensus::GetCoinbaseMaturity(10, low_branch.back(), params)};
    const auto high{Consensus::GetCoinbaseMaturity(10, high_branch.back(), params)};
    BOOST_CHECK(!low.mature);
    BOOST_CHECK(high.mature);
    BOOST_CHECK_EQUAL(low.accumulated_work.GetLow64(), 4199U);
    BOOST_CHECK_EQUAL(high.accumulated_work.GetLow64(), 8398U);

    // A prospective spending block contributes no work until it is the parent
    // of a later candidate. Its earlier, high-work ancestors contribute none.
    const CBlockIndex& before_large_block{low_branch.back()};
    AppendBlocks(low_branch, 1, 1000000);
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, before_large_block, params).mature);
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(10, low_branch.back(), params).mature);
}

BOOST_AUTO_TEST_CASE(monotonic_and_deterministic_on_extension)
{
    TestChain chain;
    AppendBlocks(chain, 11, 7);
    const auto params{WorkParams(10, 50000)};
    bool was_mature{false};
    arith_uint256 previous_work{};
    for (int i = 0; i < 13000; ++i) {
        AppendBlocks(chain, 1, i % 7 + 1);
        const auto first{Consensus::GetCoinbaseMaturity(10, chain.back(), params)};
        const auto second{Consensus::GetCoinbaseMaturity(10, chain.back(), params)};
        BOOST_CHECK_EQUAL(first.mature, second.mature);
        BOOST_CHECK(first.accumulated_work == second.accumulated_work);
        BOOST_CHECK(first.accumulated_work >= previous_work);
        BOOST_CHECK(!was_mature || first.mature);
        previous_work = first.accumulated_work;
        was_mature = first.mature;
    }
    BOOST_CHECK(was_mature);
}

BOOST_AUTO_TEST_CASE(invalid_inputs_fail_closed)
{
    TestChain chain;
    AppendBlocks(chain, 12970, 10);
    auto params{WorkParams()};
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(-1, chain.back(), params).mature);
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(12970, chain.back(), params).mature);
    CBlockIndex invalid_parent;
    invalid_parent.nHeight = -1;
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, invalid_parent, params).mature);
    invalid_parent.nHeight = std::numeric_limits<int>::max();
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, invalid_parent, params).mature);
    chain.back().nChainWork = arith_uint256{1};
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, chain.back(), params).mature);
    chain.back().nChainWork = arith_uint256{129700};
    params.nCoinbaseMaturityWork.SetNull();
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, chain.back(), params).mature);
    params = WorkParams();
    params.nCoinbaseMaturityMin = 0;
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, chain.back(), params).mature);
    params = WorkParams();
    params.nCoinbaseMaturityMax = params.nCoinbaseMaturityMin - 1;
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, chain.back(), params).mature);
}

BOOST_AUTO_TEST_CASE(network_defaults_and_regtest_options)
{
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET, ChainType::REGTEST}) {
        ArgsManager args;
        SetupChainParamsBaseOptions(args);
        const auto params{CreateChainParams(args, chain)};
        BOOST_CHECK_EQUAL(params->GetConsensus().nCoinbaseWorkMaturityActivationHeight, std::numeric_limits<int>::max());
        if (chain == ChainType::MAIN) {
            BOOST_CHECK(params->GetConsensus().nCoinbaseMaturityWork ==
                uint256{"0000000000000000000000000000000000000000000128a279b84c5571b87bae"});
        } else {
            BOOST_CHECK(params->GetConsensus().nCoinbaseMaturityWork.IsNull());
        }
        BOOST_CHECK_EQUAL(params->GetConsensus().nCoinbaseMaturityMin, 4200);
        BOOST_CHECK_EQUAL(params->GetConsensus().nCoinbaseMaturityMax, 12960);
    }

    ArgsManager args;
    SetupChainParamsBaseOptions(args);
    args.ForceSetArg("-testcoinbasematurityheight", "10");
    BOOST_CHECK_THROW(CreateChainParams(args, ChainType::REGTEST), std::runtime_error);
    const std::string work_hex{ArithToUint256(arith_uint256{41990}).GetHex()};
    args.ForceSetArg("-testcoinbasematuritywork", work_hex);
    const auto configured{CreateChainParams(args, ChainType::REGTEST)};
    BOOST_CHECK_EQUAL(configured->GetConsensus().nCoinbaseWorkMaturityActivationHeight, 10);
    BOOST_CHECK_EQUAL(configured->GetConsensus().nCoinbaseMaturityWork.GetHex(), work_hex);
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET}) {
        BOOST_CHECK_THROW(CreateChainParams(args, chain), std::runtime_error);
    }
    for (const std::string& bad_work : {std::string(64, '0'), std::string("1"), std::string(64, 'g'), "0x" + work_hex}) {
        args.ForceSetArg("-testcoinbasematuritywork", bad_work);
        BOOST_CHECK_THROW(CreateChainParams(args, ChainType::REGTEST), std::runtime_error);
    }
    args.ForceSetArg("-testcoinbasematuritywork", work_hex);
    for (const std::string bad_height : {"-1", "2147483647", "2147483648", "1x"}) {
        args.ForceSetArg("-testcoinbasematurityheight", bad_height);
        BOOST_CHECK_THROW(CreateChainParams(args, ChainType::REGTEST), std::runtime_error);
    }

    CChainParams::RegTestOptions options;
    options.coinbase_maturity_height = 10;
    BOOST_CHECK_THROW(CChainParams::RegTest(options), std::runtime_error);
    options.coinbase_maturity_work = uint256{};
    BOOST_CHECK_THROW(CChainParams::RegTest(options), std::runtime_error);
    options.coinbase_maturity_work = ArithToUint256(arith_uint256{1});
    options.coinbase_maturity_height = -1;
    BOOST_CHECK_THROW(CChainParams::RegTest(options), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(hardest_epoch_midpoint_calibration)
{
    ArgsManager args;
    const auto mainnet{CreateChainParams(args, ChainType::MAIN)};
    auto params{mainnet->GetConsensus()};
    const arith_uint256 reference_work{GetBitsProof(0x181cebcd)};
    const int midpoint{(params.nCoinbaseMaturityMin + params.nCoinbaseMaturityMax) / 2};
    BOOST_CHECK_EQUAL(midpoint, 8580);
    BOOST_CHECK(UintToArith256(params.nCoinbaseMaturityWork) == reference_work * (midpoint - 1));
    BOOST_CHECK_EQUAL(params.nCoinbaseWorkMaturityActivationHeight, std::numeric_limits<int>::max());

    // Activate only the local test parameters. Exercise the selected >64-bit
    // target through consensus, including the exact first spendable age.
    params.nCoinbaseWorkMaturityActivationHeight = 10;
    const auto check_first_age = [&](const arith_uint256& per_block_work, int first_age) {
        TestChain chain;
        AppendBlocks(chain, 10 + first_age, per_block_work);
        BOOST_CHECK(!Consensus::GetCoinbaseMaturity(10, chain[10 + first_age - 2], params).mature);
        const auto mature{Consensus::GetCoinbaseMaturity(10, chain.back(), params)};
        BOOST_CHECK(mature.mature);
        BOOST_CHECK(mature.work_based);
        if (first_age == params.nCoinbaseMaturityMax) {
            BOOST_CHECK(mature.accumulated_work < mature.required_work);
        } else {
            BOOST_CHECK(mature.accumulated_work >= mature.required_work);
        }
    };
    check_first_age(reference_work, 8580);
    check_first_age(reference_work * 2, 4291);
    check_first_age(reference_work * 3, 4200);
    check_first_age(reference_work / arith_uint256{2}, 12960);
}

BOOST_AUTO_TEST_CASE(mainnet_constant_work_rate_matrix)
{
    BOOST_REQUIRE(GetBitsProof(0x181cebcd) == ChainWork(REFERENCE_PROOF));
    struct Rate {
        int numerator;
        int denominator;
        int first_age;
    };
    // Work is floored to integer proof units, including the fractional rates.
    const std::array<Rate, 14> rates{{
        {1, 100, 12960}, {1, 20, 12960}, {1, 10, 12960}, {1, 4, 12960},
        {1, 2, 12960}, {2, 3, 12870}, {3, 4, 11440}, {1, 1, 8580},
        {3, 2, 5721}, {2, 1, 4291}, {3, 1, 4200}, {10, 1, 4200},
        {100, 1, 4200}, {1000, 1, 4200},
    }};
    for (const auto& rate : rates) {
        const std::string label{std::to_string(rate.numerator) + "/" + std::to_string(rate.denominator) + " reference work"};
        const OracleWork per_block{REFERENCE_PROOF * rate.numerator / rate.denominator};
        BOOST_CHECK_EQUAL(CheckWorkSchedule(ConstantSchedule(per_block), label), rate.first_age);
    }
}

BOOST_AUTO_TEST_CASE(mainnet_exact_work_boundaries)
{
    const auto params{MainnetWorkParams()};
    for (const int age : {4199, 4200, 8580, 12959, 12960}) {
        for (const int delta : {-1, 0, 1}) {
            BOOST_TEST_CONTEXT("age " << age << "; threshold delta " << delta) {
                TestChain chain;
                AppendBlocks(chain, MATRIX_ORIGIN + 1, ChainWork(MAINNET_REQUIRED_WORK * 100));
                AppendBlocks(chain, age - 2, 2);
                const OracleWork final_proof{MAINNET_REQUIRED_WORK + delta - OracleWork{2} * (age - 2)};
                AppendBlocks(chain, 1, ChainWork(final_proof));
                const auto state{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, chain.back(), params)};
                BOOST_CHECK(state.accumulated_work == ChainWork(MAINNET_REQUIRED_WORK + delta));
                const bool expected{age == 12960 || (age >= 4200 && delta >= 0)};
                BOOST_CHECK_EQUAL(state.mature, expected);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(mainnet_rental_bursts_and_excluded_work)
{
    const auto params{MainnetWorkParams()};
    const OracleWork quiet_proof{REFERENCE_PROOF / 100};

    TestChain pre_reward;
    AppendBlocks(pre_reward, 500, ChainWork(REFERENCE_PROOF * 1000));
    AppendBlocks(pre_reward, 1, ChainWork(MAINNET_REQUIRED_WORK * 1000));
    const int reward_height{pre_reward.back().nHeight};
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(reward_height, pre_reward.back(), params).accumulated_work == 0);
    AppendBlocks(pre_reward, 12958, ChainWork(quiet_proof));
    const auto before_cap{Consensus::GetCoinbaseMaturity(reward_height, pre_reward.back(), params)};
    BOOST_CHECK(!before_cap.mature);
    BOOST_CHECK(before_cap.accumulated_work == ChainWork(quiet_proof * 12958));
    AppendBlocks(pre_reward, 1, ChainWork(quiet_proof));
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(reward_height, pre_reward.back(), params).mature);

    for (const int burst_start : {0, 3699, 4199, 10000}) {
        auto schedule{ConstantSchedule(quiet_proof)};
        // Five hundred extremely difficult blocks constitute genuine work
        // after the reward, but can never bypass the minimum block age.
        std::fill_n(schedule.begin() + burst_start, 500, REFERENCE_PROOF * 1000);
        const int first_age{CheckWorkSchedule(schedule, "500-block burst at post-reward offset " + std::to_string(burst_start))};
        BOOST_CHECK_EQUAL(first_age, burst_start < 4199 ? 4200 : burst_start + 10);
    }
    for (const int huge_offset : {0, 4198, 4199, 10000}) {
        auto schedule{ConstantSchedule(quiet_proof)};
        schedule[huge_offset] = MAINNET_REQUIRED_WORK * 1000;
        const int first_age{CheckWorkSchedule(schedule, "single huge block at post-reward offset " + std::to_string(huge_offset))};
        BOOST_CHECK_EQUAL(first_age, std::max(4200, huge_offset + 2));
    }

    // A reward cannot spend in a huge-work candidate that would itself take
    // accumulated work across W. Only a later candidate may count that block.
    TestChain candidate;
    AppendBlocks(candidate, MATRIX_ORIGIN + 1, ChainWork(REFERENCE_PROOF));
    AppendBlocks(candidate, 4199, ChainWork(quiet_proof));
    const CBlockIndex& candidate_parent{candidate.back()};
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, candidate_parent, params).mature);
    AppendBlocks(candidate, 1, ChainWork(MAINNET_REQUIRED_WORK));
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, candidate_parent, params).mature);
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, candidate.back(), params).mature);
}

BOOST_AUTO_TEST_CASE(mainnet_work_order_and_branch_reorganization)
{
    const auto params{MainnetWorkParams()};
    TestChain common;
    AppendBlocks(common, MATRIX_ORIGIN + 1, ChainWork(MAINNET_REQUIRED_WORK));
    TestChain front_loaded, back_loaded;
    // Build both branches from the same reward so ancestor selection is
    // exercised as well as their equal completed-work totals.
    AppendBlocks(front_loaded, 1, ChainWork(MAINNET_REQUIRED_WORK), &common.back());
    AppendBlocks(front_loaded, 4999, 2);
    AppendBlocks(back_loaded, 4999, 2, &common.back());
    AppendBlocks(back_loaded, 1, ChainWork(MAINNET_REQUIRED_WORK));
    const auto front{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, front_loaded.back(), params)};
    const auto back{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, back_loaded.back(), params)};
    BOOST_CHECK(front.mature);
    BOOST_CHECK(back.mature);
    BOOST_CHECK(front.accumulated_work == back.accumulated_work);
    BOOST_CHECK_EQUAL(front.blocks_to_maximum, back.blocks_to_maximum);
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, front_loaded[4198], params).mature);
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, back_loaded[4198], params).mature);

    TestChain older_low_work, shorter_high_work;
    AppendBlocks(older_low_work, 11999, ChainWork(REFERENCE_PROOF / 100), &common.back());
    AppendBlocks(shorter_high_work, 4199, ChainWork(REFERENCE_PROOF * 3), &common.back());
    BOOST_REQUIRE(older_low_work.back().nHeight > shorter_high_work.back().nHeight);
    BOOST_REQUIRE(older_low_work.back().nChainWork < shorter_high_work.back().nChainWork);
    BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, older_low_work.back(), params).mature);
    BOOST_CHECK(Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, shorter_high_work.back(), params).mature);
    // Repeatedly switching the supplied branch must not retain a mature result
    // or work credit from the previously examined branch.
    for (int i{0}; i < 4; ++i) {
        BOOST_CHECK(Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, shorter_high_work.back(), params).mature);
        BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, older_low_work.back(), params).mature);
    }

    TestChain different_origin;
    AppendBlocks(different_origin, MATRIX_ORIGIN + 1, ChainWork(MAINNET_REQUIRED_WORK * 1000));
    AppendBlocks(different_origin, 4199, ChainWork(REFERENCE_PROOF / 100));
    const auto alternate_reward{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, different_origin.back(), params)};
    BOOST_CHECK(!alternate_reward.mature);
    BOOST_CHECK(alternate_reward.accumulated_work == ChainWork((REFERENCE_PROOF / 100) * 4199));
}

BOOST_AUTO_TEST_CASE(mainnet_dropouts_stalls_and_time_independence)
{
    auto schedule{ConstantSchedule(REFERENCE_PROOF)};
    // A long low-work interval delays the threshold but never defeats the cap.
    std::fill(schedule.begin() + 3000, schedule.begin() + 9000, REFERENCE_PROOF / 1000);
    BOOST_CHECK_EQUAL(CheckWorkSchedule(schedule, "6000-block low-work interval"), 12960);
    std::fill(schedule.begin() + 9000, schedule.end(), REFERENCE_PROOF * 3);
    BOOST_CHECK(CheckWorkSchedule(schedule, "recovery after low-work interval") < 12960);

    const auto params{MainnetWorkParams()};
    TestChain chain;
    AppendBlocks(chain, MATRIX_ORIGIN + 8580, ChainWork(REFERENCE_PROOF));
    const auto check_times = [&](uint32_t spacing) {
        for (auto& block : chain) block.nTime = 1'600'000'000 + block.nHeight * spacing;
        BOOST_CHECK(!Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, chain[chain.size() - 2], params).mature);
        const auto state{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, chain.back(), params)};
        BOOST_CHECK(state.mature);
        BOOST_CHECK(state.accumulated_work == ChainWork(MAINNET_REQUIRED_WORK));
    };
    check_times(1);
    check_times(600);
    check_times(86400);

    const auto original_mock{GetMockTime()};
    for (const int64_t elapsed : {int64_t{0}, int64_t{86400}, int64_t{31536000}, int64_t{315360000}}) {
        SetMockTime(1'600'000'000 + elapsed);
        // No new block during a stall means no age or work progress, even
        // after a year. A timestamp change cannot mature this prior parent.
        const auto stalled{Consensus::GetCoinbaseMaturity(MATRIX_ORIGIN, chain[chain.size() - 2], params)};
        BOOST_CHECK(!stalled.mature);
        BOOST_CHECK(stalled.accumulated_work == ChainWork(MAINNET_REQUIRED_WORK - REFERENCE_PROOF));
    }
    SetMockTime(original_mock);
}

BOOST_AUTO_TEST_CASE(mainnet_seeded_mixed_work_schedules)
{
    std::mt19937_64 random{0x4243324d41545552ULL};
    const std::array<int, 6> rates{1, 10, 50, 100, 200, 1000};
    bool saw_minimum{false}, saw_interior{false}, saw_maximum{false};
    for (int scenario{0}; scenario < 18; ++scenario) {
        std::vector<OracleWork> work;
        work.reserve(MATRIX_LAST_AGE - 1);
        for (int block{0}; block < MATRIX_LAST_AGE - 1; ++block) {
            // Modulo selection makes the exact stream portable across standard
            // library implementations, unlike uniform_int_distribution.
            const int noise{1 + static_cast<int>(random() % 200)};
            OracleWork proof{REFERENCE_PROOF * rates[scenario % rates.size()] * noise / 10000};
            if (block >= 2000 && block < 4000) proof /= 1000;
            if (scenario >= 6 && scenario < 12 && block >= 500 && block < 1000) proof *= 1000;
            if (scenario >= 12 && block >= 10000 && block < 10500) proof *= 1000;
            work.push_back(proof);
        }
        const int first_age{CheckWorkSchedule(work, "seeded mixed-work scenario " + std::to_string(scenario))};
        saw_minimum |= first_age == 4200;
        saw_interior |= first_age > 4200 && first_age < 12960;
        saw_maximum |= first_age == 12960;
    }
    BOOST_CHECK(saw_minimum);
    BOOST_CHECK(saw_interior);
    BOOST_CHECK(saw_maximum);
}

BOOST_AUTO_TEST_SUITE_END()
