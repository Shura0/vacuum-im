/****************************************************************************

Copyright (c) 2006 - 2011, the LibQxt project. See the Qxt AUTHORS file.
Copyright (c) 2026, the Vacuum-IM project (Wayland/KGlobalAccel additions).
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the LibQxt project nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

<http://libqxt.org>  <foundation@libqxt.org>

*****************************************************************************/

#include "qxtglobalshortcut_wayland.h"
#include "qxtglobalshortcut_p.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QHash>
#include <QPair>
#include <QVariantMap>
#include <QtDebug>

//
// Client side of the org.kde.kglobalaccel protocol (Plasma 6 / kglobalacceld
// inside KWin). It repeats what the KGlobalAccel framework does on top of the
// D-Bus service, without pulling the framework dependency in.
//


// QKeySequence has no default D-Bus marshalling, these operators define the
// wire format "(ai)" the same way the KGlobalAccel framework serialises it
// (kglobalshortcutinfo_dbus.cpp). They must live in the global namespace for
// the argument dependent lookup - the dependent calls inside the
// qDBusRegisterMetaType() lambdas can not find anonymous namespace overloads.
QDBusArgument &operator<<(QDBusArgument &AArgument, const QKeySequence &ASequence)
{
	AArgument.beginStructure();
	AArgument.beginArray(qMetaTypeId<int>());
	for (int i = 0; i < 4; i++)
		AArgument << (i < ASequence.count() ? ASequence[i].toCombined() : 0);
	AArgument.endArray();
	AArgument.endStructure();
	return AArgument;
}

const QDBusArgument &operator>>(const QDBusArgument &AArgument, QKeySequence &ASequence)
{
	AArgument.beginStructure();
	AArgument.beginArray();

	int keys[4] = {0, 0, 0, 0};
	int count = 0;
	while (!AArgument.atEnd() && count < 4)
		AArgument >> keys[count++];
	AArgument.endArray();
	AArgument.endStructure();

	ASequence = QKeySequence(keys[0], keys[1], keys[2], keys[3]);
	return AArgument;
}

