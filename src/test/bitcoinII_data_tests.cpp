// Copyright (c) 2026 The BitcoinII developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/bitcoinII_data.h>

#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_FIXTURE_TEST_SUITE(bitcoinII_data_tests, BasicTestingSetup)

static CTransaction TxWithOutputs(std::vector<CScript> scripts)
{
    CMutableTransaction tx;
    tx.vin.resize(1);

    for (const auto& script : scripts) {
        tx.vout.emplace_back(0, script);
    }

    return CTransaction{tx};
}

BOOST_AUTO_TEST_CASE(opreturn_count)
{
    const CScript opreturn{CScript() << OP_RETURN};

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({opreturn})) ==
        Consensus::BitcoinIIOutputViolation::NONE);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({opreturn, opreturn})) ==
        Consensus::BitcoinIIOutputViolation::OP_RETURN_COUNT);
}

BOOST_AUTO_TEST_CASE(opreturn_size_boundary)
{
    // OP_RETURN + OP_PUSHDATA1 + one-byte length + 80 data bytes
    // = exactly 83 bytes.
    const CScript exactly_83{
        CScript()
            << OP_RETURN
            << std::vector<unsigned char>(80, 0x01)
    };

    BOOST_REQUIRE_EQUAL(exactly_83.size(), 83U);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({exactly_83})) ==
        Consensus::BitcoinIIOutputViolation::NONE);

    // Same encoding with 81 payload bytes = 84-byte scriptPubKey.
    const CScript too_large{
        CScript()
            << OP_RETURN
            << std::vector<unsigned char>(81, 0x01)
    };

    BOOST_REQUIRE_EQUAL(too_large.size(), 84U);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({too_large})) ==
        Consensus::BitcoinIIOutputViolation::OP_RETURN_SIZE);
}

BOOST_AUTO_TEST_CASE(opreturn_op13)
{
    // An actual OP_13 opcode is forbidden.
    const CScript actual_op13{
        CScript()
            << OP_RETURN
            << OP_13
    };

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({actual_op13})) ==
        Consensus::BitcoinIIOutputViolation::OP_RETURN_OP13);

    // The byte value 0x5d inside pushed data is NOT an OP_13 opcode.
    const CScript pushed_5d{
        CScript()
            << OP_RETURN
            << std::vector<unsigned char>{0x5d}
    };

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({pushed_5d})) ==
        Consensus::BitcoinIIOutputViolation::NONE);
}

BOOST_AUTO_TEST_CASE(coinbase_obeys_same_opreturn_rule)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << 1 << 1;

    coinbase.vout.emplace_back(50 * COIN, CScript() << OP_TRUE);
    coinbase.vout.emplace_back(0, CScript() << OP_RETURN);
    coinbase.vout.emplace_back(0, CScript() << OP_RETURN);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            CTransaction{coinbase}) ==
        Consensus::BitcoinIIOutputViolation::OP_RETURN_COUNT);
}

BOOST_AUTO_TEST_CASE(bare_multisig)
{
    // Structurally valid compressed pubkey representation for Solver().
    std::vector<unsigned char> pubkey(33, 0x00);
    pubkey[0] = 0x02;

    const CScript bare_multisig{
        CScript()
            << OP_1
            << pubkey
            << OP_1
            << OP_CHECKMULTISIG
    };

    BOOST_CHECK(
        Consensus::CheckBitcoinIIOutputRules(
            TxWithOutputs({bare_multisig})) ==
        Consensus::BitcoinIIOutputViolation::BARE_MULTISIG);
}

BOOST_AUTO_TEST_CASE(ordinal_envelope)
{
    const std::vector<unsigned char> ord{'o', 'r', 'd'};

    const CScript inscription{
        CScript()
            << OP_FALSE
            << OP_IF
            << ord
            << OP_ENDIF
    };

    BOOST_CHECK(
        Consensus::HasBitcoinIIOrdinalEnvelope(
            std::span<const unsigned char>{
                inscription.data(),
                inscription.size()}));

    // "ord" bytes inside some unrelated pushed data must not match.
    const std::vector<unsigned char> unrelated{
        0x01, 0x02, 'o', 'r', 'd', 0x03, 0x04
    };

    const CScript pushed_data{
        CScript()
            << OP_FALSE
            << OP_IF
            << unrelated
            << OP_ENDIF
    };

    BOOST_CHECK(
        !Consensus::HasBitcoinIIOrdinalEnvelope(
            std::span<const unsigned char>{
                pushed_data.data(),
                pushed_data.size()}));
}

BOOST_AUTO_TEST_CASE(taproot_keypath_allowed)
{
    const CScript taproot_prevout{
        CScript()
            << OP_1
            << std::vector<unsigned char>(32, 0x01)
    };

    CScriptWitness witness;
    witness.stack.emplace_back(64, 0x01);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIWitnessRules(
            taproot_prevout,
            witness) ==
        Consensus::BitcoinIIWitnessViolation::NONE);
}

BOOST_AUTO_TEST_CASE(taproot_annex_forbidden)
{
    const CScript taproot_prevout{
        CScript()
            << OP_1
            << std::vector<unsigned char>(32, 0x01)
    };

    CScriptWitness witness;
    witness.stack.emplace_back(64, 0x01);

    std::vector<unsigned char> annex{ANNEX_TAG};
    annex.push_back(0x01);
    witness.stack.push_back(annex);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIWitnessRules(
            taproot_prevout,
            witness) ==
        Consensus::BitcoinIIWitnessViolation::TAPROOT_ANNEX);
}

BOOST_AUTO_TEST_CASE(tapscript_size_boundary)
{
    const CScript taproot_prevout{
        CScript()
            << OP_1
            << std::vector<unsigned char>(32, 0x01)
    };

    // Helper only needs the script-path witness layout:
    // [..., tapscript, control block]
    const std::vector<unsigned char> control_block(33, 0xc0);

    {
        CScriptWitness witness;
        witness.stack.emplace_back(MAX_BITCOINII_TAPSCRIPT_BYTES, OP_NOP);
        witness.stack.push_back(control_block);

        BOOST_CHECK(
            Consensus::CheckBitcoinIIWitnessRules(
                taproot_prevout,
                witness) ==
            Consensus::BitcoinIIWitnessViolation::NONE);
    }

    {
        CScriptWitness witness;
        witness.stack.emplace_back(MAX_BITCOINII_TAPSCRIPT_BYTES + 1, OP_NOP);
        witness.stack.push_back(control_block);

        BOOST_CHECK(
            Consensus::CheckBitcoinIIWitnessRules(
                taproot_prevout,
                witness) ==
            Consensus::BitcoinIIWitnessViolation::TAPSCRIPT_SIZE);
    }
}

BOOST_AUTO_TEST_CASE(taproot_ordinal_envelope_forbidden)
{
    const CScript taproot_prevout{
        CScript()
            << OP_1
            << std::vector<unsigned char>(32, 0x01)
    };

    const CScript tapscript{
        CScript()
            << OP_FALSE
            << OP_IF
            << std::vector<unsigned char>{'o', 'r', 'd'}
            << OP_ENDIF
            << OP_TRUE
    };

    CScriptWitness witness;
    witness.stack.emplace_back(
        tapscript.begin(),
        tapscript.end());
    witness.stack.emplace_back(33, 0xc0);

    BOOST_CHECK(
        Consensus::CheckBitcoinIIWitnessRules(
            taproot_prevout,
            witness) ==
        Consensus::BitcoinIIWitnessViolation::ORDINAL_ENVELOPE);
}

BOOST_AUTO_TEST_SUITE_END()
