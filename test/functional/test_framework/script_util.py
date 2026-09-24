#!/usr/bin/env python3
# Copyright (c) 2019-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Useful Script constants and utils."""
import unittest

from copy import deepcopy

from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    ser_compact_size,
    sha256,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_1,
    OP_15,
    OP_16,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_DUP,
    OP_ELSE,
    OP_ENDIF,
    OP_EQUAL,
    OP_EQUALVERIFY,
    OP_HASH160,
    OP_IF,
    OP_RETURN,
    OP_TRUE,
    hash160,
)

from test_framework.util import (
    assert_greater_than_or_equal,
    assert_equal,
)

# Maximum number of potentially executed legacy signature operations in validating a transaction.
MAX_STD_LEGACY_SIGOPS = 2_500

# Maximum number of sigops per standard P2SH redeemScript.
MAX_STD_P2SH_SIGOPS = 15

# To prevent a "tx-size-small" policy rule error, a transaction has to have a
# non-witness size of at least 65 bytes (MIN_STANDARD_TX_NONWITNESS_SIZE in
# src/policy/policy.h). Considering a Tx with the smallest possible single
# input (blank, empty scriptSig), and with an output omitting the scriptPubKey,
# we get to a minimum size of 60 bytes:
#
# Tx Skeleton: 4 [Version] + 1 [InCount] + 1 [OutCount] + 4 [LockTime] = 10 bytes
# Blank Input: 32 [PrevTxHash] + 4 [Index] + 1 [scriptSigLen] + 4 [SeqNo] = 41 bytes
# Output:      8 [Amount] + 1 [scriptPubKeyLen] = 9 bytes
#
# Hence, the scriptPubKey of the single output has to have a size of at
# least 5 bytes.
MIN_STANDARD_TX_NONWITNESS_SIZE = 65
MIN_PADDING = MIN_STANDARD_TX_NONWITNESS_SIZE - 10 - 41 - 9
assert MIN_PADDING == 5

# This script cannot be spent, allowing dust output values under
# standardness checks
DUMMY_MIN_OP_RETURN_SCRIPT = CScript([OP_RETURN] + ([OP_0] * (MIN_PADDING - 1)))
assert len(DUMMY_MIN_OP_RETURN_SCRIPT) == MIN_PADDING

PAY_TO_ANCHOR = CScript([OP_1, bytes.fromhex("4e73")])
ANCHOR_ADDRESS = "bcrt1pfeesnyr2tx"

def key_to_p2pk_script(key):
    key = check_key(key)
    return CScript([key, OP_CHECKSIG])


def keys_to_multisig_script(keys, *, k=None):
    n = len(keys)
    if k is None:  # n-of-n multisig by default
        k = n
    assert k <= n
    checked_keys = [check_key(key) for key in keys]
    return CScript([k] + checked_keys + [n, OP_CHECKMULTISIG])


def keyhash_to_p2pkh_script(hash):
    assert len(hash) == 20
    return CScript([OP_DUP, OP_HASH160, hash, OP_EQUALVERIFY, OP_CHECKSIG])


def scripthash_to_p2sh_script(hash):
    assert len(hash) == 20
    return CScript([OP_HASH160, hash, OP_EQUAL])


def key_to_p2pkh_script(key):
    key = check_key(key)
    return keyhash_to_p2pkh_script(hash160(key))


def script_to_p2sh_script(script):
    script = check_script(script)
    return scripthash_to_p2sh_script(hash160(script))


def key_to_p2sh_p2wpkh_script(key):
    key = check_key(key)
    p2shscript = CScript([OP_0, hash160(key)])
    return script_to_p2sh_script(p2shscript)


def program_to_witness_script(version, program):
    if isinstance(program, str):
        program = bytes.fromhex(program)
    assert 0 <= version <= 16
    assert 2 <= len(program) <= 40
    assert version > 0 or len(program) in [20, 32]
    return CScript([version, program])


def script_to_p2wsh_script(script):
    script = check_script(script)
    return program_to_witness_script(0, sha256(script))


def key_to_p2wpkh_script(key):
    key = check_key(key)
    return program_to_witness_script(0, hash160(key))


def script_to_p2sh_p2wsh_script(script):
    script = check_script(script)
    p2shscript = CScript([OP_0, sha256(script)])
    return script_to_p2sh_script(p2shscript)

