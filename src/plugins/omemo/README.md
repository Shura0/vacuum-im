# OMEMO plugin

Legacy OMEMO 0.3 (XEP-0384 v0.3, `eu.siacs.conversations.axolotl`) for
one-to-one chats, including encrypted files (XEP-0454) both received and sent
through XEP-0363 HTTP Upload.  OMEMO 2, group chats and calls are not
implemented yet.

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
- **Strict trust** can be enabled globally (the per-account OMEMO settings
  page) or per contact (the “Strict trust” checkbox in the device dialog).
  In a strict chat an incoming message is accepted only from a device marked
  trusted; messages from unverified or blocked devices are not decrypted, the
  session is left untouched, and the body is replaced with
  `[OMEMO message from an untrusted device]` with an amber open-lock mark.
  BTBV/TOFU behaviour is unchanged for non-strict chats.  Note that a skipped
  pre-key message may stay undecryptable later, because its one-time pre-key
  is not consumed.
- OMEMO is always enabled; there is no global or per-account on/off switch.
  Each account has an **OMEMO** page in Settings → Accounts that shows the
  device name stored with the encrypted identity, the global trust policy and
  a **Clear all OMEMO sessions** action.  Clearing sessions drops the Signal
  ratchets and rebuilds them from the devices' published bundles; identity
  keys, trust decisions and readable history are preserved.
- Clicking the lock indicator opens a menu with the per-chat switch
  “Use OMEMO in this chat”.  The switch is stored in the encrypted OMEMO
  store and survives a restart; when it is off, outgoing messages are sent
  unencrypted and queued messages are released as plaintext.
- Every chat message has a small mark in the right-aligned marks slot,
  alongside delivery/read receipts and the edited pencil: green closed lock
  = OMEMO encrypted, amber hourglass = waiting for keys, red warning =
  error, amber open lock = strict trust rejected the sender's device,
  red open lock = contact supports OMEMO but this message is
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
- **Receiving encrypted files and images** (XEP-0454): a message body of the
  form `aesgcm://<https-url>#<iv+key>` — optionally followed by a data: URI
  thumbnail — is recognized wherever OMEMO plaintexts appear.  Before the
  message is stored or displayed the body is sanitized: the url becomes a
  plain `https://` link without the key fragment and the thumbnail line is
  removed, so the AES-256-GCM key never reaches the plaintext archive.  The
  key material is kept in the encrypted OMEMO store and the decrypted file
  is cached under `<filesPath>/omemo/files/<account-uuid>/`.  In the chat
  window the attachment shows as an ordinary link until the file arrives
  (shortly after receiving the message), then it is replaced by an
  **inline image** (only when the Image Viewer plugin does not take care of
  the previews, see below) and the original link stays clickable.  Downloads follow the same
  safety rules as the Image Viewer plugin: size limit, redirect and
  stall/time limits, and blocking of requests to loopback/private/reserved
  addresses (SSRF protection of the same strength as image previews).
  Non-image files are stored as `<sha256>.bin`; clicking the attachment
  link (or the inline image) opens the **decrypted local file** with the
  system opener for every file type; the still-encrypted remote file is
  never passed to the browser.  If the file is not decrypted yet, the click
  starts the download and the click can be repeated afterwards.  Non-image
  attachments are additionally displayed under their own file name with
  the size (`document.pdf (2.3 MB)`), when cached or replacing the link
  text when the download completes.  In
  the message-area context menu of a chat window two extra items appear for
  a registered attachment link: **Open** and **Save as...** (a pending
  "Save as..." remembers the chosen target and saves the file automatically
  when the download completes).  The History dialog currently shows only
  the link.

- **Integration with the Image Viewer**: when the Image Viewer plugin is
  enabled, the OMEMO plugin acts as its encrypted media provider
  (`IImageMediaProvider`).  The viewer consults it for every image link
  before downloading anything itself, so an encrypted attachment is
  fetched and decrypted exactly once and then shown in the ordinary image
  viewer galleries with downsized previews — exactly like a normal
  `https://` image.  While the viewer takes care of the previews the OMEMO
  plugin does not insert its own inline images.  If the viewer is disabled
  or its autoload option (`Automatically load images from message URLs`)
  is off, OMEMO images fall back to the inline behaviour described above.
  Non-image attachments are not affected by the integration.

## Storage

Per-account state is stored in
`<profile>/binary/omemo/<account-uuid>.dat`:

```
"VOMEMO1" | salt[16] | nonce[12] | tag[16] | AES-256-GCM(JSON)
```

Store format version 3 (version 2 added the XEP-0454 attachment keys and
thumbnails, version 3 the per-chat strict trust flag); older files are loaded
transparently with the new fields empty and are rewritten in the new format
on the next save.

The key is derived with HKDF-SHA256 from the profile key
(`Options::cryptKey()`), using the random per-file salt.  The file is written
atomically with `0600` permissions.  Keys are never written to
`options.xml`.

## Profile password

Changing the profile password does not affect OMEMO.  The profile key itself
is not regenerated: `OptionsManager::changeProfilePassword()` decrypts the
existing key with the old password and re-encrypts the **same** key bytes with
the new one, so `Options::cryptKey()` stays stable and the HKDF-derived OMEMO
store key is unchanged.  No migration of `<account-uuid>.dat` is needed; a
store written before the change remains readable afterwards.

If the profile has no encryption key at all (an old-format profile without a
password), OMEMO cannot start and reports “The profile encryption key is not
available”.  Set a profile password to enable encrypted storage.

## PEP nodes

- `eu.siacs.conversations.axolotl.devicelist` (item `current`, root `list`)
- `eu.siacs.conversations.axolotl.bundles:<device_id>` (item `current`)
- subscribes to the devicelist node and tries to switch published nodes to
  `access_model=open`.  Items are published directly through the stanza
  processor, so servers which support PEP but do not advertise a
  `pubsub/pep` disco identity still work.

## Known limitations

- Only legacy 0.3; OMEMO 2 peers are not supported.
- No MUC, calls. One-to-one files are encrypted and sent through the HTTP
  upload service (XEP-0363 + XEP-0454) when OMEMO is enabled for the chat and
  the contact actually has OMEMO devices; without an upload service the file
  cannot be sent through OMEMO.  If the contact has no OMEMO devices (so the
  message itself would be sent in plaintext) the file is uploaded unencrypted
  and sent as an ordinary `https://` link with an `x:oob` fallback, because the
  aesgcm key would otherwise never reach the recipient.
- Identity changes on an existing device are rejected by the Signal trust
  callback and need manual session cleanup (not exposed in the UI yet).
- The UI is limited to the per-contact device/trust dialog, the chat lock
  indicator, the per-chat switch and the per-account OMEMO settings page;
  managing own devices and reacting to identity changes are not exposed yet.

## Build

The plugin is enabled by default and can be disabled with
`-DPLUGIN_omemo=OFF`.  It vendors `libomemo-c` 0.5.1 and `protobuf-c` 1.5.1
under `src/thirdparty` and links OpenSSL only for the local encrypted store.
