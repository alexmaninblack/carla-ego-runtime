# ADR 0005: Installed LibCarla package and C++20

- Status: Accepted
- Date: 2026-08-11

## Context

M1 needs the native CARLA client but the public runtime must not vendor CARLA,
refer to a developer's build-tree paths, or require Unreal Engine source. The
native LibCarla target on the Apple Silicon port requires C++20 and links a set
of static public dependencies.

The earlier runtime scaffold used C++17 and the first LibCarla install package
did not carry its transitive headers and archives, so a standalone consumer
could not compile.

## Decision

Consume LibCarla only through an installed `Carla::carla-client` CMake package
and compile the runtime as C++20. Pin the tested CARLA fork commit in the macOS
runbook. The CARLA fork installs its matching Boost, rpclib, Recast, libpng, and
zlib build artifacts and licence notices as part of that package.

Keep native connectivity behind `CARLA_EGO_WITH_CARLA`. The option is off by
default so argument parsing, tests, and documentation CI remain buildable
without CARLA. Production and integration builds explicitly enable it and
provide the install prefix through `CMAKE_PREFIX_PATH`.

## Consequences

The public repository contains no CARLA or Unreal source and no machine-local
paths. Native builds are reproducible against a pinned client package. Both
build modes use the same C++ language level, and changes to the package boundary
can be tested with a standalone consumer before the runtime is built.
