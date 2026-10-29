# Vacuum-IM — Project Reference

Quick orientation for fast investigations. Keep this file updated when the
build system or architecture changes.

## What it is

Vacuum-IM is a full-featured, cross-platform **Jabber/XMPP instant-messaging
client** written in C++/Qt. Version in tree: `1.3.0 Alpha` (see
`src/definitions/version.h`). Licensed GPLv3. Upstream:
<https://github.com/Vacuum-IM>.

The application is a **thin plugin loader**: `src/loader/main.cpp` boots a
`QApplication`, dynamically loads `libvacuumutils`, and hands control to
`PluginManager`, which discovers and wires up plugins. Nearly all user-facing
functionality lives in plugins that communicate through abstract interfaces.

## Repo layout

```
CMakeLists.txt          Top-level CMake project (Qt6, C++17)
README / INSTALL        Feature list; build requirements & commands
CHANGELOG               Versioned feature/fix log
GIGACODE.md             Legacy one-line build note (stale, qmake)
src/
  loader/               Executable "vacuum": plugin manager, about box, main()
  utils/                libvacuumutils — shared UI/util library (Jid, Message,
                        Stanza, IconStorage, WidgetManager, options, etc.)
  interfaces/           60+ pure-virtual feature interfaces (i*.h) + Q_DECLARE_INTERFACE
  definitions/          Shared constants/enums/order values, version.h, namespaces.h
  plugins/              ~75 feature plugins (see plugin_list.cmake / plugins.cmake)
  thirdparty/           Bundled zlib, minizip, idn, hunspell, idle,
                        qtlockedfile, qxtglobalshortcut
  make/                  config.cmake, translations.cmake (build helpers)
  translations/          languages.cmake + translation plumbing
  packages/              Platform packaging (windows/, macosx/)
  tools/
resources/              Icons, sounds, message styles, data resources
build/                  Local CMake build tree (git-ignored; contains scratch logs)
```

## Architecture

- **Plugin contract**: each plugin class multiply-inherits `QObject`, `IPlugin`
  (`src/interfaces/ipluginmanager.h`) and one or more feature interfaces
  (`IMessageStyleHandler`, `IRosterManager`, ...). It uses `Q_INTERFACES(...)`
  and `Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.<Name>")`.
- **Lifecycle** (`IPlugin`): `initConnections(pluginManager, &order)` →
  `initObjects()` → `initSettings()` → `startPlugin()`. Initialization order is
  controlled by `*orders.h` constants in `src/definitions/`.
- **Interfaces are versioned UUIDs**; plugins declare `implements`,
  `dependences`, and `conflicts`, and exchange services via
  `IPluginManager::pluginInterface(...)` / `pluginInstance(uuid)`.
- **Essentials vs optional**: `plugins.cmake` groups plugins by
  `<name>_IS_ESSENTIAL`; the README lists plugins that must not be disabled
  (accountmanager, connectionmanager, mainwindow, roster*, stanza processor,
  xmppstreams, etc.).
- **Feature shape**: most features are split across several cooperating
  plugins plus a manager interface, e.g.
  - roster: `rostersmodel` (model) / `rostersview` (view) / `roster` (logic) /
    `rosterchanger` (editing)
  - messages: `stanzaprocessor` → `messageprocessor` → `messagestyles` →
    `messagewidgets` → handlers (`normalmessagehandler`, `chatmessagehandler`)
  - multi-user chat: `multiuserchat` + widgets
- **Network stack**: `xmppstreams` → `connectionmanager` → `defaultconnection`
  → `socksstreams` → `starttls` / `saslauth` / `sessionnegotiation` /
  `compress`; stream features are pluggable.

## Build

Only **CMake** is supported (qmake `.pro`/`.pri` files were removed).

Requirements: Qt 6.8+ (Core Gui Widgets Network Xml Sql Svg PrintSupport
Multimedia DBus LinguistTools), OpenSSL 1.1+, CMake 3.16+, C++17.
Linux extras: libidn, libXScrnSaver, zlib/minizip, hunspell|enchant|aspell
(bundled copies exist in `src/thirdparty`).

