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

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>

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

BOOST_AUTO_TEST_SUITE_END()
