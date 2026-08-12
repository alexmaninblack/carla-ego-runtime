# Third-party dependencies

This source repository does not vendor third-party source or binaries. Optional
native builds locate dependencies already installed by the developer.

| Dependency | Use | Licence |
| --- | --- | --- |
| [CARLA LibCarla](https://github.com/carla-simulator/carla) | Simulator RPC and sensor client | MIT |
| [Boost](https://www.boost.org/) | Asio, Beast, JSON, and System components for the VISS endpoint and client | Boost Software License 1.0 |
| [OpenSSL](https://www.openssl.org/) | TLS 1.2+ endpoint and client | Apache License 2.0 |
| [NetworkX](https://networkx.org/) | CARLA BehaviorAgent route graph, local M5 tool only | BSD-3-Clause |
| [NumPy](https://numpy.org/) | CARLA BehaviorAgent calculations, local M5 tool only | BSD-3-Clause |
| [Shapely](https://shapely.readthedocs.io/) | CARLA BehaviorAgent obstacle geometry, local M5 tool only | BSD-3-Clause |

The tested native build obtains Boost public headers and its applicable licence
files from the installed LibCarla package and obtains OpenSSL from Homebrew. A
future packaged binary distribution must include the dependency notices and
licence texts required by the exact redistributed artifacts.

COVESA VISS and VSS repositories define the public interface and signal names.
No COVESA implementation source, schema, generated artifact, or binary is
redistributed here. The clean implementation decision and MPL-2.0 analysis are
recorded in
[ADR 0007](docs/decisions/0007-embedded-viss-endpoint.md).
