# Dynamic core plug-in ABI (draft)

Status: design specification only. No loader, shared-library target, or ABI
symbols are part of the project yet.

## Purpose

The current `gbb::EmulatorCore` API is a strong static, in-process C++ seam. It
is not a binary ABI: it contains STL types, exceptions, C++ ownership, and
implementation-owned pointers. A dynamic plug-in boundary must therefore be a
separate C-compatible interface rather than exporting `EmulatorCore` directly.

The first ABI should cover the stable emulation loop and leave advanced,
Game-Boy-specific services behind versioned capability extensions.

## Non-goals for ABI v1

- exporting C++ classes, RTTI, STL containers, or exceptions;
- guaranteeing that a plug-in can be unloaded while a core instance exists;
- making debugger, camera, printer, link-cable, or voxel services mandatory;
- allowing plug-ins to call host functions from arbitrary worker threads;
- promising source compatibility for the internal C++ API.

## Boundary rules

Every public ABI structure begins with `struct_size`, `abi_major`, and
`abi_minor`. Hosts accept the same major version and a minor version no newer
than they understand. New fields are appended only; a producer must never read
beyond the reported `struct_size`.

The ABI uses fixed-width integers, `uint8_t` flags, UTF-8 NUL-terminated
strings, caller-provided output buffers, and explicit result codes. It does not
use `bool`, `size_t`, C++ name mangling, compiler-specific packing, or shared
runtime allocation. Exported functions use a platform calling-convention macro
and `extern "C"`.

## Proposed entry point

```c
GBB_PLUGIN_EXPORT int GBB_PLUGIN_CALL
gbb_plugin_query(const gbb_host_v1* host, gbb_plugin_v1* out_plugin);
```

The host supplies logging, allocation, and capability negotiation callbacks.
The plug-in fills a descriptor and function table. A non-zero return means the
table is unusable; details are returned through the host error callback.

The host must validate all returned strings, counts, capability bits, required
function pointers, and descriptor invariants before exposing a plug-in core to
any frontend.

## Ownership and errors

The host owns ROM input and keeps it valid only for the `create` call. The
plug-in owns its opaque core handle and releases it through `destroy`. Buffers
returned by a plug-in are always written into host-provided memory, or are
returned as a `(pointer, byte_count, release)` triple whose release callback is
owned by the same module that allocated it. A host allocator callback is passed
to the plug-in for any unavoidable variable-sized result.

Functions return an explicit `gbb_result` (`ok`, `invalid_argument`,
`unsupported`, `buffer_too_small`, `invalid_state`, `internal_error`, or
`fatal`). No exception may cross the boundary. Error text is diagnostic only;
callers must branch on the result code.

## Core v1 surface

The initial function table should contain:

- `create(rom, options, out_handle)` and `destroy(handle)`;
- `reset(handle)`;
- `step_instruction(handle, out_cycles)`;
- `frame_ready(handle, out_ready)` and `consume_frame(handle)`;
- `video_frame(handle, caller_buffer, out_frame_info)`;
- `audio_read(handle, caller_buffer, capacity, out_sample_count)`;
- `set_input(handle, input_id, pressed)`;
- `save_state(handle, host_allocator, out_blob)` and `load_state(handle, blob)`;
- `rom_fingerprint(handle, out_fingerprint)`;
- persistent-data query/read/write operations;
- `query_extension(handle, extension_id, extension_struct)` for optional
  capability tables.

`video_frame` reports width, height, pitch, pixel format, and required byte
count. The host owns the destination buffer. `audio_read` reports the channel
count and sample rate through the descriptor and likewise writes into host
memory. This makes lifetime and allocator behavior unambiguous.

## Descriptor

The descriptor is the C representation of the currently validated static
metadata:

- stable core ID and display name;
- system ID, video dimensions, refresh/clock rates, and nominal cycles;
- audio sample rate/channels;
- input descriptors (numeric ID plus UTF-8 name);
- capability bitset;
- software title and ROM/save sizes;
- color and battery metadata;
- ABI version and descriptor structure size.

Descriptor strings and input arrays are plug-in-owned and remain valid until
the plug-in is unloaded. The host copies them if it needs longer lifetime.
Descriptor validation must reuse the semantic rules already enforced by
`validate_core_contract`; the C ABI adapter must not weaken those rules.

## Capability extensions

Optional features are separate, size-versioned tables selected by stable
extension IDs. A null table means unsupported. ABI v1 may define IDs for
printer, camera, debugger, sprite editing, link endpoint, and scene layers,
but the base table must remain usable without them.

Scene data should initially be exposed as a bounded serialized payload with an
explicit format ID and schema version. It must not expose `SceneSnapshot`, STL
vectors, or Game Boy register types directly. Link support should use the
existing core-neutral endpoint semantics rather than a concrete emulator
pointer.

## Threading and unload

All core calls are single-owner-thread operations unless an extension explicitly
says otherwise. Read-only descriptor queries may be repeated on that thread;
the host must not call a core concurrently with stepping, reset, save, or
destroy. Host callbacks are non-reentrant and must not call back into the same
core. A module may not be unloaded until every handle, extension table, pending
callback, and allocated result has been released.

## Compatibility and security

The loader must reject unsupported major versions, oversized counts, invalid
UTF-8/unterminated strings, unknown mandatory capability bits, missing required
function pointers, contradictory metadata, and results that exceed declared
buffers. Plug-ins are untrusted native code: loading is opt-in, paths are
explicit, and malformed plug-ins must fail closed without entering the SDL
frontend. A future sandbox or out-of-process mode is outside ABI v1.

## Required fixture and test plan

Before shipping a loader, add a tiny fixture plug-in and test it on every
supported native toolchain. The fixture suite must cover:

1. successful query, probe, create, frame, audio, input, state, and destroy;
2. major/minor and `struct_size` negotiation;
3. malformed descriptors and every rejected capability invariant;
4. caller-buffer-too-small and allocator/release ownership paths;
5. plug-in exceptions converted to result codes inside the plug-in;
6. concurrent-call rejection and destroy/unload ordering;
7. missing symbols, truncated tables, and failed library loading;
8. a plug-in compiled with a different C++ standard library/compiler runtime.

The fixture must be built as a separate shared library and loaded only by the
test host. The existing static registry and all 38 native contract tests remain
the compatibility baseline throughout this work.

## Migration order

1. Review and freeze this C ABI document and numeric IDs.
2. Add a public C header containing only the fixed-width ABI declarations.
3. Add the fixture plug-in and loader tests, without changing frontends.
4. Implement a host-side adapter from ABI v1 to `EmulatorCore` and run the
   existing contract validator at the adapter boundary.
5. Add explicit opt-in discovery and diagnostics to the desktop frontend.
6. Only then consider advanced capability extensions and other frontends.

Until these steps are complete, the static API remains the supported extension
mechanism and no dynamic plug-in ABI should be advertised as stable.
