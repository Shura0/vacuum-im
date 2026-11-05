#include <utils/qtcompat.h>
#include <QActionGroup>
#include "chatmessagehandler.h"

#include <QMouseEvent>
#include <QApplication>
#include <QPointer>
#include <definitions/actiongroups.h>
#include <definitions/toolbargroups.h>
#include <definitions/resources.h>
#include <definitions/menuicons.h>
#include <definitions/soundfiles.h>
#include <definitions/shortcuts.h>
#include <definitions/optionvalues.h>
#include <definitions/rosterindexkinds.h>
#include <definitions/rosterindexroles.h>
#include <definitions/rosterclickhookerorders.h>
#include <definitions/rosternotifyorders.h>
#include <definitions/recentitemtypes.h>
#include <definitions/notificationtypes.h>
#include <definitions/notificationdataroles.h>
#include <definitions/notificationtypeorders.h>
#include <definitions/tabpagenotifypriorities.h>
#include <definitions/messagedataroles.h>
#include <definitions/messagehandlerorders.h>
#include <definitions/messageeditsendhandlerorders.h>
#include <definitions/xmppurihandlerorders.h>
#include <utils/widgetmanager.h>
#include <utils/textmanager.h>
#include <utils/shortcuts.h>
#include <utils/options.h>
#include <utils/logger.h>

#define HISTORY_MESSAGES          20
#define HISTORY_TIME_DELTA        5
#define HISTORY_DUBLICATE_DELTA   2*60
#define HISTORY_COLLECTION_TIME   20*60

// Delay before the server archive is re-queried after a reconnection. It gives
// service discovery time to report the archive capabilities of the server.
#define HISTORY_CATCHUP_TIMEOUT   5000
// Number of recently displayed messages kept in memory to detect the copies of
// the already shown messages that the archive may return after a reconnection.
#define HISTORY_DUPLICATE_MESSAGES 20

#define ADR_STREAM_JID            Action::DR_StreamJid
#define ADR_CONTACT_JID           Action::DR_Parametr1

static bool isSameMessage(const Message &AMessage, const Message &AOther)
{
	// Same origin stanza id is an exact match. This also covers messages that
	// were corrected after delivery (bridges often edit their notifications to
	// append attachment URLs): the archive copy and the live copy differ in
	// text and may get re-identified, but an equal id settles it immediately.
	if (!AMessage.id().isEmpty() && !AOther.id().isEmpty() && AMessage.id()==AOther.id())
		return true;

	// Without a matching id, encrypted messages often have no readable body
	// (or the same generic fallback body).  Do not collapse them by text: this
	// used to drop OMEMO messages from history as if they were duplicates.
	static const QString omemoNs = QStringLiteral("eu.siacs.conversations.axolotl");
	if (!AMessage.stanza().firstElement(QStringLiteral("encrypted"),omemoNs).isNull()
		|| !AOther.stanza().firstElement(QStringLiteral("encrypted"),omemoNs).isNull())
		return false;

	// Plain copies of the same message coming from different archive engines
	// may carry different (or missing) ids, so compare by content: an archive
	// copy differs from the live copy only by network/clock skew.
	return AMessage.data(MDR_MESSAGE_DIRECTION)==AOther.data(MDR_MESSAGE_DIRECTION)
		&& AMessage.body()==AOther.body()
		&& qAbs(AMessage.dateTime().secsTo(AOther.dateTime()))<HISTORY_DUBLICATE_DELTA;
}

// Consecutive copies of the same message (from different archive engines, or
// from the live stream) collapse into a single entry.
static void collapseSameMessages(QList<Message> &AMessages)
{
	QList<Message> collapsed;
	foreach(const Message &message, AMessages)
	{
		if (collapsed.isEmpty() || !isSameMessage(collapsed.last(),message))
			collapsed.append(message);
	}
	AMessages = collapsed;
}

static const QList<int> ChatHandlerRosterKinds = QList<int>() << RIK_CONTACT << RIK_AGENT << RIK_MY_RESOURCE << RIK_METACONTACT << RIK_METACONTACT_ITEM;

ChatMessageHandler::ChatMessageHandler()
{
	FAvatars = NULL;
	FMainWindowPlugin = NULL;
	FMessageWidgets = NULL;
	FMessageProcessor = NULL;
	FMessageStyleManager = NULL;
	FRosterManager = NULL;
	FPresenceManager = NULL;
	FMessageArchiver = NULL;
	FRostersView = NULL;
	FRostersModel = NULL;
	FStatusIcons = NULL;
	FStatusChanger = NULL;
	FNotifications = NULL;
	FAccountManager = NULL;
	FXmppUriQueries = NULL;
	FRecentContacts = NULL;
	FEndToEndEncryption = NULL;
	FPluginManager = NULL;

	FCatchUpTimer.setSingleShot(true);
	connect(&FCatchUpTimer,SIGNAL(timeout()),SLOT(onCatchUpTimerTimeout()));
}

ChatMessageHandler::~ChatMessageHandler()
{

}

void ChatMessageHandler::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Chat Messages");
	APluginInfo->description = tr("Allows to exchange chat messages");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Potapov S.A. aka Lion";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGESTYLES_UUID));
}


