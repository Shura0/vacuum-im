#include "traymanager.h"

#include <QRandomGenerator>
#include <QApplication>
#include <QCoreApplication>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#endif
#include <definitions/resources.h>
#include <definitions/menuicons.h>
#include <definitions/actiongroups.h>
#include <utils/versionparser.h>
#include <utils/logger.h>

#define BLINK_VISIBLE_TIME      750
#define BLINK_INVISIBLE_TIME    250

TrayManager::TrayManager()
{
	FPluginManager = NULL;

	FActiveNotify = -1;

	QPixmap empty(16,16);
	empty.fill(Qt::transparent);
	FEmptyIcon.addPixmap(empty);

	FContextMenu = new Menu;
	FSystemIcon.setContextMenu(FContextMenu);

	FBlinkVisible = true;
	FBlinkTimer.setSingleShot(true);
	connect(&FBlinkTimer,SIGNAL(timeout()),SLOT(onBlinkTimerTimeout()));

	connect(&FSystemIcon,SIGNAL(messageClicked()), SIGNAL(messageClicked()));
	connect(&FSystemIcon,SIGNAL(activated(QSystemTrayIcon::ActivationReason)), SLOT(onTrayIconActivated(QSystemTrayIcon::ActivationReason)));

#ifdef Q_OS_LINUX
	QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.Notifications"),
		QStringLiteral("/org/freedesktop/Notifications"),
		QStringLiteral("org.freedesktop.Notifications"),
		QStringLiteral("ActionInvoked"),
		this,SLOT(onNativeNotificationActionInvoked(uint,QString)));
	QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.Notifications"),
		QStringLiteral("/org/freedesktop/Notifications"),
		QStringLiteral("org.freedesktop.Notifications"),
		QStringLiteral("NotificationClosed"),
		this,SLOT(onNativeNotificationClosed(uint,uint)));
#endif
}

TrayManager::~TrayManager()
{
	while (FNotifyOrder.count() > 0)
		removeNotify(FNotifyOrder.first());
	delete FContextMenu;
}

void TrayManager::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Tray Icon");
	APluginInfo->description = tr("Allows other modules to access the icon and context menu in the tray");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Potapov S.A. aka Lion";
	APluginInfo->homePage = "http://www.vacuum-im.org";
}

bool TrayManager::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	FPluginManager = APluginManager;

	return true;
}

bool TrayManager::initObjects()
{
	Action *action = new Action(FContextMenu);
	action->setIcon(RSR_STORAGE_MENUICONS,MNI_MAINWINDOW_QUIT);
	action->setText(tr("Quit"));
	connect(action,SIGNAL(triggered()),FPluginManager->instance(),SLOT(quit()));
	FContextMenu->addAction(action,AG_TMTM_TRAYMANAGER_QUIT);
	return true;
}

bool TrayManager::startPlugin()
{
	setTrayIconVisible(true);
	return true;
}

QRect TrayManager::geometry() const
{
	return FSystemIcon.geometry();
}

Menu *TrayManager::contextMenu() const
{
	return FContextMenu;
}

QIcon TrayManager::icon() const
{
	return FIcon;
}

void TrayManager::setIcon(const QIcon &AIcon)
{
	FIcon = AIcon;
	if (FActiveNotify <= 0)
		FSystemIcon.setIcon(AIcon);
	else
		updateTray();
}

QString TrayManager::toolTip() const
{
	return FToolTip;
}

void TrayManager::setToolTip(const QString &AToolTip)
{
	FToolTip = AToolTip;
	if (FActiveNotify <= 0)
		FSystemIcon.setToolTip(AToolTip);
	else
		updateTray();
}

bool TrayManager::isTrayIconVisible() const
{
	return FSystemIcon.isVisible();
}

void TrayManager::setTrayIconVisible(bool AVisible)
{
	LOG_INFO(QString("Tray icon visibitity changed to=%1").arg(AVisible));
	FSystemIcon.setVisible(AVisible);
}

int TrayManager::activeNotify() const
{
	return FActiveNotify;
}

QList<int> TrayManager::notifies() const
{
	return FNotifyOrder;
}

ITrayNotify TrayManager::notifyById(int ANotifyId) const
{
	return FNotifyItems.value(ANotifyId);
}

int TrayManager::appendNotify(const ITrayNotify &ANotify)
{
	int notifyId = QRandomGenerator::global()->generate();
	while (notifyId<=0 || FNotifyItems.contains(notifyId))
		notifyId = QRandomGenerator::global()->generate();

	FNotifyOrder.append(notifyId);
	FNotifyItems.insert(notifyId,ANotify);
	updateTray();

	LOG_DEBUG(QString("Tray notification inserted, id=%1, blink=%2").arg(notifyId).arg(ANotify.blink));
	emit notifyAppended(notifyId);

	return notifyId;
}

void TrayManager::removeNotify(int ANotifyId)
{
	if (FNotifyItems.contains(ANotifyId))
	{
		FNotifyItems.remove(ANotifyId);
		FNotifyOrder.removeAll(ANotifyId);
		updateTray();

		LOG_DEBUG(QString("Tray notification removed, id=%1").arg(ANotifyId));
		emit notifyRemoved(ANotifyId);
	}
}

