#!/usr/bin/env python3
# Copyright (c) 2026 1Miner.net
# Licensed under LICENSE-1MINER-BC2-MATURITY.md, including its prior-grant exception.
"""Exercise opt-in work-based coinbase maturity at the actual consensus bounds.

Three disconnected regtest nodes use different work thresholds. This tests the
minimum age independently of work, the work boundary independently of age, and
the maximum age with insufficient work. No mainnet parameters are changed.
"""

from decimal import Decimal

from test_framework.blocktools import (
    NORMAL_GBT_REQUEST_PARAMS,
    add_witness_commitment,
    create_block,
)
from test_framework.test_framework import BitcoinIITestFramework
from test_framework.util import assert_equal
from test_framework.wallet import getnewdestination


MIN_AGE = 4200
MAX_AGE = 12960
ACTIVATION_HEIGHT = 2
IMMATURE = "bad-txns-premature-spend-of-coinbase"
WORK_THRESHOLDS = [2, 2 * MIN_AGE, 1 << 128]


class CoinbaseWorkMaturityTest(BitcoinIITestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        # Debug builds check a growing block index while generating 21k blocks.
        self.rpc_timeout = 120
        self.extra_args = [[
            f"-testcoinbasematurityheight={ACTIVATION_HEIGHT}",
            f"-testcoinbasematuritywork={work:064x}",
        ] for work in WORK_THRESHOLDS]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # Different consensus parameters must never be connected to each other.
        self.setup_nodes()

    def mine_to_age(self, node, reward_height, age):
        """Age is relative to the NEXT block, as in mempool admission."""
        target_height = reward_height + age - 1
        assert node.getblockcount() <= target_height
        while node.getblockcount() < target_height:
            count = min(100, target_height - node.getblockcount())
            self.generatetoaddress(node, count, self.filler_address, sync_fun=self.no_op)
        assert_equal(node.getblockcount() + 1 - reward_height, age)

    def signed_spend(self, node, txid, amount=Decimal("49.999")):
        raw = node.createrawtransaction(
            [{"txid": txid, "vout": 0}],
            [{node.getnewaddress(): amount}],
        )
        signed = node.signrawtransactionwithwallet(raw)
        assert_equal(signed["complete"], True)
        return signed["hex"]

    def check_acceptance(self, node, raw, *, allowed):
        result = node.testmempoolaccept([raw])[0]
        assert_equal(result["allowed"], allowed)
        if not allowed:
            assert_equal(result["reject-reason"], IMMATURE)

    def submit_spend(self, node, raw, *, accepted):
        """Bypass mempool policy and exercise ConnectBlock consensus checks."""
        old_tip = node.getbestblockhash()
        block = create_block(tmpl=node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS), txlist=[raw])
        add_witness_commitment(block)
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None if accepted else IMMATURE)
        assert_equal(node.getbestblockhash(), block.hash_hex if accepted else old_tip)

    def check_wallet(self, node, reward, *, age, mature, work):
        status = node.gettransaction(reward)["coinbase_maturity"]
        assert_equal(status["work_based"], True)
        assert_equal(status["mature"], mature)
        assert_equal(status["blocks_to_minimum"], max(0, MIN_AGE - age))
        assert_equal(status["blocks_to_maximum"], max(0, MAX_AGE - age))
        # Regtest contributes exactly two units of work per block. The reward
        # block and candidate spending block must both be excluded.
        assert_equal(int(status["accumulated_work"], 16), 2 * (age - 1))
        assert_equal(int(status["required_work"], 16), work)
        assert_equal(reward in {coin["txid"] for coin in node.listunspent()}, mature)
        balance = node.getbalances()["mine"]
        assert_equal(balance["immature"], 0 if mature else 50)
        assert_equal(balance["trusted"], 100 if mature else 50)

    def check_reorg_removal(self, node, raw):
        """A rollback across either maturity boundary removes descendants too."""
        tip = node.getbestblockhash()
        parent = node.sendrawtransaction(raw)
        child_raw = self.signed_spend(node, parent, Decimal("49.998"))
        child = node.sendrawtransaction(child_raw)
        assert_equal(set(node.getrawmempool()), {parent, child})

        node.invalidateblock(tip)
        assert_equal(node.getrawmempool(), [])
        self.check_acceptance(node, raw, allowed=False)
        # Wallets remember evicted outgoing transactions. Release that local
        # reservation so later balance checks measure maturity, not a pending
        # wallet spend, and a restart cannot rebroadcast it.
        node.abandontransaction(parent)

        node.reconsiderblock(tip)
        self.check_acceptance(node, raw, allowed=True)

    def run_test(self):
        # Keep bulk mining outside the wallet, so tests do not scan or persist
        # twenty thousand wallet transactions just to exercise age boundaries.
        self.filler_address = getnewdestination()[2]
        rewards = []
        spends = []
        for node in self.nodes:
            blocks = self.generatetoaddress(node, 2, node.getnewaddress(), sync_fun=self.no_op)
            legacy, reward = [node.getblock(block)["tx"][0] for block in blocks]
            rewards.append(reward)
            spends.append(self.signed_spend(node, reward))
            legacy_spend = self.signed_spend(node, legacy)

            self.log.info(f"Node {node.index}: pre-activation rewards retain 100-block consensus maturity")
            self.mine_to_age(node, 1, 99)
            self.check_acceptance(node, legacy_spend, allowed=False)
            self.mine_to_age(node, 1, 100)
            self.check_acceptance(node, legacy_spend, allowed=True)
            assert_equal(node.gettransaction(legacy)["coinbase_maturity"]["work_based"], False)
            self.mine_to_age(node, 1, 101)
            assert_equal(node.gettransaction(legacy)["coinbase_maturity"]["mature"], True)
            self.check_acceptance(node, spends[-1], allowed=False)

        node = self.nodes[0]
        self.log.info("Sufficient work cannot bypass the 4,200-block minimum")
        self.mine_to_age(node, ACTIVATION_HEIGHT, MIN_AGE - 1)
        self.check_wallet(node, rewards[0], age=MIN_AGE - 1, mature=False, work=WORK_THRESHOLDS[0])
        self.check_acceptance(node, spends[0], allowed=False)
        self.submit_spend(node, spends[0], accepted=False)
        self.mine_to_age(node, ACTIVATION_HEIGHT, MIN_AGE)
        self.check_wallet(node, rewards[0], age=MIN_AGE, mature=True, work=WORK_THRESHOLDS[0])
        self.check_acceptance(node, spends[0], allowed=True)
        self.check_reorg_removal(node, spends[0])
        self.submit_spend(node, spends[0], accepted=True)

        node = self.nodes[1]
        self.log.info("Only work strictly after the reward and before the spending block counts")
        self.mine_to_age(node, ACTIVATION_HEIGHT, MIN_AGE)
        # 8,398 eligible work, with 8,400 required. Counting the reward itself,
        # its earlier chainwork, or the candidate block would incorrectly pass.
        self.check_wallet(node, rewards[1], age=MIN_AGE, mature=False, work=WORK_THRESHOLDS[1])
        self.check_acceptance(node, spends[1], allowed=False)
        self.submit_spend(node, spends[1], accepted=False)
        self.mine_to_age(node, ACTIVATION_HEIGHT, MIN_AGE + 1)
        self.check_wallet(node, rewards[1], age=MIN_AGE + 1, mature=True, work=WORK_THRESHOLDS[1])
        self.check_acceptance(node, spends[1], allowed=True)
        self.check_reorg_removal(node, spends[1])
        self.submit_spend(node, spends[1], accepted=True)

        node = self.nodes[2]
        self.log.info("The 12,960-block maximum overrides an unmet work requirement")
        self.mine_to_age(node, ACTIVATION_HEIGHT, MAX_AGE - 1)
        self.check_wallet(node, rewards[2], age=MAX_AGE - 1, mature=False, work=WORK_THRESHOLDS[2])
        self.check_acceptance(node, spends[2], allowed=False)
        self.submit_spend(node, spends[2], accepted=False)
        self.mine_to_age(node, ACTIVATION_HEIGHT, MAX_AGE)
        self.check_wallet(node, rewards[2], age=MAX_AGE, mature=True, work=WORK_THRESHOLDS[2])
        self.check_acceptance(node, spends[2], allowed=True)
        self.check_reorg_removal(node, spends[2])

        self.log.info("Rebuilding chainstate preserves work-based maturity and wallet eligibility")
        self.restart_node(2, extra_args=self.extra_args[2] + ["-reindex-chainstate"])
        self.check_wallet(node, rewards[2], age=MAX_AGE, mature=True, work=WORK_THRESHOLDS[2])
        self.check_acceptance(node, spends[2], allowed=True)
        self.submit_spend(node, spends[2], accepted=True)


if __name__ == "__main__":
    CoinbaseWorkMaturityTest(__file__).main()