bool ChatMessageHandler::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);
	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface("IMessageWidgets").value(0,NULL);
	if (plugin)
	{
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());
		if (FMessageWidgets)
			connect(FMessageWidgets->instance(),SIGNAL(chatWindowCreated(IMessageChatWindow *)),
				SLOT(onMessageChatWindowCreated(IMessageChatWindow *)));
	}

	plugin = APluginManager->pluginInterface("IMessageProcessor").value(0,NULL);
	if (plugin)
	{
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());
		if (FMessageProcessor)
			connect(FMessageProcessor->instance(),SIGNAL(activeStreamRemoved(const Jid &)),SLOT(onActiveStreamRemoved(const Jid &)));
	}

	plugin = APluginManager->pluginInterface("IMainWindowPlugin").value(0,NULL);
	if (plugin)
	{
		FMainWindowPlugin = qobject_cast<IMainWindowPlugin *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IMessageStyleManager").value(0,NULL);
	if (plugin)
	{
		FMessageStyleManager = qobject_cast<IMessageStyleManager *>(plugin->instance());
		if (FMessageStyleManager)
		{
			connect(FMessageStyleManager->instance(),SIGNAL(styleOptionsChanged(const IMessageStyleOptions &, int, const QString &)),
				SLOT(onStyleOptionsChanged(const IMessageStyleOptions &, int, const QString &)));
		}
	}

	plugin = APluginManager->pluginInterface("IAvatars").value(0,NULL);
	if (plugin)
	{
		FAvatars = qobject_cast<IAvatars *>(plugin->instance());
		if (FAvatars)
		{
			connect(FAvatars->instance(),SIGNAL(avatarChanged(const Jid &)),SLOT(onAvatarChanged(const Jid &)));
		}
	}

	plugin = APluginManager->pluginInterface("IStatusIcons").value(0,NULL);
	if (plugin)
	{
		FStatusIcons = qobject_cast<IStatusIcons *>(plugin->instance());
		if (FStatusIcons)
		{
			connect(FStatusIcons->instance(),SIGNAL(statusIconsChanged()),SLOT(onStatusIconsChanged()));
		}
	}

	plugin = APluginManager->pluginInterface("IRosterManager").value(0,NULL);
	if (plugin)
	{
		FRosterManager = qobject_cast<IRosterManager *>(plugin->instance());
		if (FRosterManager)
		{
			connect(FRosterManager->instance(),SIGNAL(rosterItemReceived(IRoster *, const IRosterItem &, const IRosterItem &)),
				SLOT(onRosterItemReceived(IRoster *, const IRosterItem &, const IRosterItem &)));
		}
	}

	plugin = APluginManager->pluginInterface("IPresenceManager").value(0,NULL);
	if (plugin)
	{
		FPresenceManager = qobject_cast<IPresenceManager *>(plugin->instance());
		if (FPresenceManager)
		{
			connect(FPresenceManager->instance(),SIGNAL(presenceItemReceived(IPresence *, const IPresenceItem &, const IPresenceItem &)),
				SLOT(onPresenceItemReceived(IPresence *, const IPresenceItem &, const IPresenceItem &)));
		}
	}

	plugin = APluginManager->pluginInterface("IMessageArchiver").value(0,NULL);
	if (plugin)
	{
		FMessageArchiver = qobject_cast<IMessageArchiver *>(plugin->instance());
		if (FMessageArchiver)
		{
			connect(FMessageArchiver->instance(),SIGNAL(archivePrefsOpened(const Jid &)),SLOT(onArchivePrefsOpened(const Jid &)));
			connect(FMessageArchiver->instance(),SIGNAL(archivePrefsClosed(const Jid &)),SLOT(onArchivePrefsClosed(const Jid &)));
			connect(FMessageArchiver->instance(),SIGNAL(messagesLoaded(const QString &, const IArchiveCollectionBody &)),
				SLOT(onArchiveMessagesLoaded(const QString &, const IArchiveCollectionBody &)));
			connect(FMessageArchiver->instance(),SIGNAL(requestFailed(const QString &, const XmppError &)),
				SLOT(onArchiveRequestFailed(const QString &, const XmppError &)));
		}
	}

	plugin = APluginManager->pluginInterface("IRostersViewPlugin").value(0,NULL);
	if (plugin)
	{
		IRostersViewPlugin *rostersViewPlugin = qobject_cast<IRostersViewPlugin *>(plugin->instance());
		if (rostersViewPlugin)
		{
			FRostersView = rostersViewPlugin->rostersView();
			connect(FRostersView->instance(),SIGNAL(indexContextMenu(const QList<IRosterIndex *> &, quint32, Menu *)),
				SLOT(onRostersViewIndexContextMenu(const QList<IRosterIndex *> &, quint32, Menu *)));
		}
	}

	plugin = APluginManager->pluginInterface("IRostersModel").value(0,NULL);
	if (plugin)
	{
		FRostersModel = qobject_cast<IRostersModel *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("INotifications").value(0,NULL);
	if (plugin)
	{
		FNotifications = qobject_cast<INotifications *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IStatusChanger").value(0,NULL);
	if (plugin)
	{
		FStatusChanger = qobject_cast<IStatusChanger *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IAccountManager").value(0,NULL);
	if (plugin)
	{
		FAccountManager = qobject_cast<IAccountManager *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IXmppUriQueries").value(0,NULL);
	if (plugin)
	{
		FXmppUriQueries = qobject_cast<IXmppUriQueries *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IRecentContacts").value(0,NULL);
	if (plugin)
	{
		FRecentContacts = qobject_cast<IRecentContacts *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IEndToEndEncryption").value(0,NULL);
	if (plugin)
	{
		FEndToEndEncryption = qobject_cast<IEndToEndEncryption *>(plugin->instance());
		if (FEndToEndEncryption)
			connect(FEndToEndEncryption->instance(),SIGNAL(destroyed(QObject *)),SLOT(onEndToEndEncryptionDestroyed(QObject *)));
	}

	connect(Shortcuts::instance(),SIGNAL(shortcutActivated(const QString &, QWidget *)),SLOT(onShortcutActivated(const QString &, QWidget *)));

	return FMessageProcessor!=NULL && FMessageWidgets!=NULL && FMessageStyleManager!=NULL;
}

bool ChatMessageHandler::initObjects()
{
	Shortcuts::declareShortcut(SCT_ROSTERVIEW_SHOWCHATDIALOG, tr("Open chat dialog"), tr("Return","Open chat dialog"), Shortcuts::WidgetShortcut);
	if (FMainWindowPlugin)
		connect(FMainWindowPlugin->mainWindow()->instance(),SIGNAL(mainWindowFirstShown()),SLOT(onMainWindowFirstShown()));

	if (FNotifications)
	{
		INotificationType notifyType;
		notifyType.order = NTO_CHATHANDLER_MESSAGE;
		notifyType.icon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_CHATMHANDLER_MESSAGE);
		notifyType.title = tr("When receiving new chat message");
		notifyType.kindMask = INotification::RosterNotify|INotification::PopupWindow|INotification::TrayNotify|INotification::TrayAction|INotification::SoundPlay|INotification::AlertWidget|INotification::TabPageNotify|INotification::ShowMinimized|INotification::AutoActivate;
		notifyType.kindDefs = notifyType.kindMask & ~(INotification::ShowMinimized|INotification::AutoActivate);
		FNotifications->registerNotificationType(NNT_CHAT_MESSAGE,notifyType);
	}
	if (FRostersView)
	{
		FRostersView->insertClickHooker(RCHO_CHATMESSAGEHANDLER,this);
		Shortcuts::insertWidgetShortcut(SCT_ROSTERVIEW_SHOWCHATDIALOG,FRostersView->instance());
	}
	if (FMessageProcessor)
	{
		FMessageProcessor->insertMessageHandler(MHO_CHATMESSAGEHANDLER,this);
	}
	if (FXmppUriQueries)
	{
		FXmppUriQueries->insertUriHandler(XUHO_DEFAULT,this);
	}
	if (FMessageWidgets)
	{
		FMessageWidgets->insertEditSendHandler(MESHO_CHATMESSAGEHANDLER,this);
	}
	return true;
}

bool ChatMessageHandler::initSettings()
{
	Options::setDefaultValue(OPV_MESSAGES_LOADHISTORY, true);
	return true;
}

bool ChatMessageHandler::messageEditSendPrepare(int AOrder, IMessageEditWidget *AWidget)
{
	Q_UNUSED(AOrder); Q_UNUSED(AWidget);
	return false;
}

bool ChatMessageHandler::messageEditSendProcesse(int AOrder, IMessageEditWidget *AWidget)
{
	if (AOrder == MESHO_CHATMESSAGEHANDLER)
	{
		IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(AWidget->messageWindow()->instance());
		if (FMessageProcessor && FWindows.contains(window))
		{
			Message message;
			message.setType(Message::Chat).setTo(window->contactJid().full());
			if (FMessageProcessor->textToMessage(AWidget->document(),message))
				return FMessageProcessor->sendMessage(window->streamJid(),message,IMessageProcessor::DirectionOut);
		}
	}
	return false;
}

bool ChatMessageHandler::messageCheck(int AOrder, const Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder); Q_UNUSED(ADirection);
	if (AMessage.type() == Message::Chat)
		return FMessageProcessor!=NULL ? FMessageProcessor->messageHasText(AMessage) : !AMessage.body().isEmpty();
	return false;
}

bool ChatMessageHandler::messageDisplay(const Message &AMessage, int ADirection)
{
	IMessageChatWindow *window = ADirection==IMessageProcessor::DirectionIn ? getWindow(AMessage.to(),AMessage.from()) : getWindow(AMessage.from(),AMessage.to());
	if (window)
	{
		if (FRecentContacts)
		{
			IRecentItem recentItem;
			recentItem.type = REIT_CONTACT;
			recentItem.streamJid = window->streamJid();
			recentItem.reference = window->contactJid().pBare();
			FRecentContacts->setItemActiveTime(recentItem);
		}

		if (FDestroyTimers.contains(window))
			delete FDestroyTimers.take(window);

		if (FHistoryRequests.values().contains(window))
			FPendingMessages[window].append(AMessage);

		if (ADirection == IMessageProcessor::DirectionIn)
		{
			if (window->streamJid()!=AMessage.to() || window->contactJid()!=AMessage.from())
			{
				LOG_STRM_DEBUG(window->streamJid(),QString("Changing chat window address from=%1 to=%2").arg(window->contactJid().full(),AMessage.from()));
				window->address()->setAddress(AMessage.to(),AMessage.from());
			}
		}

		showStyledMessage(window,AMessage);
	}
	else
	{
		REPORT_ERROR(QString("Failed to display message type=%1: Chat window not created").arg(AMessage.type()));
	}

	return window!=NULL;
}

INotification ChatMessageHandler::messageNotify(INotifications *ANotifications, const Message &AMessage, int ADirection)
{
	INotification notify;
	if (ADirection == IMessageProcessor::DirectionIn)
	{
		IMessageChatWindow *window = findWindow(AMessage.to(),AMessage.from());
		if (window)
		{
			QString typeId = NNT_CHAT_MESSAGE;

			notify.kinds = ANotifications->enabledTypeNotificationKinds(typeId);
			if (window->isActiveTabPage())
				notify.kinds &= Options::node(OPV_NOTIFICATIONS_FORCESOUND).value().toBool() ? INotification::SoundPlay : 0;

			if (notify.kinds > 0)
			{
				QIcon icon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_CHATMHANDLER_MESSAGE);
				QString name = ANotifications->contactName(AMessage.to(),AMessage.from());

				notify.typeId = typeId;
				notify.data.insert(NDR_ICON,icon);
				notify.data.insert(NDR_TOOLTIP,tr("Message from %1").arg(name));
				notify.data.insert(NDR_STREAM_JID,AMessage.to());
				notify.data.insert(NDR_CONTACT_JID,AMessage.from());
				notify.data.insert(NDR_ROSTER_ORDER,RNO_CHATMESSAGE);
				notify.data.insert(NDR_ROSTER_FLAGS,IRostersNotify::Blink|IRostersNotify::AllwaysVisible|IRostersNotify::HookClicks);
				notify.data.insert(NDR_ROSTER_CREATE_INDEX,true);
				notify.data.insert(NDR_POPUP_IMAGE,ANotifications->contactAvatar(AMessage.from()));
				notify.data.insert(NDR_POPUP_CAPTION,tr("Message received"));
				notify.data.insert(NDR_POPUP_TITLE,name);
				notify.data.insert(NDR_SOUND_FILE,SDF_CHAT_MHANDLER_MESSAGE);

				notify.data.insert(NDR_ALERT_WIDGET,(qint64)window->instance());
				notify.data.insert(NDR_TABPAGE_WIDGET,(qint64)window->instance());
				notify.data.insert(NDR_TABPAGE_PRIORITY,TPNP_NEW_MESSAGE);
				notify.data.insert(NDR_TABPAGE_ICONBLINK,true);
				notify.data.insert(NDR_SHOWMINIMIZED_WIDGET,(qint64)window->instance());

				if (!Options::node(OPV_NOTIFICATIONS_HIDEMESSAGE).value().toBool())
				{
					QTextDocument doc;
					if (FMessageProcessor && FMessageProcessor->messageToText(AMessage,&doc))
						notify.data.insert(NDR_POPUP_HTML,TextManager::getDocumentBody(doc));
					notify.data.insert(NDR_POPUP_TEXT,AMessage.body());
				}

				FNotifiedMessages.insert(window, AMessage.data(MDR_MESSAGE_ID).toInt());
			}
		}
		else if (window == NULL)
		{
			LOG_STRM_ERROR(AMessage.to(),QString("Failed to notify message from=%1: Window not found").arg(AMessage.from()));
		}
	}
	return notify;
}

IMessageWindow *ChatMessageHandler::messageShowNotified(int AMessageId)
{
	IMessageChatWindow *window = FNotifiedMessages.key(AMessageId);
	if (window)
	{
		window->showTabPage();
		return window;
	}
	else
	{
		REPORT_ERROR("Failed to show notified chat message window: Window not found");
	}
	return NULL;
}

IMessageWindow *ChatMessageHandler::messageGetWindow(const Jid &AStreamJid, const Jid &AContactJid, Message::MessageType AType)
{
	return AType==Message::Chat ? getWindow(AStreamJid,AContactJid) : NULL;
}

bool ChatMessageHandler::rosterIndexSingleClicked(int AOrder, IRosterIndex *AIndex, const QMouseEvent *AEvent)
{
	if (Options::node(OPV_MESSAGES_COMBINEWITHROSTER).value().toBool())
		return rosterIndexDoubleClicked(AOrder, AIndex, AEvent);
	return false;
}

bool ChatMessageHandler::rosterIndexDoubleClicked(int AOrder, IRosterIndex *AIndex, const QMouseEvent *AEvent)
{
	if (AOrder==RCHO_CHATMESSAGEHANDLER && AEvent->modifiers()==Qt::NoModifier && ChatHandlerRosterKinds.contains(AIndex->kind()) && AIndex->kind()!=RIK_AGENT)
		return showWindow(AIndex->data(RDR_STREAM_JID).toString(), AIndex->data(RDR_FULL_JID).toString()) != NULL;
	return false;
}

bool ChatMessageHandler::xmppUriOpen(const Jid &AStreamJid, const Jid &AContactJid, const QString &AAction, const QMultiMap<QString, QString> &AParams)
{
	if (AAction == "message")
	{
		QString type = AParams.value("type");
		if (type == "chat")
		{
			IMessageChatWindow *window = getWindow(AStreamJid, AContactJid);
			if (window)
			{
				window->editWidget()->textEdit()->setPlainText(AParams.value("body"));
				window->showTabPage();
				return true;
			}
			else
			{
				LOG_STRM_WARNING(AStreamJid,QString("Failed to open chat window by XMPP URI, with=%1: Window not created").arg(AContactJid.bare()));
			}
		}
	}
	return false;
}

IMessageChatWindow *ChatMessageHandler::showWindow(const Jid &AStreamJid, const Jid &AContactJid)
{
	IMessageChatWindow *window = getWindow(AStreamJid, AContactJid);
	if (window)
	{
		saveLastChatWindow(window);
		window->showTabPage();
	}
	return window;
}

void ChatMessageHandler::saveLastChatWindow(IMessageChatWindow *AWindow)
{
	if (AWindow!=NULL && AWindow->streamJid().isValid() && AWindow->contactJid().isValid())
	{
		Options::node(OPV_MESSAGES_LASTCHAT_STREAM).setValue(AWindow->streamJid().full());
		// Store the bare contact: the resource seen at shutdown is usually
		// stale after a restart, and the chat window addresses resources
		// automatically.
		Options::node(OPV_MESSAGES_LASTCHAT_CONTACT).setValue(AWindow->contactJid().bare());
	}
}

void ChatMessageHandler::clearLastChatWindow(const Jid &AStreamJid, const Jid &AContactJid, bool AIgnoreShutdown)
{
	// Do not lose the restore target while the application is shutting down:
	// closing all windows at exit must not clear the chat to restore next time.
	// A window explicitly closed by the user before quit still clears it.
	if (!AIgnoreShutdown && FPluginManager!=NULL && FPluginManager->isShutingDown())
		return;

	const Jid streamJid = Options::node(OPV_MESSAGES_LASTCHAT_STREAM).value().toString();
	const Jid contactJid = Options::node(OPV_MESSAGES_LASTCHAT_CONTACT).value().toString();
	if (streamJid==AStreamJid && contactJid==AContactJid.bare())
	{
		Options::node(OPV_MESSAGES_LASTCHAT_STREAM).setValue(QString());
		Options::node(OPV_MESSAGES_LASTCHAT_CONTACT).setValue(QString());
	}
}

// the chat which was open last time is shown in the main window on its first
// show after the application start, the window is not created by the start
// itself so that the main window is not shown by it
void ChatMessageHandler::onMainWindowFirstShown()
{
	const Jid streamJid = Options::node(OPV_MESSAGES_LASTCHAT_STREAM).value().toString();
	const Jid contactJid = Jid(Options::node(OPV_MESSAGES_LASTCHAT_CONTACT).value().toString()).bare();
	if (streamJid.isValid() && contactJid.isValid())
	{
		QTimer::singleShot(0, this, [this, streamJid, contactJid]() {
			IMessageChatWindow *window = getWindow(streamJid, contactJid);
			if (window != NULL)
			{
				// getWindow() creates a top-level ChatWindow which is only
				// attached to the tab window by assignTabPage(); checking
				// isWindow() here would skip the restore of every new window.
				window->showTabPage();
			}
		});
	}
}

IMessageChatWindow *ChatMessageHandler::getWindow(const Jid &AStreamJid, const Jid &AContactJid)
{
	IMessageChatWindow *window = NULL;
	if (FMessageProcessor && AStreamJid.isValid() && AContactJid.isValid())
	{
		// The windows are independent of the connection state: an offline
		// window shows the local file-archive history, only sending a message
		// requires the stream to be connected.
		window = findWindow(AStreamJid,AContactJid);
		if (!window)
		{
			window = FMessageWidgets->getChatWindow(AStreamJid,AContactJid);
			if (!window)
			{
				LOG_STRM_ERROR(AStreamJid,QString("Failed to create chat window, with=%1: Instance is not created").arg(AContactJid.bare()));
			}
		}
	}
	else if (FMessageProcessor == NULL)
	{
		REPORT_ERROR("Failed to create chat window: IMessageProcessor is NULL");
	}
	else if (!AStreamJid.isValid())
	{
		REPORT_ERROR("Failed to create chat window: Stream Jid is not valid");
	}
	else if (!AContactJid.isValid())
	{
		REPORT_ERROR("Failed to create chat window: Contact is not valid");
	}
	return window;
}

void ChatMessageHandler::onMessageChatWindowCreated(IMessageChatWindow *AWindow)
{
	// The signal fires during FMessageWidgets->getChatWindow, so every window
	// gets wired up no matter which way it was opened (roster click, shortcut,
	// last chat restore, XMPP URI, vim ":chat" command or an incoming message).
	LOG_STRM_INFO(AWindow->streamJid(),QString("Chat window created, with=%1").arg(AWindow->contactJid().bare()));

	AWindow->address()->setAutoAddresses(true);
	AWindow->infoWidget()->setAddressMenuVisible(true);
	AWindow->infoWidget()->addressMenu()->menuAction()->setToolTip(tr("Contact resource"));
	AWindow->setTabPageNotifier(FMessageWidgets->newTabPageNotifier(AWindow));

	connect(AWindow->instance(),SIGNAL(tabPageActivated()),SLOT(onWindowActivated()));
	connect(AWindow->instance(),SIGNAL(tabPageClosed()),SLOT(onWindowClosed()));
	connect(AWindow->instance(),SIGNAL(tabPageDestroyed()),SLOT(onWindowDestroyed()));
	connect(AWindow->address()->instance(),SIGNAL(addressChanged(const Jid &, const Jid &)),SLOT(onWindowAddressChanged()));
	connect(AWindow->address()->instance(),SIGNAL(availAddressesChanged()),SLOT(onWindowAvailAddressesChanged()));
	connect(AWindow->infoWidget()->instance(),SIGNAL(addressMenuRequested(Menu *)),SLOT(onWindowAddressMenuRequested(Menu *)));
	connect(AWindow->infoWidget()->instance(),SIGNAL(contextMenuRequested(Menu *)),SLOT(onWindowContextMenuRequested(Menu *)));
	connect(AWindow->infoWidget()->instance(),SIGNAL(toolTipsRequested(QMap<int,QString> &)),SLOT(onWindowToolTipsRequested(QMap<int,QString> &)));
	connect(AWindow->viewWidget()->instance(),SIGNAL(contentAppended(const QString &, const IMessageStyleContentOptions &)),
		SLOT(onWindowContentAppended(const QString &, const IMessageStyleContentOptions &)));
	connect(AWindow->viewWidget()->instance(),SIGNAL(messageStyleOptionsChanged(const IMessageStyleOptions &, bool)),
		SLOT(onWindowMessageStyleOptionsChanged(const IMessageStyleOptions &, bool)));
	connect(AWindow->tabPageNotifier()->instance(),SIGNAL(activeNotifyChanged(int)),this,SLOT(onWindowNotifierActiveNotifyChanged(int)));

	FWindows.append(AWindow);
	FWindowStatus[AWindow].createTime = QDateTime::currentDateTime();

	Action *clearAction = new Action(AWindow->instance());
	clearAction->setText(tr("Clear Chat Window"));
	clearAction->setIcon(RSR_STORAGE_MENUICONS,MNI_CHATMHANDLER_CLEAR_CHAT);
	connect(clearAction,SIGNAL(triggered(bool)),SLOT(onClearWindowAction(bool)));
	AWindow->toolBarWidget()->toolBarChanger()->insertAction(clearAction, TBG_MWTBW_CLEAR_WINDOW);

	updateWindow(AWindow);
	setMessageStyle(AWindow);
	requestHistory(AWindow);
}

IMessageChatWindow *ChatMessageHandler::findWindow(const Jid &AStreamJid, const Jid &AContactJid) const
{
	foreach(IMessageChatWindow *window, FWindows)
		if (window->address()->availAddresses(true).contains(AStreamJid,AContactJid.bare()))
			return window;
	return NULL;
}

void ChatMessageHandler::updateWindow(IMessageChatWindow *AWindow)
{
	if (FAvatars)
	{
		QString avatar = FAvatars->avatarHash(AWindow->contactJid());
		if (FAvatars->hasAvatar(avatar))
			AWindow->infoWidget()->setFieldValue(IMessageInfoWidget::Avatar,avatar);
		else
			AWindow->infoWidget()->setFieldValue(IMessageInfoWidget::Avatar,FAvatars->emptyAvatarImage(FAvatars->avatarSize(IAvatars::AvatarSmall)));
	}

	QString name = FMessageStyleManager!=NULL ? FMessageStyleManager->contactName(AWindow->streamJid(),AWindow->contactJid()) : AWindow->contactJid().uFull();
	AWindow->infoWidget()->setFieldValue(IMessageInfoWidget::Caption,name);

	QIcon statusIcon = FStatusIcons!=NULL ? FStatusIcons->iconByJid(AWindow->streamJid(),AWindow->contactJid()) : QIcon();
	AWindow->infoWidget()->setFieldValue(IMessageInfoWidget::StatusIcon,statusIcon);

	IPresence *presence = FPresenceManager!=NULL ? FPresenceManager->findPresence(AWindow->streamJid()) : NULL;
	IPresenceItem pitem = presence!=NULL ? presence->findItem(AWindow->contactJid()) : IPresenceItem();
	AWindow->infoWidget()->setFieldValue(IMessageInfoWidget::StatusText,pitem.status);

	QString resource = AWindow->contactJid().hasResource() ? AWindow->contactJid().resource() : tr("<Absent>");
	AWindow->infoWidget()->addressMenu()->setTitle(TextManager::getElidedString(resource,Qt::ElideRight,20));

	QIcon tabIcon = statusIcon;
	if (AWindow->tabPageNotifier() && AWindow->tabPageNotifier()->activeNotify()>0)
		tabIcon = AWindow->tabPageNotifier()->notifyById(AWindow->tabPageNotifier()->activeNotify()).icon;

	AWindow->updateWindow(tabIcon,name,tr("%1 - Chat").arg(name),QString());
}

void ChatMessageHandler::onEndToEndEncryptionDestroyed(QObject *AObject)
{
	Q_UNUSED(AObject);
	// The encryption plugin is gone (usually at shutdown): stop calling into it.
	// Do not dereference the sender - its derived part is already destroyed.
	FEndToEndEncryption = NULL;
}

void ChatMessageHandler::removeNotifiedMessages(IMessageChatWindow *AWindow)
{
	if (FNotifiedMessages.contains(AWindow))
	{
		foreach(int messageId, FNotifiedMessages.values(AWindow))
			FMessageProcessor->removeMessageNotify(messageId);
		FNotifiedMessages.remove(AWindow);
	}
}

void ChatMessageHandler::showHistory(IMessageChatWindow *AWindow)
{
	if (!FHistoryRequests.values().contains(AWindow))
	{
		AWindow->viewWidget()->clearContent();

		QList<Message> pending = FPendingMessages.take(AWindow);
		IArchiveCollectionBody history = FHistoryMessages.take(AWindow);
		LOG_STRM_INFO(AWindow->streamJid(),QString("Chat history display, with=%1, messages=%2")
			.arg(AWindow->contactJid().bare()).arg(history.messages.count()));
		std::stable_sort(history.messages.begin(),history.messages.end(),qGreater<Message>());

		// Remove copies of the same message that different archive engines and
		// the live stream may both deliver.
		collapseSameMessages(history.messages);
		LOG_STRM_INFO(AWindow->streamJid(),QString("Chat history collapsed, with=%1, messages=%2")
			.arg(AWindow->contactJid().bare()).arg(history.messages.count()));

		// Remove extra history messages
		if (history.messages.count() > HISTORY_MESSAGES)
		{
			QDateTime removeTime;
			for(QList<Message>::iterator it=history.messages.begin()+HISTORY_MESSAGES; it!=history.messages.end(); it = removeTime.isValid() ? history.messages.erase(it) : it+1)
				if (it->dateTime().secsTo((it-1)->dateTime()) > HISTORY_COLLECTION_TIME)
					removeTime = (it-1)->dateTime().addSecs(HISTORY_COLLECTION_TIME);

			for(QMultiMap<QDateTime,QString>::iterator it=history.notes.upperBound(removeTime); removeTime.isValid() && it!=history.notes.end(); )
				it = history.notes.erase(it);
		}

		// Show all archive messages. Live messages that arrived while the
		// request was in flight are re-added below after the archive.
		int messageIt = history.messages.count()-1;
		QMultiMap<QDateTime,QString>::const_iterator noteIt = history.notes.constBegin();
		while (messageIt>=0 || noteIt!=history.notes.constEnd())
		{
			if (messageIt>=0 && (noteIt==history.notes.constEnd() || history.messages.at(messageIt).dateTime()<noteIt.key()))
			{
				showStyledMessage(AWindow,history.messages.at(messageIt));
				messageIt--;
			}
			else if (noteIt != history.notes.constEnd())
			{
				showStyledStatus(AWindow,noteIt.value(),true,noteIt.key());
				++noteIt;
			}
		}

		// A live message can arrive while the archive request is in flight. It
		// was already shown once, but clearContent() removes it; re-add every
		// pending message that is not present in the archive result.
		foreach(const Message &pmessage, pending)
		{
			bool duplicate = false;
			foreach(const Message &hmessage, history.messages)
			{
				if (isSameMessage(hmessage,pmessage))
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				showStyledMessage(AWindow,pmessage);
		}

		// Show pending content
		foreach(const WindowContent &content, FPendingContent.take(AWindow))
		{
			showDateSeparator(AWindow,content.options.time);
			AWindow->viewWidget()->appendHtml(content.html,content.options);
		}

		WindowStatus &wstatus = FWindowStatus[AWindow];
		wstatus.startTime = !history.messages.isEmpty() ? history.messages.last().dateTime() : QDateTime();
	}
}

void ChatMessageHandler::requestHistory(IMessageChatWindow *AWindow)
{
	if (FMessageArchiver && Options::node(OPV_MESSAGES_LOADHISTORY).value().toBool() && !FHistoryRequests.values().contains(AWindow))
	{
		WindowStatus &wstatus = FWindowStatus[AWindow];

		IArchiveRequest request;
		request.order = Qt::DescendingOrder;
		if (wstatus.createTime.secsTo(QDateTime::currentDateTime()) > HISTORY_TIME_DELTA)
			request.start = wstatus.startTime.isValid() ? wstatus.startTime : wstatus.createTime;
		else
			request.maxItems = HISTORY_MESSAGES;
		request.end = QDateTime::currentDateTime();

		showStyledStatus(AWindow,tr("Loading history..."),true);
		QMultiMap<Jid,Jid> addresses = AWindow->address()->availAddresses(true);
		for (QMultiMap<Jid,Jid>::const_iterator it=addresses.constBegin(); it!=addresses.constEnd(); ++it)
		{
			request.with = it.value();
			request.exactmatch = !request.with.hasNode();

			QString reqId = FMessageArchiver->loadMessages(it.key(),request);
			if (!reqId.isEmpty())
			{
				LOG_STRM_INFO(it.key(),QString("Load chat history request sent, with=%1, id=%2").arg(request.with.bare(),reqId));
				FHistoryRequests.insert(reqId,AWindow);
			}
			else
			{
				LOG_STRM_WARNING(it.key(),QString("Failed to send chat history load request, with=%1").arg(request.with.bare()));
			}
		}
	}
}

void ChatMessageHandler::requestCatchUpHistory(IMessageChatWindow *AWindow)
{
	if (FMessageArchiver && Options::node(OPV_MESSAGES_LOADHISTORY).value().toBool()
		&& !FHistoryRequests.values().contains(AWindow) && !FCatchUpRequests.values().contains(AWindow))
	{
		// Messages are requested starting from the last message already shown in
		// the window, so only the archive entries received while the stream was
		// disconnected are appended (duplicates are filtered out by isSameMessage).
		QDateTime start = FWindowStatus.value(AWindow).lastMessageTime;
		if (start.isValid())
		{
			start = start.addSecs(1);
			IArchiveRequest request;
			request.order = Qt::AscendingOrder;
			request.start = start;
			request.end = QDateTime::currentDateTime();

			QMultiMap<Jid,Jid> addresses = AWindow->address()->availAddresses(true);
			for (QMultiMap<Jid,Jid>::const_iterator it=addresses.constBegin(); it!=addresses.constEnd(); ++it)
			{
				request.with = it.value();
				request.exactmatch = !request.with.hasNode();

				QString reqId = FMessageArchiver->loadMessages(it.key(),request);
				if (!reqId.isEmpty())
				{
					LOG_STRM_INFO(it.key(),QString("History catch-up request sent, with=%1, start=%2, id=%3").arg(request.with.bare(),start.toString(Qt::ISODate),reqId));
					FCatchUpRequests.insert(reqId,AWindow);
				}
				else
				{
					LOG_STRM_WARNING(it.key(),QString("Failed to send history catch-up request, with=%1").arg(request.with.bare()));
				}
			}
		}
	}
}

void ChatMessageHandler::appendHistoryMessages(IMessageChatWindow *AWindow, const QList<Message> &AMessages)
{
	QList<Message> messages = AMessages;
	std::stable_sort(messages.begin(),messages.end(),qLess<Message>());

	// The server archive and the local file archive may both return the same
	// message with a slightly different timestamp, and pages of the same
	// server query may overlap, so drop the copies first.
	collapseSameMessages(messages);

	foreach(const Message &message, messages)
	{
		const WindowStatus &wstatus = FWindowStatus.value(AWindow);
		if (wstatus.lastMessageTime.isValid() && message.dateTime()<=wstatus.lastMessageTime)
			continue;

		// The archive may also return the messages which are already shown in
		// the window after reconnection or a client restart.
		bool duplicate = false;
		foreach(const Message &shown, wstatus.lastMessages)
		{
			if (isSameMessage(message,shown))
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			showStyledMessage(AWindow,message);
	}
}

void ChatMessageHandler::setMessageStyle(IMessageChatWindow *AWindow)
{
	if (FMessageStyleManager)
	{
		LOG_STRM_DEBUG(AWindow->streamJid(),QString("Changing message style for chat window, with=%1").arg(AWindow->contactJid().bare()));
		IMessageStyleOptions soptions = FMessageStyleManager->styleOptions(Message::Chat);
		if (AWindow->viewWidget()->messageStyle()==NULL || !AWindow->viewWidget()->messageStyle()->changeOptions(AWindow->viewWidget()->styleWidget(),soptions,true))
		{
			IMessageStyle *style = FMessageStyleManager->styleForOptions(soptions);
			AWindow->viewWidget()->setMessageStyle(style,soptions);
		}
		FWindowStatus[AWindow].lastDateSeparator = QDate();
	}
}

void ChatMessageHandler::showDateSeparator(IMessageChatWindow *AWindow, const QDateTime &ADateTime)
{
	if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
	{
		QDate sepDate = ADateTime.date();
		WindowStatus &wstatus = FWindowStatus[AWindow];
		if (FMessageStyleManager && sepDate.isValid() && wstatus.lastDateSeparator!=sepDate)
		{
			IMessageStyleContentOptions options;
			options.kind = IMessageStyleContentOptions::KindStatus;
			if (wstatus.createTime > ADateTime)
				options.type |= IMessageStyleContentOptions::TypeHistory;
			options.status = IMessageStyleContentOptions::StatusDateSeparator;
			options.direction = IMessageStyleContentOptions::DirectionIn;
			options.time.setDate(sepDate);
			options.time.setTime(QTime(0,0));
			options.timeFormat = " ";
			wstatus.lastDateSeparator = sepDate;
			AWindow->viewWidget()->appendText(FMessageStyleManager->dateSeparator(sepDate),options);
		}
	}
}

void ChatMessageHandler::fillContentOptions(const Jid &AStreamJid, const Jid &AContactJid, IMessageStyleContentOptions &AOptions) const
{
	if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
		AOptions.timeFormat = FMessageStyleManager->timeFormat(AOptions.time,AOptions.time);
	else
		AOptions.timeFormat = FMessageStyleManager->timeFormat(AOptions.time);

	if (AOptions.direction == IMessageStyleContentOptions::DirectionIn)
	{
		AOptions.senderId = AContactJid.pFull();
		AOptions.senderAvatar = FMessageStyleManager->contactAvatar(AContactJid);
		AOptions.senderIcon = FMessageStyleManager->contactIcon(AStreamJid,AContactJid);
		AOptions.senderName = FMessageStyleManager->contactName(AStreamJid,AContactJid).toHtmlEscaped();
	}
	else
	{
		AOptions.senderId = AStreamJid.pFull();
		AOptions.senderAvatar = FMessageStyleManager->contactAvatar(AStreamJid);
		AOptions.senderIcon = FMessageStyleManager->contactIcon(AStreamJid);
		if (AStreamJid.pBare() != AContactJid.pBare())
			AOptions.senderName = FMessageStyleManager->contactName(AStreamJid).toHtmlEscaped();
		else
			AOptions.senderName = AStreamJid.hasResource() ? AStreamJid.resource().toHtmlEscaped() : AStreamJid.uNode().toHtmlEscaped();
	}
}

void ChatMessageHandler::showStyledStatus(IMessageChatWindow *AWindow, const QString &AMessage, bool ADontSave, const QDateTime &ATime)
{
	IMessageStyleContentOptions options;
	options.kind = IMessageStyleContentOptions::KindStatus;
	options.direction = IMessageStyleContentOptions::DirectionIn;
	options.time = ATime;

	if (!ADontSave && FMessageArchiver && Options::node(OPV_MESSAGES_ARCHIVESTATUS).value().toBool())
		FMessageArchiver->saveNote(AWindow->streamJid(), AWindow->contactJid(), AMessage);

	showDateSeparator(AWindow,options.time);
	fillContentOptions(AWindow->streamJid(),AWindow->contactJid(),options);
	AWindow->viewWidget()->appendText(AMessage,options);
}

void ChatMessageHandler::showStyledMessage(IMessageChatWindow *AWindow, const Message &AMessage)
{
	Message message = AMessage;
	const int direction = message.data(MDR_MESSAGE_DIRECTION).toInt();
	const bool wasEncrypted = !message.stanza().firstElement(QStringLiteral("encrypted"),
		QStringLiteral("eu.siacs.conversations.axolotl")).isNull();
	const QString originalBody = message.body();
	if (FEndToEndEncryption == NULL && FPluginManager != NULL)
	{
		IPlugin *plugin = FPluginManager->pluginInterface("IEndToEndEncryption").value(0,NULL);
		if (plugin != NULL)
		{
			FEndToEndEncryption = qobject_cast<IEndToEndEncryption *>(plugin->instance());
			if (FEndToEndEncryption != NULL)
				connect(FEndToEndEncryption->instance(),SIGNAL(destroyed(QObject *)),SLOT(onEndToEndEncryptionDestroyed(QObject *)));
		}
	}
	if (FEndToEndEncryption != NULL)
		FEndToEndEncryption->decryptMessage(AWindow->streamJid(),message,direction);

	// The normal message pipeline may already have decrypted the stanza. The
	// history hook must never make such a message disappear if a cache lookup
	// or a failed decode leaves the body empty.
	if (message.body().isEmpty() && !originalBody.isEmpty())
		message.setBody(originalBody);
	const int bodyAfter = message.body().length();

	IMessageStyleContentOptions options;
	options.kind = IMessageStyleContentOptions::KindMessage;

	options.time = message.dateTime();
	options.messageId = message.id();
	if (options.time.secsTo(FWindowStatus.value(AWindow).createTime)>HISTORY_TIME_DELTA)
		options.type |= IMessageStyleContentOptions::TypeHistory;

	if (direction == IMessageProcessor::DirectionOut)
	{
		options.direction = IMessageStyleContentOptions::DirectionOut;
		fillContentOptions(message.from(),message.to(),options);
	}
	else
	{
		options.direction = IMessageStyleContentOptions::DirectionIn;
		fillContentOptions(message.to(),message.from(),options);
	}

	showDateSeparator(AWindow,options.time);
	if (message.body().isEmpty() && wasEncrypted)
		message.setBody(tr("[This message is OMEMO encrypted]"));

	// Note: deliberately no "message did not render -> append a raw copy"
	// fallback here.  Style handlers are allowed to transform the rendered
	// content (e.g. imageviewer replaces image URLs with thumbnails), so text
	// disappearing from toPlainText() must not produce a second copy.
	AWindow->viewWidget()->appendMessage(message,options);

	WindowStatus &wstatus = FWindowStatus[AWindow];
	if (!wstatus.lastMessageTime.isValid() || message.dateTime()>wstatus.lastMessageTime)
		wstatus.lastMessageTime = message.dateTime();

	wstatus.lastMessages.append(message);
	while (wstatus.lastMessages.count() > HISTORY_DUPLICATE_MESSAGES)
		wstatus.lastMessages.removeFirst();
}

bool ChatMessageHandler::isSelectionAccepted(const QList<IRosterIndex *> &ASelected) const
{
	foreach(IRosterIndex *index, ASelected)
	{
		if (!ChatHandlerRosterKinds.contains(index->kind()))
			return false;
	}
	return !ASelected.isEmpty();
}

QMap<Jid, QList<Jid> > ChatMessageHandler::getSortedAddresses(const QMultiMap<Jid,Jid> &AAddresses) const
{
	QMap<Jid, QList<Jid> > addresses;
	foreach(const Jid &streamJid, AAddresses.uniqueKeys())
	{
		QList<Jid> contacts = AAddresses.values(streamJid);
		IPresence *presence = FPresenceManager!=NULL ? FPresenceManager->findPresence(streamJid) : NULL;
		if (presence)
		{
			QList<IPresenceItem> pitemList;
			foreach(const Jid &contactJid, contacts)
			{
				IPresenceItem pitem = presence->findItem(contactJid);
				pitem.itemJid = contactJid;
				pitemList.append(pitem);
			}

			contacts.clear();
			pitemList = FPresenceManager->sortPresenceItems(pitemList);
			foreach(const IPresenceItem &pitem, pitemList)
				contacts.append(pitem.itemJid);
		}
		else
		{
			REPORT_ERROR("Failed to sort addresses: IPresence not found");
		}
		addresses[streamJid] = contacts;
	}
	return addresses;
}

void ChatMessageHandler::onWindowActivated()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender());
	if (window)
	{
		LOG_STRM_DEBUG(window->streamJid(),QString("Chat window activated, with=%1").arg(window->contactJid().bare()));
		saveLastChatWindow(window);
		if (FDestroyTimers.contains(window))
			delete FDestroyTimers.take(window);
		removeNotifiedMessages(window);
	}
}

void ChatMessageHandler::onWindowClosed()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender());
	if (window)
	{
		LOG_STRM_DEBUG(window->streamJid(),QString("Chat window closed, with=%1").arg(window->contactJid().bare()));

		// Forget the restore target only if this chat was not reopened by the
		// tab activation that follows the close (another tab may become active
		// first and save its own target).
		const Jid streamJid = window->streamJid();
		const Jid contactJid = window->contactJid();
		const bool userClose = FPluginManager==NULL || !FPluginManager->isShutingDown();
		QPointer<QObject> guard = window->instance();
		QTimer::singleShot(0,this,[this,streamJid,contactJid,guard,window,userClose]() {
			if (!guard.isNull() && window->isVisibleTabPage())
				return;
			clearLastChatWindow(streamJid,contactJid,userClose);
		});

		int destroyTimeout = Options::node(OPV_MESSAGES_CLEANCHATTIMEOUT).value().toInt();
		if (destroyTimeout>0 && !FNotifiedMessages.contains(window))
		{
			if (!FDestroyTimers.contains(window))
			{
				QTimer *timer = new QTimer;
				timer->setSingleShot(true);
				connect(timer,SIGNAL(timeout()),window->instance(),SLOT(deleteLater()));
				FDestroyTimers.insert(window,timer);
			}
			FDestroyTimers[window]->start(destroyTimeout*60*1000);
		}
	}
}

void ChatMessageHandler::onWindowDestroyed()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender());
	if (FWindows.contains(window))
	{
		LOG_STRM_INFO(window->streamJid(),QString("Chat window destroyed, with=%1").arg(window->contactJid().bare()));
		const Jid streamJid = window->streamJid();
		const Jid contactJid = window->contactJid();
		QTimer::singleShot(0,this,[this,streamJid,contactJid]() {
			clearLastChatWindow(streamJid,contactJid);
		});
		removeNotifiedMessages(window);

		if (FDestroyTimers.contains(window))
			delete FDestroyTimers.take(window);

		foreach(const QString &reqId, FHistoryRequests.keys(window))
			FHistoryRequests.remove(reqId);
		foreach(const QString &reqId, FCatchUpRequests.keys(window))
			FCatchUpRequests.remove(reqId);
		FHistoryMessages.remove(window);

		FWindows.removeAt(FWindows.indexOf(window));
		FWindowStatus.remove(window);
		FPendingMessages.remove(window);
		FPendingContent.remove(window);
	}
}

