BitcoinII
============

https://github.com/Bitcoin-II/BitcoinII-Core

BitcoinII is a BitcoinII full-node implementation derived from BitcoinII Core,
with a focus on database performance, storage efficiency, node configurability,
and selected forward-looking performance improvements.

BitcoinII v31.1. is based on Bitcoin Core v31.1 and remains compatible with
the BitcoinII network and BitcoinII consensus rules.

This initial BitcoinII release is provided as source code. Pre-built release
binaries are not currently provided.

What is BitcoinII?
---------------------

BitcoinII connects to the BitcoinII peer-to-peer network to download and fully
validate blocks and transactions. It also includes the BitcoinII wallet, RPC
interface, command-line utilities, and graphical user interface.

BitcoinII is not a separate cryptocurrency and does not define a separate
blockchain. It is an alternative BitcoinII node implementation built from the
BitcoinII Core codebase.

The primary changes in BitcoinII v31.1.1 include:

- LevelDB database and block-storage architecture aligned with Bitcoin Core
  v31.1.
- Parallel block-input prevout fetching during block validation, backported
  from post-v31.1 Bitcoin Core development.
- Consensus-enforced restrictions on arbitrary blockchain data storage.
- BitcoinII-specific branding, configuration naming, network parameters, and
  application integration.

Further technical information is available in the [doc folder](doc/).

BitcoinII-Specific Documentation
-----------------------------------

The following documents describe significant BitcoinII-specific behavior:

- [Parallel prevout fetching](doc/BitcoinII-Parallel-Prevout-Fetch.md)
- [Work-based coinbase maturity proposal](doc/coinbase-work-maturity.md) (disabled by default)
- [Maturity hashrate and regression tests](doc/coinbase-work-maturity-hashrate-tests.md)

Build Dependencies
------------------

Users building BitcoinII from source are encouraged to use the BitcoinII
Depends system for reproducible dependency builds.

See the platform-specific build documentation in the [doc folder](doc/) and
the [depends documentation](depends/README.md) for additional information.

Configuration
-------------

The primary BitcoinII configuration file is:

    bitcoinII.conf

BitcoinII retains the familiar Bitcoin Core configuration model and
command-line option format while adding BitcoinII-specific functionality.


Development
-----------

The `main` branch contains the current BitcoinII development and release
history.

Official source release points are identified with version tags such as:

    v31.1.1

BitcoinII is derived from Bitcoin Core and continues to incorporate relevant
upstream Bitcoin Core development while maintaining the BitcoinII-specific
storage, database, performance, policy, and application changes.

