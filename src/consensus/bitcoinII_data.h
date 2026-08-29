// Copyright (c) 2026 The BitcoinII developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOINII_CONSENSUS_BITCOINII_DATA_H
#define BITCOINII_CONSENSUS_BITCOINII_DATA_H

#include <consensus/consensus.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/solver.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Consensus {

enum class BitcoinIIOutputViolation {
    NONE,
    OP_RETURN_COUNT,
    OP_RETURN_SIZE,
    OP_RETURN_OP13,
    BARE_MULTISIG,
};

enum class BitcoinIIWitnessViolation {
    NONE,
    TAPROOT_ANNEX,
    TAPSCRIPT_SIZE,
    ORDINAL_ENVELOPE,
};

inline bool IsBitcoinIIOpReturn(const CScript& script)
{
    return !script.empty() && script[0] == OP_RETURN;
}

/**
 * Detect an actual OP_13 opcode in an OP_RETURN script.
 *
 * A byte value equal to OP_13 inside pushed data is not an opcode and
 * therefore does not trigger this rule.
 */
inline bool HasBitcoinIIForbiddenOp13(const CScript& script)
{
    if (!IsBitcoinIIOpReturn(script)) {
        return false;
    }

    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> pushed;

    // Consume the leading OP_RETURN.
    if (!script.GetOp(pc, opcode, pushed) || opcode != OP_RETURN) {
        return false;
    }

    while (pc < script.end()) {
        if (!script.GetOp(pc, opcode, pushed)) {
            // Malformed trailing script is not itself an OP_13 violation.
            return false;
        }

        if (opcode == OP_13) {
            return true;
        }
    }

    return false;
}

/**
 * Detect an Ordinals inscription envelope semantically:
 *
 *     OP_FALSE OP_IF <push "ord">
 *
 * This deliberately parses script operations instead of scanning raw bytes,
 * so identical bytes contained inside pushed data do not create a match.
 */
inline bool HasBitcoinIIOrdinalEnvelope(
    std::span<const unsigned char> script_bytes)
{
    CScript script{script_bytes.begin(), script_bytes.end()};
    CScript::const_iterator pc{script.begin()};
    opcodetype opcode;
    std::vector<unsigned char> pushed;

    // 0 = looking for OP_FALSE
    // 1 = OP_FALSE seen; looking for OP_IF
    // 2 = OP_FALSE OP_IF seen; looking for pushed "ord"
    int state{0};

    while (pc < script.end()) {
        if (!script.GetOp(pc, opcode, pushed)) {
            return false;
        }

        if (state == 2) {
            if (opcode <= OP_PUSHDATA4 &&
                pushed.size() == 3 &&
                pushed[0] == 'o' &&
                pushed[1] == 'r' &&
                pushed[2] == 'd') {
                return true;
            }

            state = opcode == OP_0 ? 1 : 0;
            continue;
        }

        if (state == 1) {
            if (opcode == OP_IF) {
                state = 2;
            } else {
                state = opcode == OP_0 ? 1 : 0;
            }
            continue;
        }

        if (opcode == OP_0) {
            state = 1;
        }
    }

    return false;
}

/**
 * Rules which can be evaluated solely from transaction outputs.
 */
inline BitcoinIIOutputViolation CheckBitcoinIIOutputRules(
    const CTransaction& tx)
{
    unsigned int op_return_count{0};

    for (const CTxOut& txout : tx.vout) {
        const CScript& script{txout.scriptPubKey};

        if (IsBitcoinIIOpReturn(script)) {
            ++op_return_count;

            if (op_return_count > 1) {
                return BitcoinIIOutputViolation::OP_RETURN_COUNT;
            }

            if (script.size() > MAX_BITCOINII_OP_RETURN_BYTES) {
                return BitcoinIIOutputViolation::OP_RETURN_SIZE;
            }

            if (HasBitcoinIIForbiddenOp13(script)) {
                return BitcoinIIOutputViolation::OP_RETURN_OP13;
            }
        }

        std::vector<std::vector<unsigned char>> solutions;
        if (Solver(script, solutions) == TxoutType::MULTISIG) {
            return BitcoinIIOutputViolation::BARE_MULTISIG;
        }
    }

    return BitcoinIIOutputViolation::NONE;
}

/**
 * Rules for native Taproot spends.
 *
 * Key-path spends remain permitted.
 * Annexes are forbidden.
 * Script-path revealed scripts are capped at 3,600 bytes.
 * Ordinals inscription envelopes are forbidden.
 */
inline BitcoinIIWitnessViolation CheckBitcoinIIWitnessRules(
    const CScript& prev_script,
    const CScriptWitness& witness)
{
    if (!prev_script.IsPayToTaproot() || witness.stack.empty()) {
        return BitcoinIIWitnessViolation::NONE;
    }

    // BIP341 annex is the final witness element and begins with 0x50.
    if (witness.stack.size() >= 2 &&
        !witness.stack.back().empty() &&
        witness.stack.back()[0] == ANNEX_TAG) {
        return BitcoinIIWitnessViolation::TAPROOT_ANNEX;
    }

    // One witness element is a normal Taproot key-path spend.
    if (witness.stack.size() < 2) {
        return BitcoinIIWitnessViolation::NONE;
    }

    // Without an annex, the final item is the control block and the
    // penultimate item is the revealed script for a script-path spend.
    const auto& tapscript{witness.stack[witness.stack.size() - 2]};

    if (tapscript.size() > MAX_BITCOINII_TAPSCRIPT_BYTES) {
        return BitcoinIIWitnessViolation::TAPSCRIPT_SIZE;
    }

    if (HasBitcoinIIOrdinalEnvelope(
            std::span<const unsigned char>{
                tapscript.data(),
                tapscript.size()})) {
        return BitcoinIIWitnessViolation::ORDINAL_ENVELOPE;
    }

    return BitcoinIIWitnessViolation::NONE;
}

} // namespace Consensus

#endif // BITCOINII_CONSENSUS_BITCOINII_DATA_H