void ChatMessageHandler::onWindowAddressChanged()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender()->parent());
	if (window)
	{
		LOG_STRM_DEBUG(window->streamJid(),QString("Chat window address changed, with=%1").arg(window->contactJid().bare()));
		updateWindow(window);
	}
}

void ChatMessageHandler::onWindowAvailAddressesChanged()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender()->parent());
	if (window)
	{
		QMultiMap<Jid,Jid> addresses = window->address()->availAddresses();
		if (addresses.isEmpty())
		{
			LOG_STRM_DEBUG(window->streamJid(),QString("Destroying chat window due to avail addresses is empty, with=%1").arg(window->contactJid().bare()));
			window->instance()->deleteLater();
		}
		else if (!addresses.contains(window->streamJid(),window->contactJid()))
		{
			LOG_STRM_DEBUG(window->streamJid(),QString("Changing chat window address due to avail addresses changed, with=%1").arg(window->contactJid().bare()));
			QMap<Jid, QList<Jid> > sortedAddresses = getSortedAddresses(addresses);
			QMap<Jid, QList<Jid> >::const_iterator it = sortedAddresses.constBegin();
			window->address()->setAddress(it.key(),it.value().first());
		}
		else
		{
			LOG_STRM_DEBUG(window->streamJid(),QString("Chat window avail addresses changed, with=%1").arg(window->contactJid().bare()));
		}
	}
}

