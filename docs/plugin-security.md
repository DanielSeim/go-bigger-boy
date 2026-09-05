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
- ABI, descriptor, capability, metadata, and core-contract validation occurs
  before a plug-in is registered.
- Android, Web, and other sandboxed/non-native targets do not load native
  plug-ins.

The allowlist is an identity and configuration policy. It is not a
cryptographic signature and must not be treated as proof that a library is
safe. Users should only allow libraries obtained from a trusted source.

## UX requirements

The desktop Settings page exposes whether discovery is enabled, whether the
identity allowlist is required, how many plug-ins loaded, and the latest
rejection reason. Changes are persisted to `settings.ini` and take effect after
restart so an active core cannot be replaced underneath an emulation session.
Detailed path and ABI diagnostics remain available through the shared logger.

## Future gate before general-user enablement

1. Define a signed manifest format and verification key distribution process.
2. Add a user-visible trust prompt for first use and key changes.
3. Add capability-specific permission prompts for link, camera, printer,
   debugger, and scene-layer services.
4. Evaluate out-of-process hosting for untrusted or third-party cores.
5. Add CI coverage for signed, revoked, and tampered plugin packages.
