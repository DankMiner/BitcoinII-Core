#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the bitcoinII wrapper tool."""
from test_framework.test_framework import (
    BitcoinIITestFramework,
    SkipTest,
)
from test_framework.util import (
    append_config,
    assert_equal,
)

import platform
import re


class ToolBitcoinIITest(BitcoinIITestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        # Skip test on windows because currently when `bitcoinII node -version` is
        # run on windows, python doesn't capture output from the child
        # `bitcoinII-d` and `bitcoinII-node` process started with _wexecvp, and
        # stdout/stderr are always empty. See
        # https://github.com/bitcoin/bitcoin/pull/33229#issuecomment-3265524908
        if platform.system() == "Windows":
            raise SkipTest("Test does not currently work on windows")

    def setup_network(self):
        """Set up nodes normally, but save a copy of their arguments before starting them."""
        self.add_nodes(self.num_nodes, self.extra_args)
        node_argv = self.get_binaries().node_argv()
        self.node_options = [node.args[len(node_argv):] for node in self.nodes]
        assert all(node.args[:len(node_argv)] == node_argv for node in self.nodes)

    def set_cmd_args(self, node, args):
        """Set up node so it will be started through bitcoinII wrapper command with specified arguments."""
        # Manually construct the `bitcoinII node` command, similar to Binaries::node_argv()
        bitcoinII_cmd = node.binaries.valgrind_cmd + [node.binaries.paths.bitcoinII_bin]
        node.args = bitcoinII_cmd + args + ["node"] + self.node_options[node.index]

    def test_args(self, cmd_args, node_args, expect_exe=None, expect_error=None):
        node = self.nodes[0]
        self.set_cmd_args(node, cmd_args)
        extra_args = node_args + ["-version"]
        if expect_error is not None:
            node.assert_start_raises_init_error(expected_msg=expect_error, extra_args=extra_args)
        else:
            assert expect_exe
            node.start(extra_args=extra_args)
            ret, out, err = get_node_output(node)
            try:
                assert_equal(get_exe_name(out), expect_exe.encode())
                assert_equal(err, b"")
            except Exception as e:
                raise RuntimeError(f"Unexpected output from {node.args + extra_args}: {out=!r} {err=!r} {ret=!r}") from e

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Ensure bitcoinII node command invokes bitcoinII-d by default")
        self.test_args([], [], expect_exe="bitcoinII-d")

        self.log.info("Ensure bitcoinII -M invokes bitcoinII-d")
        self.test_args(["-M"], [], expect_exe="bitcoinII-d")

        self.log.info("Ensure bitcoinII -M does not accept -ipcbind")
        self.test_args(["-M"], ["-ipcbind=unix"], expect_error='Error: Error parsing command line arguments: Invalid parameter -ipcbind=unix')

        if self.is_ipc_compiled():
            self.log.info("Ensure bitcoinII -m invokes bitcoinII-node")
            self.test_args(["-m"], [], expect_exe="bitcoinII-node")

            self.log.info("Ensure bitcoinII -m does accept -ipcbind")
            self.test_args(["-m"], ["-ipcbind=unix"], expect_exe="bitcoinII-node")

            self.log.info("Ensure bitcoinII accepts -ipcbind by default")
            self.test_args([], ["-ipcbind=unix"], expect_exe="bitcoinII-node")

            self.log.info("Ensure bitcoinII recognizes -ipcbind in config file")
            append_config(node.datadir_path, ["ipcbind=unix"])
            self.test_args([], [], expect_exe="bitcoinII-node")


def get_node_output(node):
    ret = node.process.wait(timeout=60)
    node.stdout.seek(0)
    node.stderr.seek(0)
    out = node.stdout.read()
    err = node.stderr.read()
    node.stdout.close()
    node.stderr.close()

    # Clean up TestNode state
    node.running = False
    node.process = None
    node.rpc_connected = False
    node.rpc = None

    return ret, out, err


def get_exe_name(version_str):
    """Get exe name from last word of first line of version string."""
    return re.match(rb".*?(\S+)\s*?(?:\n|$)", version_str.strip()).group(1)


if __name__ == '__main__':
    ToolBitcoinIITest(__file__).main()
