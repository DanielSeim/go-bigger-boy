# Cloud-save synchronization

Go Bigger Boy already keeps battery saves, RTC data, quick states, and manual
backups on each device. Cloud synchronization is a natural next layer, but it
must remain optional and offline-first. The first implementation step is the
provider-neutral manifest contract in `include/gbb/save_sync.hpp`; it does not
perform network I/O and it never uploads ROM contents.

## Scope and identity

Each save payload is stored separately from a manifest entry. An entry is
identified by:

```text
system_id + rom_fingerprint + artifact_kind + slot
```

The current artifact kinds are `battery-save`, `rtc`, and `save-state`.
Battery saves and RTC data always use slot zero; save states have numbered
slots. `rom_fingerprint` is the exact ROM identity already used by the core,
so localized or modified ROMs do not accidentally share save data.

The manifest uses the versioned `gbb.save-sync.v1` JSON schema. Each entry
records its byte size, a stable content hash, revision lineage, update time,
origin device ID, core ID, and hardware model. 64-bit fingerprints, hashes, and
revision values are encoded as hexadecimal JSON strings so web clients cannot
lose precision. The manifest device ID identifies the writer; entries may have
originated on another device after a cloud merge. Save-state metadata is
intentionally more specific because a state is coupled to the emulator format
and hardware configuration. The payload is still validated by the existing
save-state container when it is loaded.

The content hash is for change detection, not authentication. A future
provider must use authenticated transport and should support optional
client-side encryption. ROMs, cover artwork downloads, and emulator logs are
not part of cloud saves.

## Conflict policy

Identical content is silently deduplicated. A newer revision replaces an older
one only when it names that exact record as its parent. Two devices that edit
the same revision independently produce a conflict; the client must never
silently discard either copy. The UI should offer the local copy, the cloud
copy, and a backup of the losing copy before resolving.

Battery saves cannot generally be merged. RTC data also needs special care:
restoring an older clock can make time-sensitive games appear to move backward.
The safe default is to show the conflict and let the user choose. Save states
should additionally be rejected when their ROM fingerprint, core, format, or
hardware model is incompatible.

## Recommended rollout

1. Add a provider interface that uploads/downloads payloads and manifests but
   does not know about emulator internals.
2. Implement synchronization for battery saves and RTC data first. Queue
   retries when offline and sync after a successful flush or on clean exit.
3. Add Android background-work constraints, desktop retry behavior, and a
   browser IndexedDB fallback before adding account UI.
4. Add one configurable backend (for example WebDAV or a small HTTPS service)
   rather than coupling the emulator to one commercial provider.
5. Add save-state synchronization only after format/version compatibility and
   conflict recovery are covered by tests.

The existing ZIP backup/import flow remains the manual recovery path and must
continue to work when a user has no account or network connection.