```sh
# Configure + build (Ninja)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build            # default prefix /usr/local
```

Notable options:
- `PLUGIN_<name>=ON|OFF` — enable/disable a plugin (e.g.
  `-DPLUGIN_adiummessagestyle=OFF`, the default; not yet ported to Qt 6).
- `SPELLCHECKER_BACKEND=HUNSPELL|ENCHANT|ASPELL|NONE`
- `INSTALL_SDK`, `INSTALL_DOCS`, `LANGS`, `FORCE_BUNDLED_MINIZIP`

CI: `.github/workflows/build.yml` builds on ubuntu-24.04 with Qt 6.8 + Ninja.

Outputs land in `build/plugins/*.so` and `build/` (loader). Plugins install to
`<prefix>/lib/vacuum/plugins` (platform-dependent; see `src/make/config.cmake`).

## Branch / history notes

- Global shortcuts are QPA-aware since the Qt6 Wayland fix: the bundled
  `src/thirdparty/qxtglobalshortcut` dispatches at runtime - classic
  `XGrabKey` grabs for the xcb platform, and the `org.kde.kglobalaccel`
  D-Bus protocol (kglobalacceld inside KWin) for the wayland platform
  (`qxtglobalshortcut_wayland.{h,cpp}`, linked with `Qt6::DBus`). On Wayland
  the activation arrives via the component's `globalShortcutPressed` signal
  (intentionally the deprecated one - the daemon also emits the newer
  `pressed` signal, using both would double-trigger). A `QDBusArgument`
  marshalling/demarshalling pair for `QKeySequence` (wire format `(ai)`) is
  implemented in the backend and must be global-scope (ADL), not hidden in an
  anonymous namespace.

- Current branch: **`qt6`** — the in-progress Qt 5 → Qt 6 migration, 9 commits
  ahead of / 2 behind `master`. Contains `src/utils/qtcompat.h` and per-file
  Qt6 signal/slot renames. CI config references
  `/home/shura/.opencode/plan/qt6-migration.md`.
- Other local branches: `master`, `imageviewer-plugin`, `fixed-crash`,
  `fixed-crush`. Remote `origin/dev_*` branches host feature work.
- `build/` holds many migration scratch artifacts (`qt6-*.log`, `install-qt6*`,
  `err*.txt`) — useful history, not source.

## Conventions & gotchas

- Tabs for indentation in C++; Qt naming (`A`-prefixed args, `F`-prefixed
  members). Source is a mix of older style, so match the file you touch.
- Plugin sources are listed explicitly in each plugin's `.cmake`
  (`<name>.cmake`), not globbed — add new files there.
- New plugin scaffolding: `src/plugins/newplugin.sh` (+ `.bat`), registered in
  `src/plugins/plugin_list.cmake`.
- Build dir already contains stale `moc_predefs.h` copies checked into source
  dirs in a few places; treat `build/` as disposable.
- `.vacuum` symlink points to `~/.vacuum` (runtime profile/home dir).
- `src/libs/` holds prebuilt static/shared libs, not source.

## Fast entry points per task

| Task                       | Start here |
|----------------------------|------------|
| Startup / plugin loading   | `src/loader/main.cpp`, `src/loader/pluginmanager.cpp` |
| Add/change a feature       | matching `src/plugins/<name>/` + `src/interfaces/i<name>.h` |
| Roster behavior            | `src/plugins/roster*`, `irostermanager.h`, `irostersmodel.h` |
| Message pipeline           | `src/plugins/message*`, `imessageprocessor.h`, `imessagewidgets.h` |
| XMPP stream / connection   | `src/plugins/xmppstreams`, `connectionmanager`, `starttls`, `saslauth` |
| Shared utilities           | `src/utils/` (Jid, Stanza, Message, IconStorage, options) |
| Build/packaging            | top `CMakeLists.txt`, `src/make/`, `src/packages/` |

## Other
Do not try to install program. Ask user.