void ChatMessageHandler::onWindowAddressMenuRequested(Menu *AMenu)
{
	IMessageInfoWidget *widget = qobject_cast<IMessageInfoWidget *>(sender());
	if (widget)
	{
		QMap<Jid, QList<Jid> > addresses = getSortedAddresses(widget->messageWindow()->address()->availAddresses());

		int streamGroup = AG_MWIWAM_CHATMHANDLER_ADDRESSES-1;
		foreach(const Jid &streamJid, addresses.keys())
		{
			IAccount *account = FAccountManager!=NULL ? FAccountManager->findAccountByStream(streamJid) : NULL;
			QString accountName = account!=NULL ? account->name() : streamJid.uBare();

			streamGroup++;
			Action *accountAction = new Action(AMenu);
			accountAction->setText(QString("<%1>").arg(accountName));
			accountAction->setEnabled(false);
			QFont font = accountAction->font();
			font.setBold(true);
			accountAction->setFont(font);
			AMenu->addAction(accountAction,streamGroup);

			QActionGroup *addressGroup = new QActionGroup(AMenu);
			foreach(const Jid &contactJid, addresses.value(streamJid))
			{
				QString addressName = FMessageStyleManager!=NULL ? FMessageStyleManager->contactName(streamJid,contactJid) : contactJid.uBare();
				if (contactJid.hasResource() && addressName!=contactJid.resource())
					addressName += "/" + contactJid.resource();

				bool isCurAddress = streamJid==widget->messageWindow()->streamJid() && contactJid==widget->messageWindow()->contactJid();

				Action *addressAction = new Action(AMenu);
				addressAction->setCheckable(true);
				addressAction->setChecked(isCurAddress);
				addressAction->setText(addressName);
				addressAction->setActionGroup(addressGroup);
				addressAction->setData(ADR_STREAM_JID,streamJid.full());
				addressAction->setData(ADR_CONTACT_JID,contactJid.full());
				addressAction->setIcon(FStatusIcons!=NULL ? FStatusIcons->iconByJid(streamJid,contactJid) : QIcon());
				connect(addressAction,SIGNAL(triggered()),SLOT(onChangeWindowAddressAction()));
				AMenu->addAction(addressAction,streamGroup);
			}
		}
	}
}

