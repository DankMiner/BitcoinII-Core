#!/usr/bin/env python3
# Copyright (c) 2026 The BitcoinII developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from feature_taproot import (
    TaprootTest,
    add_spender,
)

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.key import (
    compute_xonly_pubkey,
    generate_privkey,
)
from test_framework.script import (
    ANNEX_TAG,
    CScript,
    OP_0,
    OP_1,
    OP_ENDIF,
    OP_IF,
    OP_NOP,
    taproot_construct,
)
from test_framework.util import assert_equal


MAX_BITCOINII_TAPSCRIPT_BYTES = 3600


class BitcoinIITaprootConsensusTest(TaprootTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        node = self.nodes[0]

        # TaprootTest uses a clean regtest chain. Mine enough wallet funds
        # to create and spend our custom P2TR outputs.
        self.log.info("Mining mature test funds")
        self.generate(node, COINBASE_MATURITY + 1)

        spenders = []

        # ============================================================
        # 1. Ordinary Taproot key-path spending remains valid.
        # ============================================================
        self.log.info("Preparing Taproot key-path acceptance test")

        keypath_sec = generate_privkey()
        keypath_pub = compute_xonly_pubkey(keypath_sec)[0]
        keypath_tap = taproot_construct(keypath_pub)

        add_spender(
            spenders,
            comment="bitcoinII/keypath-allowed",
            tap=keypath_tap,
            key=keypath_sec,
        )

        # ============================================================
        # 2. Exactly 3,600 bytes of revealed tapscript remains valid.
        #
        # 3,599 OP_NOP bytes + OP_1 gives a valid script which leaves
        # true on the stack and is exactly 3,600 bytes long.
        # ============================================================
        self.log.info("Preparing 3,600-byte tapscript acceptance test")

        size_sec = generate_privkey()
        size_pub = compute_xonly_pubkey(size_sec)[0]

        script_3600 = CScript(
            [OP_NOP] * (MAX_BITCOINII_TAPSCRIPT_BYTES - 1)
            + [OP_1]
        )

        assert_equal(
            len(script_3600),
            MAX_BITCOINII_TAPSCRIPT_BYTES,
        )

        size_tap = taproot_construct(
            size_pub,
            [("limit", script_3600)],
        )

        add_spender(
            spenders,
            comment="bitcoinII/tapscript-3600-allowed",
            tap=size_tap,
            leaf="limit",
            inputs=[],
        )

        # ============================================================
        # 3. 3,601-byte revealed tapscript is consensus-invalid.
        #
        # The valid form uses the actual committed 3,600-byte leaf.
        # The failing form substitutes a 3,601-byte witness script.
        # BitcoinII must reject it with our consensus reason before
        # ordinary Taproot commitment/script validation can matter.
        # ============================================================
        self.log.info("Preparing 3,601-byte tapscript rejection test")

        script_3601 = CScript(
            [OP_NOP] * MAX_BITCOINII_TAPSCRIPT_BYTES
            + [OP_1]
        )

        assert_equal(
            len(script_3601),
            MAX_BITCOINII_TAPSCRIPT_BYTES + 1,
        )

        add_spender(
            spenders,
            comment="bitcoinII/tapscript-3601-rejected",
            tap=size_tap,
            leaf="limit",
            inputs=[],
            failure={
                "script_taproot": script_3601,
            },
            err_msg="bad-txns-tapscript-size",
        )

        # ============================================================
        # 4. Taproot annex is consensus-invalid.
        # ============================================================
        self.log.info("Preparing Taproot annex rejection test")

        annex_sec = generate_privkey()
        annex_pub = compute_xonly_pubkey(annex_sec)[0]

        annex_script = CScript([OP_1])

        annex_tap = taproot_construct(
            annex_pub,
            [("normal", annex_script)],
        )

        add_spender(
            spenders,
            comment="bitcoinII/taproot-annex-rejected",
            tap=annex_tap,
            leaf="normal",
            inputs=[],
            failure={
                "annex": bytes([ANNEX_TAG, 0x01]),
            },
            err_msg="bad-txns-taproot-annex",
        )

        # ============================================================
        # 5. Ordinals inscription envelope is consensus-invalid.
        #
        # Valid witness uses OP_1.
        # Failing witness substitutes:
        #
        #   OP_FALSE OP_IF "ord" OP_ENDIF OP_1
        #
        # which is otherwise an executable/successful tapscript.
        # ============================================================
        self.log.info("Preparing Ordinals envelope rejection test")

        ord_sec = generate_privkey()
        ord_pub = compute_xonly_pubkey(ord_sec)[0]

        normal_script = CScript([OP_1])

        ord_tap = taproot_construct(
            ord_pub,
            [("normal", normal_script)],
        )

        ordinal_script = CScript([
            OP_0,
            OP_IF,
            b"ord",
            OP_ENDIF,
            OP_1,
        ])

        add_spender(
            spenders,
            comment="bitcoinII/ordinal-envelope-rejected",
            tap=ord_tap,
            leaf="normal",
            inputs=[],
            failure={
                "script_taproot": ordinal_script,
            },
            err_msg="bad-txns-ordinal-envelope",
        )

        # ============================================================
        # 6. Incidental "ord" bytes are NOT forbidden.
        #
        # This deliberately contains "ord" inside an unexecuted
        # OP_FALSE/OP_IF branch, but the pushed element is not the
        # actual three-byte Ordinals protocol marker.
        # ============================================================
        self.log.info("Preparing incidental ord-bytes acceptance test")

        incidental_sec = generate_privkey()
        incidental_pub = compute_xonly_pubkey(incidental_sec)[0]

        incidental_script = CScript([
            OP_0,
            OP_IF,
            b"not-an-ord-envelope",
            OP_ENDIF,
            OP_1,
        ])

        incidental_tap = taproot_construct(
            incidental_pub,
            [("incidental", incidental_script)],
        )

        add_spender(
            spenders,
            comment="bitcoinII/incidental-ord-bytes-allowed",
            tap=incidental_tap,
            leaf="incidental",
            inputs=[],
        )

        # Run every rule independently so a failure identifies exactly
        # which BitcoinII consensus boundary broke.
        self.test_spenders(
            node,
            spenders,
            input_counts=[1],
        )


if __name__ == "__main__":
    BitcoinIITaprootConsensusTest(__file__).main()