namespace {

const QString kComponentUnique = QStringLiteral("org.vacuum-im");
const QString kComponentFriendly = QStringLiteral("Vacuum-IM");

const QString kService = QStringLiteral("org.kde.kglobalaccel");
const QString kMainPath = QStringLiteral("/kglobalaccel");
const QString kMainIface = QStringLiteral("org.kde.KGlobalAccel");
const QString kComponentIface = QStringLiteral("org.kde.kglobalaccel.Component");

// org.kde.KGlobalAccel.setShortcutKeys() flags, they must match the values
// used by the KGlobalAccel framework (see its kglobalaccel_p.h).
enum SetShortcutFlag
{
	SetPresent = 2,             // the shortcut is active right now
	NoAutoloading = 4,          // do not restore the saved assignment
	IsDefault = 8               // the keys are the default assignment
};

// Registry of the shortcuts already registered with the daemon:
// (nativeKey, nativeMods) <-> D-Bus action unique name.
QHash<QPair<quint32,quint32>, QString> s_keyToAction;
QHash<QString, QPair<quint32,quint32> > s_actionToKey;

bool s_metaTypesInited = false;
QString s_componentPath;
bool s_receiverConnected = false;

QDBusMessage createCall(const QString &AMethod)
{
	return QDBusMessage::createMethodCall(kService, kMainPath, kMainIface, AMethod);
}

QKeySequence keySequence(quint32 ANativeKey, quint32 ANativeMods)
{
	return QKeySequence(Qt::Key(ANativeKey) | Qt::KeyboardModifiers(ANativeMods));
}

QString actionUnique(quint32 ANativeKey, quint32 ANativeMods)
{
	QString name = QStringLiteral("shortcut_") + keySequence(ANativeKey, ANativeMods).toString(QKeySequence::PortableText);
	QString compact;
	compact.reserve(name.size());
	foreach(const QChar &ch, name)
	{
		if (ch.isLetterOrNumber())
			compact.append(ch.toLower());
	}
	return compact;
}

QStringList actionId(const QString &AActionUnique, const QString &AActionFriendly)
{
	QStringList actionId;
	actionId.append(kComponentUnique);      // [0] Component Unique Name
	actionId.append(AActionUnique);         // [1] Action Unique Name
	actionId.append(kComponentFriendly);    // [2] Component Friendly Name
	actionId.append(AActionFriendly);       // [3] Action Friendly Name
	return actionId;
}

void initMetaTypes()
{
	if (s_metaTypesInited)
		return;
	qDBusRegisterMetaType<QKeySequence>();
	qDBusRegisterMetaType<QList<QKeySequence> >();
	s_metaTypesInited = true;
}

/**
 * The daemon replies with QSet<QKeySequence> marshalled to the "a(ai)" array,
 * unpack it in place, the generic QVariant::value<T>() does not have access
 * to the custom demarshalling (the KDE clients use the generated proxy
 * classes which avoid this).
 */
QList<QKeySequence> demarshallSequences(const QDBusMessage &AMessage)
{
	QList<QKeySequence> result;
	const QVariantList arguments = AMessage.arguments();
	if (arguments.isEmpty())
		return result;
	QDBusArgument argument = arguments.first().value<QDBusArgument>();
	argument.beginArray();
	while (!argument.atEnd())
	{
		QKeySequence sequence;
		argument >> sequence;
		result.append(sequence);
	}
	argument.endArray();
	return result;
}

/**
 * Announce the action to the daemon and grab the keys. Returns the message
 * of the keys assignment which carries the assigned keys for the caller to
 * inspect.
 */
QDBusMessage registerWithDaemon(quint32 ANativeKey, quint32 ANativeMods, const QString &AActionUnique)
{
	const QKeySequence sequence = keySequence(ANativeKey, ANativeMods);
	const QString friendly = QStringLiteral("Global shortcut %1").arg(sequence.toString(QKeySequence::NativeText));

	QDBusConnection bus = QDBusConnection::sessionBus();

	// Tell the daemon about the action before assigning keys to it.
	QDBusMessage registerReply = bus.call(createCall(QStringLiteral("doRegister")) << actionId(AActionUnique, friendly));
	if (registerReply.type() != QDBusMessage::ReplyMessage)
		qWarning() << "QxtGlobalShortcut: kglobalaccel refused the action registration:" << registerReply.errorMessage();

	// Assign the keys and also register the same keys as the default
	// assignment, so the "Reset to defaults" button in the global shortcuts
	// settings brings this key back.
	QVariantList arguments;
	arguments.append(QVariant(actionId(AActionUnique, friendly)));
	arguments.append(QVariant::fromValue(QList<QKeySequence>() << sequence));
	arguments.append(QVariant(uint(SetPresent)));
	QDBusMessage assign = createCall(QStringLiteral("setShortcutKeys"));
	assign.setArguments(arguments);
	QDBusMessage assignReply = bus.call(assign);

	arguments.clear();
	arguments.append(QVariant(actionId(AActionUnique, friendly)));
	arguments.append(QVariant::fromValue(QList<QKeySequence>() << sequence));
	arguments.append(QVariant(uint(IsDefault)));
	QDBusMessage defaults = createCall(QStringLiteral("setShortcutKeys"));
	defaults.setArguments(arguments);
	bus.asyncCall(defaults); // may legally fail on old daemon versions

	return assignReply;
}

bool connectReceiver()
{
	if (s_receiverConnected && !s_componentPath.isEmpty())
		return true;

	// Watch the daemon, its restart (e.g. the KWin restart) invalidates the
	// component object path and all the grabbed keys.
	QDBusServiceWatcher *watcher = new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
		QDBusServiceWatcher::WatchForOwnerChange, QxtWaylandShortcut::activator());
	QObject::connect(watcher, SIGNAL(serviceOwnerChanged(QString,QString,QString)),
		QxtWaylandShortcut::activator(), SLOT(onDaemonOwnerChanged(QString,QString,QString)));

	QDBusConnection bus = QDBusConnection::sessionBus();
	QDBusMessage reply = bus.call(createCall(QStringLiteral("getComponent")) << kComponentUnique);
	if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
	{
		qWarning() << "QxtGlobalShortcut: failed to fetch the kglobalaccel component path:" << reply.errorMessage();
		return false;
	}

	const QDBusObjectPath componentPath = reply.arguments().first().value<QDBusObjectPath>();
	if (componentPath.path().isEmpty())
	{
		qWarning() << "QxtGlobalShortcut: kglobalaccel returned an empty component path";
		return false;
	}
	s_componentPath = componentPath.path();

	// The deprecated globalShortcutPressed signal (component, action,
	// timestamp) is listened on purpose: the current daemon versions send it
	// together with the newer "pressed" signal, listening to both would
	// trigger the action twice per key press.
	bus.connect(kService, s_componentPath, kComponentIface, "globalShortcutPressed", "ssx",
		QxtWaylandShortcut::activator(), SLOT(onGlobalShortcutPressed(QString,QString,qlonglong)));

	s_receiverConnected = true;
	return true;
}