void ChatMessageHandler::onWindowContextMenuRequested(Menu *AMenu)
{
	IMessageInfoWidget *widget = qobject_cast<IMessageInfoWidget *>(sender());
	if (widget && FRostersModel && FRostersView)
	{
		IRosterIndex *index = FRostersModel->findContactIndexes(widget->messageWindow()->streamJid(),widget->messageWindow()->contactJid()).value(0);
		if (index)
			FRostersView->contextMenuForIndex(QList<IRosterIndex *>()<<index,NULL,AMenu);
	}
}

void ChatMessageHandler::onWindowToolTipsRequested(QMap<int,QString> &AToolTips)
{
	IMessageInfoWidget *widget = qobject_cast<IMessageInfoWidget *>(sender());
	if (widget && FRostersModel && FRostersView)
	{
		IRosterIndex *index = FRostersModel->findContactIndexes(widget->messageWindow()->streamJid(),widget->messageWindow()->contactJid()).value(0);
		if (index)
			FRostersView->toolTipsForIndex(index,NULL,AToolTips);
	}
}

void ChatMessageHandler::onWindowNotifierActiveNotifyChanged(int ANotifyId)
{
	Q_UNUSED(ANotifyId);
	IMessageTabPageNotifier *notifier = qobject_cast<IMessageTabPageNotifier *>(sender());
	IMessageChatWindow *window = notifier!=NULL ? qobject_cast<IMessageChatWindow *>(notifier->tabPage()->instance()) : NULL;
	if (window)
		updateWindow(window);
}

