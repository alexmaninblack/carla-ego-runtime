# Public repository policy

This repository is public. Every committed file must be safe to redistribute
under its declared licence.

## Never commit

- Epic Games or Unreal Engine source code from a restricted repository;
- Unreal Engine binaries, libraries, headers, assets, Derived Data Cache, or
  build output;
- `.uasset`, `.umap`, cooked packages, CARLA content packs, or screenshots that
  contain confidential material;
- access tokens, credentials, cookies, private keys, `.env` files, or private
  Git remote URLs containing credentials;
- third-party code or generated files without a compatible licence and the
  required attribution;
- local logs or crash dumps that may expose usernames, paths, or secrets.

## CARLA boundary

CARLA is an open-source MIT-licensed project, but this runtime should normally
consume an installed LibCarla package rather than copy CARLA source. If a small
CARLA-derived portion ever becomes necessary, it requires an explicit licence
review and attribution in the same change.

The private Unreal Engine repository is a build prerequisite for the simulator,
not a dependency to distribute from this repository.

## COVESA boundary

The project references COVESA VISS and VSS specifications and uses their public
signal names. Their source repositories are licensed under MPL-2.0. Do not copy
or generate distributable COVESA source artifacts into this repository without
recording the exact upstream version, preserving required notices, and
reviewing the resulting licence obligations.

The project-owned `Vehicle.CarlaSimulation.*` overlay must remain clearly
identified as an extension rather than an official part of COVESA VSS.

## Before every public push

1. Review the complete staged file list and diff.
2. Check for credentials, private URLs, binaries, archives, and oversized files.
3. Confirm new dependencies and copied material have compatible licences.
4. Build and test from tracked files only.
5. Keep generated output outside Git.

If redistribution rights are uncertain, do not commit the material until the
licence is confirmed.