The contribution workflow is described in
[CONTRIBUTING.md](CONTRIBUTING.md), and developer information can be found in
[doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

BitcoinII inherits Bitcoin Core's extensive unit, functional, fuzz, and
integration testing infrastructure and adds or modifies tests where required
for BitcoinII-specific functionality.

Unit tests can be compiled and executed with CTest when tests were enabled
during build configuration:

    ctest

Further information about unit tests is available in
[src/test/README.md](src/test/README.md).

Functional and integration tests are located under [test/](test/) and can be
run using the functional test runner from the configured build tree.

BitcoinII modifies security-critical Bitcoin software. Changes should be
reviewed and tested carefully, particularly changes affecting validation,
database handling, transaction policy, block storage, or wallet behavior.

Upstream Bitcoin Core
---------------------

BitcoinII is derived from the Bitcoin Core project:

https://bitcoincore.org

Bitcoin Core source code is available at:

https://github.com/bitcoin/bitcoin

BitcoinII retains substantial Bitcoin Core code, documentation, testing
infrastructure, and copyright attribution.

## Licensing

This repository contains material under different licenses. Bitcoin Core-derived
code retains its MIT license. Original work-based maturity contributions have
separate 1Miner.net terms, and ShockWave has the separate terms below.

### Work-based coinbase maturity

Copyright (c) 2026 1Miner.net. The
[1Miner.net BitcoinII Work-Based Maturity License](LICENSE-1MINER-BC2-MATURITY.md)
permits use, modification, and distribution for BitcoinII at no charge, including
mining, node operation, wallets, pools, exchanges, development, and testing.
It does not authorize use of covered material on other production networks
without written permission. See the
[scope record](doc/coinbase-work-maturity-license-scope.md) for the exact
contribution and exclusions.

The earlier maturity commit `3dbafb5` was published with MIT notices. The new
terms do not revoke rights granted by that earlier distribution. Upstream
licenses and independently written implementations are unaffected.

The maturity change is available for developer review and private regtest
testing. Production activation is disabled. See the
[proposal implementation](doc/coinbase-work-maturity.md) and
[validation report](doc/coinbase-work-maturity-validation.md).

### MIT License
-------

Large portions of BitcoinII are released under the MIT license, as inherited from upstream Bitcoin Core.

See [COPYING](COPYING) for the full license text or:

https://opensource.org/license/MIT

## License pertaining to the "ShockWave" Difficulty Adjustment Algorithm:

### ShockWave Difficulty Adjustment Algorithm

**Exception:** Original source code and implementation material comprising the **ShockWave Difficulty Adjustment Algorithm** is **not distributed under the MIT license**.

Portions of the ShockWave implementation derived from Bitcoin Core, Dash/Darkcoin, or other pre-existing MIT-licensed software remain subject to their respective MIT license terms. The restrictions below apply only to original ShockWave material for which KvantaMechanic & Second Chance Digital, LLC. holds the applicable copyright.

### ShockWave Proprietary Source-Review License

Copyright (c) 2026 KvantaMechanic & Second Chance Digital, LLC
All rights reserved.

Except for portions independently subject to the MIT license as described above, the original ShockWave source code and implementation contained in BitcoinII Core are proprietary software.

The ShockWave source is made publicly available solely for:

* security review;
* technical and consensus audit;
* interoperability analysis;
* academic or technical evaluation; and
* testing of the BitcoinII implementation.

Permission is granted to view the ShockWave source code and to make temporary copies strictly as necessary to inspect, compile, execute, and test it for the review and evaluation purposes listed above.

**NO LICENSE IS GRANTED** to incorporate, deploy, reuse, redistribute, or exploit the proprietary ShockWave implementation, in whole or in substantial part, in another blockchain, cryptocurrency, distributed-ledger system, software product, service, protocol, or other implementation.

Without prior express written permission from KvantaMechanic & Second Chance Digital, LLC. you may not:

* incorporate proprietary ShockWave source code, or any substantial portion of it, into another project;
* copy, adapt, modify, translate, port, or create derivative works from the proprietary ShockWave implementation except as strictly necessary for permitted review and testing;
* redistribute, republish, sublicense, sell, license, or commercially exploit the proprietary ShockWave implementation;
* deploy the proprietary ShockWave implementation, or a derivative of it, on another blockchain, cryptocurrency, distributed-ledger network, software product, service, or protocol;
* use the proprietary ShockWave implementation as the basis for another production difficulty-adjustment implementation; or
* remove, obscure, or alter this copyright or license notice.

The limited permissions granted for review, audit, compilation, execution, and testing do not constitute an open-source license and grant no rights except those expressly stated.

Nothing in this notice restricts rights independently granted under the MIT license with respect to Bitcoin Core, Dash/Darkcoin, or other pre-existing MIT-licensed code. The proprietary restrictions above apply only to original ShockWave material for which KvantaMechanic & Second Chance Digital, LLC. holds the applicable copyright.

Any use of proprietary ShockWave material outside the expressly permitted review and evaluation purposes requires prior express written authorization from KvantaMechanic & Second Chance Digital, LLC.

THE PROPRIETARY SHOCKWAVE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY ARISING FROM, OUT OF, OR IN CONNECTION WITH THE PROPRIETARY SHOCKWAVE SOFTWARE OR ITS USE.


Source Repository
-----------------

The official BitcoinII source repository is:

https://github.com/Bitcoin-II/BitcoinII-Core

Issues and source-development reports may be submitted through:

https://github.com/Bitcoin-II/BitcoinII-Core/issues

