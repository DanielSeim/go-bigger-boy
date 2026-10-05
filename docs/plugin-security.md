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
- Configured relative paths are resolved beside `settings.ini`. Candidate paths
  are canonicalized when possible; directly configured symlinks and symlink
  directory entries are rejected. This is not an all-parent-components symlink
  check or a race-free filesystem sandbox. Only native shared library
  extensions are considered. The default limit is 32 candidates; API callers
  can change it up to the implementation ceiling of 256.
- `plugin.RequireAllowlist = true` makes the exact `core_id` allowlist mandatory;
  each accepted identity is supplied with a repeated `plugin.AllowCore` entry.
- `plugin.RequireCapabilityAllowlist = true` makes the capability permission
  allowlist mandatory; each permitted capability is supplied with a repeated
  `plugin.AllowCapability` entry. Unknown capability names are rejected.
- ABI/descriptor validation and catalog policies run before registry admission.
  `validate_core_contract` runs later, when `PluginLoader::create` creates a
  core; catalog admission alone does not validate runtime frame/state behavior.
- Android, Web, and other sandboxed/non-native targets do not load native
  plug-ins.

The allowlist is an identity and configuration policy. It is not a
cryptographic signature and must not be treated as proof that a library is
safe. Users should only allow libraries obtained from a trusted source.
Capability policy restricts advertised adapter capabilities; it does not sandbox
native code or replace adapter-level support checks.
Host integrations can also provide a synchronous trust callback. It runs after
ABI, identity, and capability validation but before registry admission, and a
false decision fails closed. This is the enforcement boundary for a future
interactive prompt; it is not itself a signature verifier.
The library is already loaded and its query entry point has run before these
catalog policies or the trust callback execute. Module initialization can run
arbitrary native code even for a library subsequently rejected by policy.
`PluginLoader::load` itself does not enforce catalog path or allowlist rules.
The shared `plugin_sha256_file` helper can be used by that callback to pin
approved binary digests. Digest pinning detects replacement or tampering but
does not establish publisher identity, so it cannot replace a signed manifest.

## UX requirements

The native Windows dashboard Settings page exposes whether discovery is enabled, whether the
identity and capability allowlists are required, how many plug-ins loaded, and
the latest rejection reason. Changes are persisted to `settings.ini` and take
effect after restart so an active core cannot be replaced underneath an
emulation session. Detailed path, capability, and ABI diagnostics remain
available through the shared logger.

Other native desktop builds use the same persisted configuration/loading path,
but do not have these Windows controls. Non-empty identity or capability lists
also restrict admission when their corresponding `Require*` flag is false.
Unknown capability names emit rejection diagnostics and grant no bits; they do
not necessarily abort discovery of otherwise permitted libraries.

## Future gate before general-user enablement

1. Implement the signed manifest and trust-store contract in
   [`plugin-manifest.md`](plugin-manifest.md), using a vetted cross-platform
   Ed25519 backend.
2. Add a user-visible trust prompt for first use and key changes.
3. Add an interactive first-use capability prompt layered on top of the
   settings allowlist.
4. Evaluate out-of-process hosting for untrusted or third-party cores.
5. Add CI coverage for signed, revoked, and tampered plugin packages.