bool TrayManager::isNativeNotificationsAvailable() const
{
#ifdef Q_OS_LINUX
	if (QDBusConnection::sessionBus().isConnected())
	{
		QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
		return bus!=NULL && bus->isServiceRegistered(QStringLiteral("org.freedesktop.Notifications")).value();
	}
#endif
	return false;
}

bool TrayManager::isMessagesSupported() const
{
	return isNativeNotificationsAvailable() || FSystemIcon.supportsMessages();
}

void TrayManager::showMessage(const QString &ATitle, const QString &AMessage, QSystemTrayIcon::MessageIcon AIcon, int ATimeout)
{
#ifdef Q_OS_LINUX
	if (isNativeNotificationsAvailable())
	{
		QDBusInterface notificationService(QStringLiteral("org.freedesktop.Notifications"),
			QStringLiteral("/org/freedesktop/Notifications"),
			QStringLiteral("org.freedesktop.Notifications"),
			QDBusConnection::sessionBus());
		if (notificationService.isValid())
		{
			// Qt's QSystemTrayIcon only adds the "default" action for critical
			// messages, so a normal popup cannot be activated by a click. Talk
			// to the notification service directly and always provide it.
			QStringList actions;
			actions << QStringLiteral("default") << tr("Open");

			QVariantMap hints;
			hints.insert(QStringLiteral("urgency"), AIcon==QSystemTrayIcon::Critical ? 2 : (AIcon==QSystemTrayIcon::Warning ? 1 : 0));

			QDBusReply<uint> reply = notificationService.call(QStringLiteral("Notify"),
				QCoreApplication::applicationName(), 0u, FSystemIcon.icon().name(),
				ATitle, AMessage, actions, hints, ATimeout);
			if (reply.isValid())
			{
				FNativeNotificationIds.insert(reply.value());
				LOG_INFO(QString("Native notification shown, id=%1").arg(reply.value()));
				emit messageShown(ATitle,AMessage,AIcon,ATimeout);
				return;
			}
			LOG_WARNING(QString("Failed to show native notification: %1").arg(reply.error().message()));
		}
	}
#endif
	FSystemIcon.showMessage(ATitle,AMessage,AIcon,ATimeout);
	emit messageShown(ATitle,AMessage,AIcon,ATimeout);
}

void TrayManager::onNativeNotificationActionInvoked(uint AId, const QString &AAction)
{
	if (FNativeNotificationIds.remove(AId))
	{
		LOG_INFO(QString("Native notification action invoked, id=%1, action=%2").arg(AId).arg(AAction));
		if (AAction == QLatin1String("default"))
			emit messageClicked();
	}
}

void TrayManager::onNativeNotificationClosed(uint AId, uint AReason)
{
	Q_UNUSED(AReason);
	FNativeNotificationIds.remove(AId);
}

void TrayManager::updateTray()
{
	int notifyId = !FNotifyOrder.isEmpty() ? FNotifyOrder.last() : -1;
	if (notifyId != FActiveNotify)
	{
		FBlinkVisible = true;
		FBlinkTimer.stop();
		FActiveNotify = notifyId;

		if (FActiveNotify > 0)
		{
			const ITrayNotify &notify = FNotifyItems.value(notifyId);
			if (notify.blink)
				FBlinkTimer.start(BLINK_VISIBLE_TIME);
			if (!notify.iconKey.isEmpty() && !notify.iconStorage.isEmpty())
				IconStorage::staticStorage(notify.iconStorage)->insertAutoIcon(&FSystemIcon,notify.iconKey);
			else
				FSystemIcon.setIcon(notify.icon);
			FSystemIcon.setToolTip(notify.toolTip);
		}
		else
		{
			FSystemIcon.setIcon(FIcon);
			FSystemIcon.setToolTip(FToolTip);
		}

		emit activeNotifyChanged(notifyId);
	}
}

void TrayManager::onTrayIconActivated(QSystemTrayIcon::ActivationReason AReason)
{
	if (FActiveNotify > 0)
		LOG_DEBUG(QString("Tray notification activated, id=%1").arg(FActiveNotify));
	emit notifyActivated(FActiveNotify,AReason);
}

void TrayManager::onBlinkTimerTimeout()
{
	const ITrayNotify &notify = FNotifyItems.value(FActiveNotify);
	if (!FBlinkVisible)
	{
		if (!notify.iconStorage.isEmpty() && !notify.iconKey.isEmpty())
			IconStorage::staticStorage(notify.iconStorage)->insertAutoIcon(&FSystemIcon,notify.iconKey);
		else
			FSystemIcon.setIcon(notify.icon);
		FBlinkVisible = true;
		FBlinkTimer.start(BLINK_VISIBLE_TIME);
	}
	else
	{
		IconStorage::staticStorage(notify.iconStorage)->removeAutoIcon(&FSystemIcon);
		FSystemIcon.setIcon(FEmptyIcon);
		FBlinkVisible = false;
		FBlinkTimer.start(BLINK_INVISIBLE_TIME);
	}
}