/**
 * Re-register everything with a fresh (restarted) daemon.
 */
void registerAllActions()
{
	initMetaTypes();
	for (QHash<QPair<quint32,quint32>, QString>::ConstIterator it = s_keyToAction.constBegin(); it != s_keyToAction.constEnd(); ++it)
		registerWithDaemon(it.key().first, it.key().second, it.value());
}

} // namespace

namespace QxtWaylandShortcut {

bool isActive()
{
	return qGuiApp != NULL && qGuiApp->platformName() == QLatin1String("wayland");
}

quint32 nativeKeycode(Qt::Key key)
{
	// The daemon works with Qt key sequences, the Qt key itself is the
	// identity, the (key, modifiers) pair is unique by the QKeySequence
	// design, so there is no need for the native keycode tables here.
	return quint32(key);
}

quint32 nativeModifiers(Qt::KeyboardModifiers modifiers)
{
	return quint32(modifiers.toInt());
}

bool registerShortcut(quint32 ANativeKey, quint32 ANativeMods)
{
	if (ANativeKey == 0)
		return false;

	initMetaTypes();

	const QPair<quint32,quint32> key(ANativeKey, ANativeMods);
	if (s_keyToAction.contains(key))
		return true;

	const QKeySequence sequence = keySequence(ANativeKey, ANativeMods);
	const QString unique = actionUnique(ANativeKey, ANativeMods);

	const QDBusMessage assignReply = registerWithDaemon(ANativeKey, ANativeMods, unique);
	if (assignReply.type() != QDBusMessage::ReplyMessage)
	{
		qWarning() << "QxtGlobalShortcut: failed to set global shortcut" << sequence.toString(QKeySequence::NativeText)
			<< assignReply.errorMessage();
		return false;
	}

	// Respect what the daemon really accepted - on conflicts or autoloaded
	// settings it can return another combination.
	const QList<QKeySequence> assigned = demarshallSequences(assignReply);
	if (assigned.isEmpty())
	{
		qWarning() << "QxtGlobalShortcut: kglobalaccel did not accept" << sequence.toString(QKeySequence::NativeText);
		return false;
	}
	if (assigned.first() != sequence)
	{
		qWarning() << "QxtGlobalShortcut: kglobalaccel assigned" << assigned.first().toString(QKeySequence::NativeText)
			<< "instead of" << sequence.toString(QKeySequence::NativeText);
	}

	// Subscribe to the activation signals after the successful assignment
	// but before the local cache is updated, so a failed subscription leaves
	// no lost registration behind.
	if (!connectReceiver())
	{
		QDBusMessage revert = createCall(QStringLiteral("unregister"));
		revert << kComponentUnique << unique;
		revert.setAutoStartService(false);
		QDBusConnection::sessionBus().asyncCall(revert);
		return false;
	}

	s_keyToAction.insert(key, unique);
	s_actionToKey.insert(unique, key);
	return true;
}

bool unregisterShortcut(quint32 ANativeKey, quint32 ANativeMods)
{
	const QPair<quint32,quint32> key(ANativeKey, ANativeMods);
	const QString unique = s_keyToAction.take(key);
	if (unique.isEmpty())
		return true;
	s_actionToKey.remove(unique);

	// The action was announced as present, drop it completely.
	QDBusMessage unreg = createCall(QStringLiteral("unregister"));
	unreg << kComponentUnique << unique;
	unreg.setAutoStartService(false);
	QDBusConnection::sessionBus().asyncCall(unreg);
	return true;
}

void activateAction(const QString &AActionUnique)
{
	QPair<quint32,quint32> key = s_actionToKey.value(AActionUnique, qMakePair(quint32(0), quint32(0)));
	if (key.first != 0)
		QxtGlobalShortcutPrivate::activateShortcut(key.first, key.second);
}

void daemonOwnerChanged(const QString &ANewOwner)
{
	if (ANewOwner.isEmpty())
		return; // the daemon is gone, wait until somebody starts it again

	qDebug() << "kglobalaccel restarted, re-registering global shortcuts";
	QDBusConnection::sessionBus().disconnect(kService, s_componentPath, kComponentIface, "globalShortcutPressed",
		QxtWaylandShortcut::activator(), SLOT(onGlobalShortcutPressed(QString,QString,qlonglong)));
	s_componentPath.clear();
	s_receiverConnected = false;

	registerAllActions();
	connectReceiver();
}

QObject *activator()
{
	static QxtWaylandShortcutReceiver *receiver = new QxtWaylandShortcutReceiver;
	return receiver;
}

} // namespace QxtWaylandShortcut
