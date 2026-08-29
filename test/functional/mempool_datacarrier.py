#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Copyright (c) 2026 The BitcoinII developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Test BitcoinII fixed OP_RETURN relay rules."""

from random import randbytes

from test_framework.messages import CTxOut
from test_framework.script import (
    CScript,
    OP_13,
    OP_RETURN,
)
from test_framework.test_framework import BitcoinIITestFramework
from test_framework.test_node import TestNode
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import MiniWallet


BITCOINII_MAX_OP_RETURN_BYTES = 83


class DataCarrierTest(BitcoinIITestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def make_transaction(self, scripts):
        tx = self.wallet.create_self_transfer(fee_rate=0)["tx"]

        for script in scripts:
            tx.vout.append(
                CTxOut(
                    nValue=0,
                    scriptPubKey=script,
                )
            )

        # Pay 1 sat2/vbyte.
        tx.vout[0].nValue -= tx.get_vsize()

        return tx

    def submit_scripts(
        self,
        node: TestNode,
        scripts,
        *,
        success: bool,
        reject_reason: str = "",
    ) -> None:
        tx = self.make_transaction(scripts)
        tx_hex = tx.serialize().hex()

        if success:
            self.wallet.sendrawtransaction(
                from_node=node,
                tx_hex=tx_hex,
            )
            assert tx.txid_hex in node.getrawmempool(True)
        else:
            assert_raises_rpc_error(
                -26,
                reject_reason,
                self.wallet.sendrawtransaction,
                from_node=node,
                tx_hex=tx_hex,
            )

    def run_test(self):
        node = self.nodes[0]
        self.wallet = MiniWallet(node)

        self.log.info("Checking fixed BitcoinII relay settings")
        info = node.getmempoolinfo()

        assert_equal(
            info["maxdatacarriersize"],
            BITCOINII_MAX_OP_RETURN_BYTES,
        )
        assert_equal(
            info["permitbaremultisig"],
            False,
        )

        # OP_RETURN + OP_PUSHDATA1 + length byte + 80 data bytes
        # is exactly 83 bytes.
        exactly_83 = CScript([
            OP_RETURN,
            randbytes(80),
        ])

        assert_equal(
            len(exactly_83),
            BITCOINII_MAX_OP_RETURN_BYTES,
        )

        self.log.info("Accepting exactly one 83-byte OP_RETURN")
        self.submit_scripts(
            node,
            [exactly_83],
            success=True,
        )

        # 81 bytes of pushed data makes the complete script 84 bytes.
        too_large = CScript([
            OP_RETURN,
            randbytes(81),
        ])

        assert_equal(
            len(too_large),
            BITCOINII_MAX_OP_RETURN_BYTES + 1,
        )

        self.log.info("Rejecting an 84-byte OP_RETURN")
        self.submit_scripts(
            node,
            [too_large],
            success=False,
            reject_reason="bitcoinII-opreturn-size",
        )

        self.log.info("Rejecting two OP_RETURN outputs")
        self.submit_scripts(
            node,
            [
                CScript([OP_RETURN]),
                CScript([OP_RETURN]),
            ],
            success=False,
            reject_reason="bitcoinII-opreturn-count",
        )

        self.log.info("Rejecting actual OP_13 inside OP_RETURN")
        self.submit_scripts(
            node,
            [CScript([OP_RETURN, OP_13])],
            success=False,
            reject_reason="bitcoinII-opreturn-op13",
        )

        self.log.info("Accepting byte 0x5d when it is pushed data")
        self.submit_scripts(
            node,
            [CScript([OP_RETURN, bytes([0x5d])])],
            success=True,
        )

        self.log.info("Accepting small ordinary OP_RETURN outputs")
        self.submit_scripts(
            node,
            [CScript([OP_RETURN])],
            success=True,
        )

        self.submit_scripts(
            node,
            [CScript([OP_RETURN, b"BitcoinII"])],
            success=True,
        )


if __name__ == '__main__':
    DataCarrierTest(__file__).main()
