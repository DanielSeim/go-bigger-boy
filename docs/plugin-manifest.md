# Signed native plug-in manifest

This document defines the package metadata that will sit above the frozen
plug-in ABI. It is a design contract, not an implementation claim: loading
remains opt-in and unsigned libraries remain subject to the current identity,
capability, and host trust policies.

## Canonical payload

The signed payload is UTF-8 text with LF line endings and no trailing blank
line. Fields are sorted lexicographically by name and encoded as one
`name=value` record per line:

```text
format=1
core_id=example-core
abi_major=1
abi_minor=0
sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
capabilities=persistent_memory,rtc
platforms=linux-x86_64,windows-x86_64
publisher_key_id=example-release-2026
```

`sha256` is the lowercase digest of the exact native library bytes. Capability
and platform lists are sorted, comma-separated identifiers from the host's
stable vocabulary. Unknown fields, duplicate fields, malformed UTF-8, invalid
hex digests, and unsorted lists are rejected before signature verification.

The detached signature is stored separately as a base64-encoded Ed25519
signature over the exact canonical payload bytes. The manifest file contains
the payload followed by one final record:

```text
signature=base64-ed25519-signature
```

The signature record is never included in the signed payload.

## Trust and rotation

- A release trust store maps `publisher_key_id` to an Ed25519 public key.
- Key IDs are stable and are not inferred from filenames or core IDs.
- A key can be revoked with an explicit revocation record and optional
  effective timestamp. Revocation is checked before accepting a manifest.
- Rotation publishes a new key before signing with it; old keys remain valid
  until their stated retirement or revocation time.
- Missing trust-store entries, revoked keys, expired manifests, signature
  failures, and digest mismatches all fail closed.

The trust store must be delivered through the signed application release or a
separately authenticated update channel. A plug-in must never be allowed to
extend its own trust store.

## Verification order

1. Resolve and canonicalize the library and manifest paths; reject symlinks.
2. Parse and canonicalize the payload; reject duplicate or unknown fields.
3. Load the release trust store and reject unknown or revoked key IDs.
4. Verify the Ed25519 signature over the canonical payload bytes.
5. Hash the library with SHA-256 and compare it to `sha256`.
6. Load the library and require ABI, descriptor identity, capability, and
   adapter validation to match the manifest.
7. Apply the host's configured allowlists and trust callback before registry
   admission.

No step may be skipped when signed-manifest mode is enabled. The manifest
format intentionally does not change the v1.0 C ABI; it is a host-side package
trust layer.

## Implementation gate

Implementation requires a maintained, cross-platform Ed25519 backend available
to Linux, Windows, macOS, Android, and Web build configurations (or a small,
audited vendored implementation) plus CI fixtures for valid, revoked, expired,
tampered, wrong-key, and digest-mismatch packages. Until that dependency and
fixture matrix are accepted, the runtime must not advertise signed-manifest
support and must continue using the explicit opt-in policy documented in
[`plugin-security.md`](plugin-security.md).