def bulk_vout(tx, target_vsize):
    current_vsize = tx.get_vsize()
    if target_vsize < current_vsize:
        raise RuntimeError(f"target_vsize {target_vsize} is less than transaction virtual size {current_vsize}")

    # BitcoinII limits OP_RETURN payload size, so use ordinary P2WSH
    # outputs for bulk padding. Keep the existing small OP_RETURN output
    # for the final few bytes needed to hit target_vsize exactly.
    padding_spk = CScript([OP_0, b'\x01' * 32])
    padding_value = 10_000
    tail_output = tx.vout[-1]
    padding_vsize = len(CTxOut(nValue=padding_value, scriptPubKey=padding_spk).serialize())
    output_count = len(tx.vout)
    count_size = len(ser_compact_size(output_count))
    available_vbytes = target_vsize - current_vsize
    padding_count = available_vbytes // padding_vsize

    # Outputs add whole non-witness bytes, so their vsize contribution is
    # exact even when the original transaction has fractional witness weight.
    # Account for the larger CompactSize prefix before allocating outputs.
    def added_vsize(count):
        return count * padding_vsize + len(ser_compact_size(output_count + count)) - count_size

    while added_vsize(padding_count) > available_vbytes:
        padding_count -= 1

    padding_amount = padding_count * padding_value
    if tx.vout[0].nValue < padding_amount:
        raise RuntimeError("insufficient output value for transaction padding")

    tx.vout[0].nValue -= padding_amount
    tx.vout.extend(CTxOut(nValue=padding_value, scriptPubKey=padding_spk) for _ in range(padding_count))

    # The final gap is smaller than one more P2WSH output (including any
    # count-prefix growth), and fits in a small BitcoinII OP_RETURN output.
    remaining_vbytes = available_vbytes - added_vsize(padding_count)
    assert 0 <= remaining_vbytes < added_vsize(padding_count + 1) - added_vsize(padding_count)

    tail_output.scriptPubKey = CScript(
        [OP_RETURN] + [OP_1] * remaining_vbytes
    )

    assert_equal(tx.get_vsize(), target_vsize)


def output_key_to_p2tr_script(key):
    assert len(key) == 32
    return program_to_witness_script(1, key)


def check_key(key):
    if isinstance(key, str):
        key = bytes.fromhex(key)  # Assuming this is hex string
    if isinstance(key, bytes) and (len(key) == 33 or len(key) == 65):
        return key
    assert False


def check_script(script):
    if isinstance(script, str):
        script = bytes.fromhex(script)  # Assuming this is hex string
    if isinstance(script, bytes) or isinstance(script, CScript):
        return script
    assert False


def build_malleated_tx_package(*, parent: CTransaction, rebalance_parent_output_amount, child_amount):
    """
    Returns a transaction package with valid witness:
    - Parent transaction whose last output contains a script that has two spending conditions
    - Two malleated child transactions with same txid but different wtxids because of different witnesses

    Args:
        parent: Transaction with modifiable outputs. Either unsigned (sign after
        calling this function) or anyone-can-spend (e.g., MiniWallet's OP_TRUE).
    """
    hashlock = hash160(b'Preimage')
    witness_script = CScript([OP_IF, OP_HASH160, hashlock, OP_EQUAL, OP_ELSE, OP_TRUE, OP_ENDIF])
    witness_program = sha256(witness_script)
    script_pubkey = CScript([OP_0, witness_program])

    # Append to the transaction the vout containing the script supporting 2 spending conditions
    assert_greater_than_or_equal(len(parent.vout), 1)
    last_output = parent.vout[len(parent.vout) - 1]
    assert_greater_than_or_equal(last_output.nValue, rebalance_parent_output_amount)
    last_output.nValue -= rebalance_parent_output_amount
    parent.vout.append(CTxOut(rebalance_parent_output_amount, script_pubkey))


    # Create 2 valid children that differ only in witness data.
    # 1. Create a new transaction with witness solving first branch
    child_witness_script = CScript([OP_TRUE])
    child_witness_program = sha256(child_witness_script)
    child_script_pubkey = CScript([OP_0, child_witness_program])
    child_one = CTransaction()

    child_one.vin.append(CTxIn(COutPoint(int(parent.txid_hex, 16), len(parent.vout) - 1), b""))
    child_one.vout.append(CTxOut(child_amount, child_script_pubkey))
    child_one.wit.vtxinwit.append(CTxInWitness())
    child_one.wit.vtxinwit[0].scriptWitness.stack = [b'Preimage', b'\x01', witness_script]
    # 2. Create another identical transaction with witness solving second branch
    child_two = deepcopy(child_one)
    child_two.wit.vtxinwit[0].scriptWitness.stack = [b'', witness_script]
    return parent, child_one, child_two


