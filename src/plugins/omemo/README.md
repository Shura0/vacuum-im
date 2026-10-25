# OMEMO plugin

Legacy OMEMO 0.3 (XEP-0384 v0.3, `eu.siacs.conversations.axolotl`) for
one-to-one chats.  OMEMO 2, group chats, files and calls are intentionally
not implemented yet.

## Behaviour

- OMEMO is used automatically for a chat as soon as the contact publishes a
  device list.  If the device list is not known yet, the outgoing message is
  queued while the PEP node is fetched.  A failed fetch is not treated as an
  empty list, so the message is never silently downgraded to plaintext.
- The message body is encrypted with a fresh AES-128-GCM key.  The 16-byte
  key and 16-byte GCM tag are sent through the Signal session of every
  trusted device of the contact and of the sender's own account.
- Incoming messages, carbons and MAM history are decrypted.  A readable
  fallback body is sent for clients that do not support OMEMO.
- The first device list of a contact is trusted (TOFU/BTBV).  Devices added
  later are **not used for sending** until the user trusts them.  Open the
  chat window context menu and choose “OMEMO devices...” to see fingerprints
  and trust/block devices.  Devices of the same account are trusted
  automatically.
- Clicking the lock indicator opens a menu with the per-chat switch
  “Use OMEMO in this chat”.  The switch is stored in the encrypted OMEMO
  store and survives a restart; when it is off, outgoing messages are sent
  unencrypted and queued messages are released as plaintext.
- Every chat message has a small mark in the right-aligned marks slot,
  alongside delivery/read receipts and the edited pencil: green closed lock
  = OMEMO encrypted, amber hourglass = waiting for keys, red warning =
  error, red open lock = contact supports OMEMO but this message is
  plaintext, gray open lock = OMEMO is not available for this contact.  U+FE0E is added to the emoji so
  the font uses the monochrome text presentation and the CSS color is
  visible.  If no other plugin has created a right tab stop for the line,
  the OMEMO handler adds one itself.
- A lock indicator in the chat window header shows the current state:
  green = encryption active with trusted devices, orange = unverified or no
  trusted devices, red = all devices blocked, gray = OMEMO unavailable.
  Hover for details; click it to open the device/trust dialog.
- Local archive history is decrypted on demand: if the stream is not
  connected yet, the account crypto/store is initialized when the first
  encrypted archived message is shown.  Closing the stream keeps the store
  loaded for offline history; reconnecting republishes the PEP state.
- Decrypted OMEMO plaintexts are cached (encrypted at rest) in the OMEMO
  store by stanza id.  This keeps chat history and the separate History
  dialog readable later even after the ratchet has advanced; the archive
  viewer also uses the same decryption hook.

## Storage

Per-account state is stored in
`<profile>/binary/omemo/<account-uuid>.dat`:

```
"VOMEMO1" | salt[16] | nonce[12] | tag[16] | AES-256-GCM(JSON)
```

The key is derived with HKDF-SHA256 from the profile key
(`Options::cryptKey()`), using the random per-file salt.  The file is written
atomically with `0600` permissions.  Keys are never written to
`options.xml`.

## PEP nodes

- `eu.siacs.conversations.axolotl.devicelist` (item `current`, root `list`)
- `eu.siacs.conversations.axolotl.bundles:<device_id>` (item `current`)
- subscribes to the devicelist node and tries to switch published nodes to
  `access_model=open`.  Items are published directly through the stanza
  processor, so servers which support PEP but do not advertise a
  `pubsub/pep` disco identity still work.

## Known limitations

- Only legacy 0.3; OMEMO 2 peers are not supported.
- No MUC, file transfer or calls.
- Identity changes on an existing device are rejected by the Signal trust
  callback and need manual session cleanup (not exposed in the UI yet).
- Profile password changes are not migrated yet; the OMEMO file is encrypted
  with the current profile key.
- The UI is limited to the per-contact device/trust dialog, the chat lock
  indicator and the per-chat OMEMO switch; there is no account-level switch
  yet.

## Build

The plugin is enabled by default and can be disabled with
`-DPLUGIN_omemo=OFF`.  It vendors `libomemo-c` 0.5.1 and `protobuf-c` 1.5.1
under `src/thirdparty` and links OpenSSL only for the local encrypted store.
