# Native plug-in security and UX policy

Native plug-ins execute in the emulator process and therefore have the same
authority as the desktop application. They are an advanced extension point,
not a sandbox. A malformed or hostile plug-in can read files available to the
user, access the network, or terminate the process.

## Current policy

- Discovery is disabled by default.
- A user must explicitly enable `plugin.Discovery` and provide one or more
  `plugin.Path` entries. No working-directory, system-directory, or implicit
  environment scan is performed.
- Paths are canonicalized, symbolic links are rejected, only native shared
  library extensions are considered, and discovery is capped at 32 candidates.
- `plugin.RequireAllowlist = true` makes the exact `core_id` allowlist mandatory;
  each accepted identity is supplied with a repeated `plugin.AllowCore` entry.
- `plugin.RequireCapabilityAllowlist = true` makes the capability permission
  allowlist mandatory; each permitted capability is supplied with a repeated
  `plugin.AllowCapability` entry. Unknown capability names are rejected.
- ABI, descriptor, capability, metadata, and core-contract validation occurs
  before a plug-in is registered.
- Android, Web, and other sandboxed/non-native targets do not load native
  plug-ins.

The allowlist is an identity and configuration policy. It is not a
cryptographic signature and must not be treated as proof that a library is
safe. Users should only allow libraries obtained from a trusted source.
Capability policy is an additional least-privilege gate; it does not sandbox
native code or replace adapter-level support checks.
Host integrations can also provide a synchronous trust callback. It runs after
ABI, identity, and capability validation but before registry admission, and a
false decision fails closed. This is the enforcement boundary for a future
interactive prompt; it is not itself a signature verifier.
The shared `plugin_sha256_file` helper can be used by that callback to pin
approved binary digests. Digest pinning detects replacement or tampering but
does not establish publisher identity, so it cannot replace a signed manifest.

## UX requirements

The desktop Settings page exposes whether discovery is enabled, whether the
identity and capability allowlists are required, how many plug-ins loaded, and
the latest rejection reason. Changes are persisted to `settings.ini` and take
effect after restart so an active core cannot be replaced underneath an
emulation session. Detailed path, capability, and ABI diagnostics remain
available through the shared logger.

## Future gate before general-user enablement

1. Implement the signed manifest and trust-store contract in
   [`plugin-manifest.md`](plugin-manifest.md), using a vetted cross-platform
   Ed25519 backend.
2. Add a user-visible trust prompt for first use and key changes.
3. Add an interactive first-use capability prompt layered on top of the
   settings allowlist.
4. Evaluate out-of-process hosting for untrusted or third-party cores.
5. Add CI coverage for signed, revoked, and tampered plugin packages.