class TestFrameworkScriptUtil(unittest.TestCase):
    def test_bulk_vout_compact_size(self):
        for witness_size in (None, 1, 2, 3, 4):
            for output_count, extra_vbytes, padding_count, tail_size in (
                (2, 0, 0, 0), (2, 1, 0, 1), (2, 42, 0, 42),
                (2, 43, 1, 0), (2, 44, 1, 1),
                (251, 85, 1, 42), (251, 86, 1, 43),
                (251, 87, 1, 44), (251, 88, 2, 0),
                (252, 42, 0, 42), (252, 43, 0, 43),
                (252, 44, 0, 44), (252, 45, 1, 0),
                (253, 43, 1, 0),
            ):
                with self.subTest(witness_size=witness_size, output_count=output_count, extra_vbytes=extra_vbytes):
                    tx = CTransaction()
                    tx.vin = [CTxIn(COutPoint(1, 0))]
                    tx.vout = [CTxOut(10_000, CScript([OP_TRUE])) for _ in range(output_count)]
                    tx.vout[0].nValue = 100_000
                    tx.vout[-1] = CTxOut(0, CScript([OP_RETURN]))
                    if witness_size is not None:
                        witness = CTxInWitness()
                        witness.scriptWitness.stack = [bytes(witness_size)]
                        tx.wit.vtxinwit = [witness]
                    target_vsize = tx.get_vsize() + extra_vbytes
                    expected = deepcopy(tx)
                    expected.vout[0].nValue -= padding_count * 10_000
                    expected.vout[-1].scriptPubKey = CScript([OP_RETURN] + [OP_1] * tail_size)
                    expected.vout.extend(CTxOut(10_000, CScript([OP_0, b'\x01' * 32])) for _ in range(padding_count))
                    bulk_vout(tx, target_vsize)
                    self.assertEqual(tx.serialize(), expected.serialize())
                    self.assertEqual(tx.get_vsize(), target_vsize)

    def test_bulk_vout_funding(self):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(1, 0))]
        tx.vout = [CTxOut(10_000, CScript([OP_TRUE])), CTxOut(0, CScript([OP_RETURN]))]
        original = tx.serialize()
        target_vsize = tx.get_vsize()
        with self.assertRaisesRegex(RuntimeError, "less than transaction virtual size"):
            bulk_vout(tx, target_vsize - 1)
        self.assertEqual(tx.serialize(), original)
        with self.assertRaisesRegex(RuntimeError, "insufficient output value"):
            bulk_vout(tx, target_vsize + 86)
        self.assertEqual(tx.serialize(), original)

        # The discarded probe output in the old loop needed funds even when
        # all actual padding outputs could be funded exactly.
        bulk_vout(tx, target_vsize + 43)
        self.assertEqual(tx.vout[0].nValue, 0)
        self.assertEqual(sum(output.nValue for output in tx.vout), 10_000)
        self.assertEqual(tx.get_vsize(), target_vsize + 43)

    def test_multisig(self):
        fake_pubkey = bytes([0]*33)
        # check correct encoding of P2MS script with n,k <= 16
        normal_ms_script = keys_to_multisig_script([fake_pubkey]*16, k=15)
        self.assertEqual(len(normal_ms_script), 1 + 16*34 + 1 + 1)
        self.assertTrue(normal_ms_script.startswith(bytes([OP_15])))
        self.assertTrue(normal_ms_script.endswith(bytes([OP_16, OP_CHECKMULTISIG])))

        # check correct encoding of P2MS script with n,k > 16
        max_ms_script = keys_to_multisig_script([fake_pubkey]*20, k=19)
        self.assertEqual(len(max_ms_script), 2 + 20*34 + 2 + 1)
        self.assertTrue(max_ms_script.startswith(bytes([1, 19])))  # using OP_PUSH1
        self.assertTrue(max_ms_script.endswith(bytes([1, 20, OP_CHECKMULTISIG])))
