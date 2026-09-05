# Dynamic core plug-in ABI v1.0 freeze record

Status: **approved and frozen**  
Effective date: 2026-09-05  
Baseline: the freeze commit on `main` (immediately after `86129ee`)

This record approves the public C dynamic-core ABI as the Go Bigger Boy v1.0
plug-in contract. The freeze covers `include/gbb/plugin_abi.h`, its numeric
identifiers, structure layouts, required prefixes, calling convention, and the
ownership, error, threading, and unload rules in [`plugin-abi.md`](plugin-abi.md).

## Immutable v1.0 contract

- `GBB_PLUGIN_ABI_MAJOR` is `1` and `GBB_PLUGIN_ABI_MINOR` is `0`.
- Result, system, input, capability, pixel-format, and persistent-data IDs are
  fixed. Existing values must never be renumbered or reused.
- Versioned tables are append-only. A future minor may add fields after the
  v1.0 required prefix, but v1.0 producers and hosts must not read unknown
  storage. A major version is required for field reordering, changed meaning,
  changed calling convention, or incompatible ownership/error semantics.
- The ABI uses fixed-width C types, UTF-8 NUL-terminated strings, explicit
  result codes, caller-provided buffers, and module-matched release callbacks.
  C++ exceptions, STL types, RTTI, and C++ ownership never cross the boundary.
- The supported v1.0 native matrix is 64-bit Windows, Linux, and macOS. The
  reference header test locks the 64-bit sizes and selected offsets.
- Core calls are single-owner-thread operations. A module remains loaded until
  all handles, extension tables, callbacks, and allocated results are released.

## Approval checks

The following checks passed before this approval was recorded:

- C header compilation and 64-bit layout assertions;
- GCC and Clang ABI fixture/loader suites, including a mixed GCC-host/Clang-
  fixture run;
- MSVC Windows ABI and full desktop contract suite;
- Linux and both macOS architectures;
- Address/Undefined and Thread Sanitizer suites, including the Xvfb SDL smoke;
- bounded parser fuzz smoke and accuracy ROM baseline;
- Android and Web build workflows;
- local native build with all 37 registered tests passing.

The fixture intentionally tests exception conversion *inside* the plug-in. A
fixture that throws across the shared-library boundary is not valid C++ ABI
coverage and is excluded from the freeze matrix.

## Change control after freeze

Changes to the v1.0 prefix or numeric values require a new ABI major and a new
freeze record. Append-only additions may use a future minor version only after
updating the compatibility matrix, fixture coverage, and this record. Bug
fixes to the reference loader that preserve the contract do not change the ABI
version, but must retain the frozen layout and identifier assertions.

Automatic plug-in discovery, sandboxing, and frontend UX are deliberately not
part of this freeze. They can be added later without changing the v1.0 binary
contract, subject to a separate security and product review.