void ChatMessageHandler::onWindowContentAppended(const QString &AHtml, const IMessageStyleContentOptions &AOptions)
{
	IMessageViewWidget *viewWidget = qobject_cast<IMessageViewWidget *>(sender());
	IMessageChatWindow *window = viewWidget!=NULL ? qobject_cast<IMessageChatWindow *>(viewWidget->messageWindow()->instance()) : NULL;
	if (window && FHistoryRequests.values().contains(window))
	{
		WindowContent content;
		content.html = AHtml;
		content.options = AOptions;
		FPendingContent[window].append(content);
		LOG_STRM_DEBUG(window->streamJid(),QString("Added pending content to chat window, with=%1").arg(window->contactJid().bare()));
	}
}

void ChatMessageHandler::onWindowMessageStyleOptionsChanged(const IMessageStyleOptions &AOptions, bool ACleared)
{
	Q_UNUSED(AOptions);
	IMessageViewWidget *viewWidget = qobject_cast<IMessageViewWidget *>(sender());
	IMessageChatWindow *window = viewWidget!=NULL ? qobject_cast<IMessageChatWindow *>(viewWidget->messageWindow()->instance()) : NULL;
	if (window)
	{
		if (ACleared)
			FWindowStatus[window].lastDateSeparator = QDate();
		LOG_STRM_DEBUG(window->streamJid(),QString("Chat window style options changed, with=%1, cleared=%2").arg(window->contactJid().bare()).arg(ACleared));
	}
}

