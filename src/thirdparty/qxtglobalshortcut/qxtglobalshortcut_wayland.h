#ifndef QXTGLOBALSHORTCUT_WAYLAND_H
#define QXTGLOBALSHORTCUT_WAYLAND_H

#include <QObject>
#include <QtGlobal>
#include <Qt>

// KGlobalAccel-based global shortcut backend for the Wayland QPA platform.
//
// X11-style XGrabKey() grabs are impossible for a native Wayland client, so
// under Wayland the keys are registered with the org.kde.kglobalaccel service
// (kglobalacceld, which runs inside KWin on Plasma). The daemon grabs the keys
// and delivers activation events back to us through D-Bus signals. A nice side
// effect is that the registered shortcuts appear in Plasma's own
// "Global Shortcuts" settings module, where the user can also manage them.
//
// A shortcut is identified by the full Qt combination (Key | KeyboardModifiers),
// the pair is passed here as (nativeKey, nativeMods) to keep the common
// QxtGlobalShortcutPrivate bookkeeping unchanged.
//
// The protocol mirrors the KGlobalAccel framework client (src/kglobalaccel.cpp):
// 1. doRegister(actionId) - announce component/action to the daemon;
// 2. setShortcutKeys(actionId, keys, SetPresent) - grab the keys;
// 3. listen org.kde.kglobalaccel.Component::globalShortcutPressed on the
//    component object path for the activation events.
namespace QxtWaylandShortcut {
	// True when the current QGuiApplication runs on the wayland platform.
	bool isActive();

	quint32 nativeKeycode(Qt::Key key);
	quint32 nativeModifiers(Qt::KeyboardModifiers modifiers);

	bool registerShortcut(quint32 nativeKey, quint32 nativeMods);
	bool unregisterShortcut(quint32 nativeKey, quint32 nativeMods);

	// Indirect entry points for the receiver slots, they are defined in the
	// implementation file and need the internal state from there.
	void activateAction(const QString &AActionUnique);
	void daemonOwnerChanged(const QString &ANewOwner);

	// The long-lived D-Bus signal receiver singleton.
	QObject *activator();

/**
 * Receives D-Bus activation events from kglobalaccel. The methods only
 * forward to the QxtWaylandShortcut namespace functions which live in the
 * implementation file. The class lives here because the project mocs
 * headers, not sources; keep the external linkage so the moc-generated
 * QxtWaylandShortcutReceiver::staticMetaObject has exactly one definition.
 */
class QxtWaylandShortcutReceiver : public QObject
{
	Q_OBJECT
public:
	explicit QxtWaylandShortcutReceiver(QObject *AParent = nullptr) : QObject(AParent) {}

public slots:
	void onGlobalShortcutPressed(const QString &AComponentUnique, const QString &AActionUnique, qlonglong ATimestamp)
	{
		Q_UNUSED(AComponentUnique);
		Q_UNUSED(ATimestamp);
		activateAction(AActionUnique);
	}

	void onDaemonOwnerChanged(const QString &AService, const QString &AOldOwner, const QString &ANewOwner)
	{
		Q_UNUSED(AService);
		Q_UNUSED(AOldOwner);
		daemonOwnerChanged(ANewOwner);
	}
};

} // namespace QxtWaylandShortcut

#endif // QXTGLOBALSHORTCUT_WAYLAND_H
