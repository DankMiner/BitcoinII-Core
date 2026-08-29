#!/usr/bin/env python3
# Copyright (c) 2026 The BitcoinII developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut
from test_framework.script import (
    CScript,
    OP_1,
    OP_13,
    OP_CHECKMULTISIG,
    OP_RETURN,
)
from test_framework.test_framework import BitcoinIITestFramework
from test_framework.util import assert_equal


class BitcoinIIDataConsensusTest(BitcoinIITestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
    def make_coinbase_block(self, extra_scripts):
        node = self.nodes[0]
        height = node.getblockcount() + 1
        prev_hash = int(node.getbestblockhash(), 16)
        prev_time = node.getblockheader(node.getbestblockhash())["time"]

        coinbase = create_coinbase(height)

        for script in extra_scripts:
            coinbase.vout.append(CTxOut(0, script))

        block = create_block(
            hashprev=prev_hash,
            coinbase=coinbase,
            ntime=prev_time + 1,
        )
        block.solve()
        return block

    def assert_rejected(self, scripts, reason):
        block = self.make_coinbase_block(scripts)
        assert_equal(
            self.nodes[0].submitblock(block.serialize().hex()),
            reason,
        )

    def run_test(self):
        self.log.info("Rejecting multiple OP_RETURN outputs in coinbase")

        self.assert_rejected(
            [
                CScript([OP_RETURN]),
                CScript([OP_RETURN]),
            ],
            "bad-txns-opreturn-count",
        )

        self.log.info("Rejecting an 84-byte OP_RETURN script")

        opreturn_84 = CScript([
            OP_RETURN,
            bytes([0x01] * 81),
        ])

        assert_equal(len(opreturn_84), 84)

        self.assert_rejected(
            [opreturn_84],
            "bad-txns-opreturn-size",
        )

        self.log.info("Rejecting actual OP_13 in OP_RETURN")

        self.assert_rejected(
            [
                CScript([
                    OP_RETURN,
                    OP_13,
                ])
            ],
            "bad-txns-opreturn-op13",
        )

        self.log.info("Rejecting bare multisig output")

        pubkey = bytes.fromhex(
            "02" + "11" * 32
        )

        assert_equal(len(pubkey), 33)

        bare_multisig = CScript([
            OP_1,
            pubkey,
            OP_1,
            OP_CHECKMULTISIG,
        ])

        self.assert_rejected(
            [bare_multisig],
            "bad-txns-bare-multisig",
        )

        self.log.info("Accepting exactly one 83-byte OP_RETURN")

        opreturn_83 = CScript([
            OP_RETURN,
            bytes([0x01] * 80),
        ])

        assert_equal(len(opreturn_83), 83)

        valid_block = self.make_coinbase_block([opreturn_83])

        assert_equal(
            self.nodes[0].submitblock(valid_block.serialize().hex()),
            None,
        )

        assert_equal(self.nodes[0].getblockcount(), 1)


if __name__ == "__main__":
    BitcoinIIDataConsensusTest(__file__).main()