void ChatMessageHandler::onStatusIconsChanged()
{
	foreach(IMessageChatWindow *window, FWindows)
		updateWindow(window);
}

void ChatMessageHandler::onAvatarChanged(const Jid &AContactJid)
{
	foreach(IMessageChatWindow *window, FWindows)
		if (window->contactJid().pBare() == AContactJid.pBare())
			updateWindow(window);
}

void ChatMessageHandler::onRosterItemReceived(IRoster *ARoster, const IRosterItem &AItem, const IRosterItem &ABefore)
{
	if (AItem.name!=ABefore.name || AItem.subscription!=ABefore.subscription)
	{
		IMessageChatWindow *window = findWindow(ARoster->streamJid(),AItem.itemJid);
		if (window)
			updateWindow(window);
	}
}

void ChatMessageHandler::onPresenceItemReceived(IPresence *APresence, const IPresenceItem &AItem, const IPresenceItem &ABefore)
{
	if (AItem.show!=ABefore.show || AItem.status!=ABefore.status)
	{
		IMessageChatWindow *window = findWindow(APresence->streamJid(),AItem.itemJid);
		if (window)
		{
			if (Options::node(OPV_MESSAGES_SHOWSTATUS).value().toBool())
			{
				QString show = FStatusChanger ? FStatusChanger->nameByShow(AItem.show) : QString();
				QString name = FMessageStyleManager!=NULL ? FMessageStyleManager->contactName(APresence->streamJid(),AItem.itemJid) : AItem.itemJid.uBare();
				if (AItem.itemJid.hasResource() && name!=AItem.itemJid.resource())
					name += "/" + AItem.itemJid.resource();
				QString message = tr("%1 changed status to [%2] %3").arg(name,show,AItem.status);
				showStyledStatus(window,message);
			}
			updateWindow(window);
		}
	}
}

