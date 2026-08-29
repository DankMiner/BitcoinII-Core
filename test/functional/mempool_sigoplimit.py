#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test sigop limit mempool policy (`-bytespersigop` parameter)"""
from copy import deepcopy
from decimal import Decimal
from math import ceil

from test_framework.messages import (
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    WITNESS_SCALE_FACTOR,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_2DUP,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_DROP,
    OP_ENDIF,
    OP_FALSE,
    OP_IF,
    OP_NOT,
    OP_RETURN,
    OP_TRUE,
)
from test_framework.script_util import (
    program_to_witness_script,
    script_to_p2wsh_script,
    script_to_p2sh_script,
    MAX_STD_LEGACY_SIGOPS,
    MAX_STD_P2SH_SIGOPS,
)
from test_framework.test_framework import BitcoinIITestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import generate_keypair

DEFAULT_BYTES_PER_SIGOP = 20  # default setting
PACKAGE_SIGOPS = 20

class BytesPerSigOpTest(BitcoinIITestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def create_p2wsh_spending_tx(self, witness_script, output_script):
        """Create a 1-input-1-output P2WSH spending transaction with only the
           witness script in the witness stack and the given output script."""
        # create P2WSH address and fund it via MiniWallet first
        fund = self.wallet.send_to(
            from_node=self.nodes[0],
            scriptPubKey=script_to_p2wsh_script(witness_script),
            amount=1000000,
        )

        # create spending transaction
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(fund["txid"], 16), fund["sent_vout"]))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [bytes(witness_script)]
        tx.vout = [CTxOut(500000, output_script)]
        return tx

    @staticmethod
    def compact_size_len(value):
        if value < 253:
            return 1
        if value <= 0xffff:
            return 3
        if value <= 0xffffffff:
            return 5
        return 9

    def pad_transaction_to_vsize(self, tx, target_vsize):
        """Pad tx exactly using standard future-witness outputs."""
        current_vsize = tx.get_vsize()
        assert_greater_than_or_equal(target_vsize, current_vsize)

        if current_vsize == target_vsize:
            return

        delta = target_vsize - current_vsize
        initial_outputs = len(tx.vout)
        padding_value = 400

        for num_outputs in range(1, 5000):
            count_growth = (
                self.compact_size_len(initial_outputs + num_outputs)
                - self.compact_size_len(initial_outputs)
            )

            output_bytes = delta - count_growth

            if not 13 * num_outputs <= output_bytes <= 51 * num_outputs:
                continue

            extra_bytes = output_bytes - 13 * num_outputs
            total_value = padding_value * num_outputs

            if tx.vout[0].nValue <= total_value:
                raise AssertionError("Insufficient value for padding outputs")

            tx.vout[0].nValue -= total_value

            for _ in range(num_outputs):
                increase = min(38, extra_bytes)
                extra_bytes -= increase

                tx.vout.append(
                    CTxOut(
                        padding_value,
                        program_to_witness_script(
                            2,
                            b'\x66' * (2 + increase),
                        ),
                    )
                )

            assert_equal(extra_bytes, 0)
            assert_equal(tx.get_vsize(), target_vsize)
            return

        raise AssertionError(
            f"Unable to pad tx from {current_vsize} to {target_vsize}"
        )

    def test_sigops_limit(self, bytes_per_sigop, num_sigops):
        sigop_equivalent_vsize = ceil(num_sigops * bytes_per_sigop / WITNESS_SCALE_FACTOR)
        self.log.info(f"- {num_sigops} sigops (equivalent size of {sigop_equivalent_vsize} vbytes)")

        # create a template tx with the specified sigop cost in the witness script
        # (note that the sigops count even though being in a branch that's not executed)
        num_multisigops = num_sigops // 20
        num_singlesigops = num_sigops % 20
        witness_script = CScript(
            [OP_FALSE, OP_IF] +
            [OP_CHECKMULTISIG]*num_multisigops +
            [OP_CHECKSIG]*num_singlesigops +
            [OP_ENDIF, OP_TRUE]
        )
        # BitcoinII permits one OP_RETURN output with a complete
        # scriptPubKey no larger than 83 bytes. Keep OP_RETURN legal
        # and use standard future-witness outputs for size padding.
        base_tx = self.create_p2wsh_spending_tx(
            witness_script,
            CScript([OP_RETURN, b'X' * 80]),
        )

        assert_equal(len(base_tx.vout[0].scriptPubKey), 83)

        # Exactly at the sigop-equivalent vsize.
        tx = deepcopy(base_tx)
        self.pad_transaction_to_vsize(tx, sigop_equivalent_vsize)

        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(tx.get_vsize(), sigop_equivalent_vsize)
        assert_equal(res['vsize'], sigop_equivalent_vsize)

        # One vbyte above.
        tx = deepcopy(base_tx)
        self.pad_transaction_to_vsize(tx, sigop_equivalent_vsize + 1)

        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(tx.get_vsize(), sigop_equivalent_vsize + 1)
        assert_equal(res['vsize'], sigop_equivalent_vsize + 1)

        # One vbyte below. Sigop-adjusted vsize must still win.
        tx = deepcopy(base_tx)
        self.pad_transaction_to_vsize(tx, sigop_equivalent_vsize - 1)

        res = self.nodes[0].testmempoolaccept([tx.serialize().hex()])[0]
        assert_equal(res['allowed'], True)
        assert_equal(tx.get_vsize(), sigop_equivalent_vsize - 1)
        assert_equal(res['vsize'], sigop_equivalent_vsize)

        # Small form for the ancestor/descendant accounting checks.
        tx = deepcopy(base_tx)
        tx.vout[0].scriptPubKey = CScript([OP_RETURN, b'test123'])
        assert_greater_than(sigop_equivalent_vsize, tx.get_vsize())
        self.nodes[0].sendrawtransaction(hexstring=tx.serialize().hex(), maxburnamount='1.0')

        # fetch parent tx, which doesn't contain any sigops
        parent_txid = tx.vin[0].prevout.hash.to_bytes(32, 'big').hex()
        parent_tx = tx_from_hex(self.nodes[0].getrawtransaction(txid=parent_txid))

        entry_child = self.nodes[0].getmempoolentry(tx.txid_hex)
        assert_equal(entry_child['descendantcount'], 1)
        assert_equal(entry_child['descendantsize'], sigop_equivalent_vsize)
        assert_equal(entry_child['ancestorcount'], 2)
        assert_equal(entry_child['ancestorsize'], sigop_equivalent_vsize + parent_tx.get_vsize())

        entry_parent = self.nodes[0].getmempoolentry(parent_tx.txid_hex)
        assert_equal(entry_parent['ancestorcount'], 1)
        assert_equal(entry_parent['ancestorsize'], parent_tx.get_vsize())
        assert_equal(entry_parent['descendantcount'], 2)
        assert_equal(entry_parent['descendantsize'], parent_tx.get_vsize() + sigop_equivalent_vsize)

    def test_sigops_package(self):
        self.log.info("Test a overly-large sigops-vbyte hits package limits")
        # Make a 2-transaction package which fails vbyte checks even though
        # separately they would work.
        self.restart_node(
            0,
            extra_args=["-bytespersigop=5000"],
        )

        def create_sigop_tx(utxo_to_spend=None):
            _, pubkey = generate_keypair()

            total_sigop_value = 50000
            value_per_output = total_sigop_value // PACKAGE_SIGOPS

            tx_dict = self.wallet.create_self_transfer(
                fee=Decimal("3"),
                utxo_to_spend=utxo_to_spend,
            )

            tx_utxo = tx_dict["new_utxo"]
            tx = tx_dict["tx"]

            p2pk_script = CScript([
                pubkey,
                OP_CHECKSIG,
            ])

            for _ in range(PACKAGE_SIGOPS):
                tx.vout.append(
                    CTxOut(
                        value_per_output,
                        p2pk_script,
                    )
                )

            tx.vout[0].nValue -= total_sigop_value
            tx_utxo["txid"] = tx.txid_hex
            tx_utxo["value"] -= Decimal("0.00050000")

            return tx_utxo, tx

        tx_parent_utxo, tx_parent = create_sigop_tx()
        _tx_child_utxo, tx_child = create_sigop_tx(
            tx_parent_utxo
        )

        # Twenty CHECKSIG outputs produce the same twenty legacy
        # sigops previously obtained from bare CHECKMULTISIG.
        parent_individual_testres = self.nodes[0].testmempoolaccept(
            [tx_parent.serialize().hex()]
        )[0]

        assert parent_individual_testres["allowed"]

        sigop_adjusted_vsize = PACKAGE_SIGOPS * 5000
        assert_equal(
            parent_individual_testres["vsize"],
            sigop_adjusted_vsize,
        )

        # Together the two transactions exceed package limits.
        packet_test = self.nodes[0].testmempoolaccept([
            tx_parent.serialize().hex(),
            tx_child.serialize().hex(),
        ])

        expected_package_error = "too-large-cluster"
        assert_equal(
            [x["package-error"] for x in packet_test],
            [expected_package_error] * 2,
        )

        # When we actually try to submit, the parent makes it into the mempool, but the child would exceed cluster vsize limits
        res = self.nodes[0].submitpackage([tx_parent.serialize().hex(), tx_child.serialize().hex()])
        assert "too-large-cluster" in res["tx-results"][tx_child.wtxid_hex]["error"]
        assert tx_parent.txid_hex in self.nodes[0].getrawmempool()

        # Serialized transactions remain tiny compared with their
        # sigop-adjusted package vsize.
        assert_greater_than(
            sigop_adjusted_vsize,
            tx_parent.get_vsize() + tx_child.get_vsize(),
        )

    def test_legacy_sigops_stdness(self):
        self.log.info("Test a transaction with too many legacy sigops in its inputs is non-standard.")

        # Restart with the default settings
        self.restart_node(0)

        # Create a P2SH script with 15 sigops.
        _, dummy_pubkey = generate_keypair()
        packed_redeem_script = [dummy_pubkey]
        for _ in range(MAX_STD_P2SH_SIGOPS - 1):
            packed_redeem_script += [OP_2DUP, OP_CHECKSIG, OP_DROP]
        packed_redeem_script = CScript(packed_redeem_script + [OP_CHECKSIG, OP_NOT])
        packed_p2sh_script = script_to_p2sh_script(packed_redeem_script)

        # Create enough outputs to reach the sigops limit when spending them all at once.
        outpoints = []
        for _ in range(int(MAX_STD_LEGACY_SIGOPS / MAX_STD_P2SH_SIGOPS) + 1):
            res = self.wallet.send_to(from_node=self.nodes[0], scriptPubKey=packed_p2sh_script, amount=1_000)
            txid = int.from_bytes(bytes.fromhex(res["txid"]), byteorder="big")
            outpoints.append(COutPoint(txid, res["sent_vout"]))
        self.generate(self.nodes[0], 1)

        # Spending all these outputs at once accounts for 2505 legacy sigops and is non-standard.
        nonstd_tx = CTransaction()
        nonstd_tx.vin = [CTxIn(op, CScript([b"", packed_redeem_script])) for op in outpoints]
        nonstd_tx.vout = [CTxOut(0, CScript([OP_RETURN, b""]))]
        assert_raises_rpc_error(-26, "bad-txns-nonstandard-inputs", self.nodes[0].sendrawtransaction, nonstd_tx.serialize().hex())

        # Spending one less accounts for 2490 legacy sigops and is standard.
        std_tx = deepcopy(nonstd_tx)
        std_tx.vin.pop()
        self.nodes[0].sendrawtransaction(std_tx.serialize().hex())

        # Make sure the original, non-standard, transaction can be mined.
        self.generateblock(self.nodes[0], output="raw(42)", transactions=[nonstd_tx.serialize().hex()])

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])

        for bytes_per_sigop in (DEFAULT_BYTES_PER_SIGOP, 43, 81, 165, 327, 649, 1072):
            if bytes_per_sigop == DEFAULT_BYTES_PER_SIGOP:
                self.log.info(f"Test default sigops limit setting ({bytes_per_sigop} bytes per sigop)...")
            else:
                bytespersigop_parameter = f"-bytespersigop={bytes_per_sigop}"
                self.log.info(f"Test sigops limit setting {bytespersigop_parameter}...")
                self.restart_node(0, extra_args=[bytespersigop_parameter])

            for num_sigops in (69, 101, 142, 183, 222):
                self.test_sigops_limit(bytes_per_sigop, num_sigops)

            self.generate(self.wallet, 1)

        self.test_sigops_package()
        self.test_legacy_sigops_stdness()


if __name__ == '__main__':
    BytesPerSigOpTest(__file__).main()