void ChatMessageHandler::onShowWindowAction(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	if (action)
		showWindow(action->data(ADR_STREAM_JID).toString(), action->data(ADR_CONTACT_JID).toString());
}

void ChatMessageHandler::onClearWindowAction(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	IMessageChatWindow *window = action!=NULL ? qobject_cast<IMessageChatWindow *>(action->parent()) : NULL;
	if (window)
		window->viewWidget()->clearContent();
}

void ChatMessageHandler::onChangeWindowAddressAction()
{
	Action *action = qobject_cast<Action *>(sender());
	if (action)
	{
		Jid streamJid = action->data(ADR_STREAM_JID).toString();
		Jid contactJid = action->data(ADR_CONTACT_JID).toString();
		IMessageChatWindow *window = findWindow(streamJid,contactJid);
		if (window)
		{
			LOG_STRM_DEBUG(window->streamJid(),QString("Changing chat window address by action, with=%1").arg(window->contactJid().bare()));
			window->address()->setAddress(streamJid,contactJid);
		}
	}
}

void ChatMessageHandler::onActiveStreamRemoved(const Jid &AStreamJid)
{
	foreach(IMessageChatWindow *window, FWindows)
		window->address()->removeAddress(AStreamJid);
}

void ChatMessageHandler::onShortcutActivated(const QString &AId, QWidget *AWidget)
{
	if (FRostersView && AWidget==FRostersView->instance())
	{
		QList<IRosterIndex *> indexes = FRostersView->selectedRosterIndexes();
		if (AId==SCT_ROSTERVIEW_SHOWCHATDIALOG && indexes.count()==1)
		{
			// a recent contact (offline ones included) is not a roster contact
			// kind, the click path resolves it to the contact the same way
			IRosterIndex *index = indexes.first();
			if (index->kind()==RIK_RECENT_ITEM && index->data(RDR_RECENT_TYPE).toString()==REIT_CONTACT)
				showWindow(index->data(RDR_STREAM_JID).toString(),index->data(RDR_RECENT_REFERENCE).toString());
			else if (isSelectionAccepted(indexes))
				showWindow(index->data(RDR_STREAM_JID).toString(),index->data(RDR_FULL_JID).toString());
		}
	}
}

void ChatMessageHandler::onArchivePrefsOpened(const Jid &AStreamJid)
{
	// The archive is always re-opened when the stream is (re)connected. Query
	// the server archive afterwards so that the messages received while the
	// connection was down are appended to the already opened chat windows.
	FCatchUpStreams.insert(AStreamJid);
	FCatchUpTimer.start(HISTORY_CATCHUP_TIMEOUT);
}

void ChatMessageHandler::onArchivePrefsClosed(const Jid &AStreamJid)
{
	FCatchUpStreams.remove(AStreamJid);
}

void ChatMessageHandler::onCatchUpTimerTimeout()
{
	QSet<Jid> streams = FCatchUpStreams;
	FCatchUpStreams.clear();
	foreach(const Jid &streamJid, streams)
	{
		foreach(IMessageChatWindow *window, FWindows)
		{
			if (window->streamJid()==streamJid)
				requestCatchUpHistory(window);
		}
	}
}

void ChatMessageHandler::onArchiveRequestFailed(const QString &AId, const XmppError &AError)
{
	if (FCatchUpRequests.contains(AId))
	{
		IMessageChatWindow *window = FCatchUpRequests.take(AId);
		LOG_STRM_WARNING(window->streamJid(),QString("Failed to load chat history catch-up, id=%1: %2").arg(AId,AError.condition()));
	}
	else if (FHistoryRequests.contains(AId))
	{
		IMessageChatWindow *window = FHistoryRequests.take(AId);
		LOG_STRM_WARNING(window->streamJid(),QString("Failed to load chat history, id=%1: %2").arg(AId,AError.condition()));
		showHistory(window);
		showStyledStatus(window,tr("Failed to load history: %1").arg(AError.errorMessage()),true);
	}
}

void ChatMessageHandler::onArchiveMessagesLoaded(const QString &AId, const IArchiveCollectionBody &ABody)
{
	if (FCatchUpRequests.contains(AId))
	{
		IMessageChatWindow *window = FCatchUpRequests.take(AId);
		LOG_STRM_INFO(window->streamJid(),QString("Chat history catch-up loaded, with=%1, id=%2").arg(window->contactJid().bare(),AId));
		appendHistoryMessages(window,ABody.messages);
	}
	else if (FHistoryRequests.contains(AId))
	{
		IMessageChatWindow *window = FHistoryRequests.take(AId);
		LOG_STRM_INFO(window->streamJid(),QString("Chat history loaded, id=%1").arg(AId));

		FHistoryMessages[window].messages += ABody.messages;
		FHistoryMessages[window].notes.unite(ABody.notes);
		showHistory(window);
	}
}

void ChatMessageHandler::onRostersViewIndexContextMenu(const QList<IRosterIndex *> &AIndexes, quint32 ALabelId, Menu *AMenu)
{
	if (ALabelId==AdvancedDelegateItem::DisplayId && isSelectionAccepted(AIndexes) && AIndexes.count()==1)
	{
		Action *action = new Action(AMenu);
		action->setText(tr("Open chat dialog"));
		action->setIcon(RSR_STORAGE_MENUICONS,MNI_CHATMHANDLER_MESSAGE);
		action->setData(ADR_STREAM_JID,AIndexes.first()->data(RDR_STREAM_JID));
		action->setData(ADR_CONTACT_JID,AIndexes.first()->data(RDR_FULL_JID));
		action->setShortcutId(SCT_ROSTERVIEW_SHOWCHATDIALOG);
		AMenu->addAction(action,AG_RVCM_CHATMHANDLER_OPEN,true);
		connect(action,SIGNAL(triggered(bool)),SLOT(onShowWindowAction(bool)));
	}
}

void ChatMessageHandler::onStyleOptionsChanged(const IMessageStyleOptions &AOptions, int AMessageType, const QString &AContext)
{
	if (AMessageType==Message::Chat && AContext.isEmpty())
	{
		foreach (IMessageChatWindow *window, FWindows)
		{
			IMessageStyle *style = window->viewWidget()!=NULL ? window->viewWidget()->messageStyle() : NULL;
			if (style==NULL || !style->changeOptions(window->viewWidget()->styleWidget(),AOptions,false))
			{
				setMessageStyle(window);
				requestHistory(window);
			}
		}
	}
}
