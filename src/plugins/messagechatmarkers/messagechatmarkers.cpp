#include "messagechatmarkers.h"

#include <definitions/namespaces.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
#include <definitions/optionnodes.h>
#include <definitions/optionvalues.h>
#include <definitions/optionwidgetorders.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QFont>
#include <QColor>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

// XEP-0333 Chat Markers for one-to-one chats.
//
// Outgoing chat messages are marked with <markable xmlns='urn:xmpp:chat-markers:0'/>
// and the peer answers with <displayed id='...'/> once the message has been
// read. The reverse direction works the same way: a received message carrying
// <markable/> is acknowledged with a <displayed/> marker as soon as the chat
// window is actually shown to the user.
//
// The state of an outgoing message is bound to the message in the style
// document with an invisible per-message token (a different zero-width
// sequence than the one used by the message receipts plugin). Once a chat
// marker is received all messages up to the referenced one are marked as
// displayed, so the marker semantics stay cumulative like the spec requires.

// Builds an invisible tag which binds a chat marker to a given stanza id.
static QString messageMarkerToken(const QString &AMessageId)
{
	quint32 hash = qHash(AMessageId);
	QString token;
	static const ushort keys[] = {0x2063,0x2062,0x2061,0x2060,0x200D,0x200C,0x200B};
	for (int i=0; i<6; i++)
	{
		token += QChar(keys[hash & 0x07]);
		hash >>= 3;
	}
	return token;
}

// Appends an invisible marker token to the end of the last non-empty text
// block of the style document, so a displayed marker can be patched into the
// exact message later.
static bool appendMessageMarkerToken(QTextBrowser *ABrowser, const QString &AMessageId)
{
	if (ABrowser == NULL || AMessageId.isEmpty())
		return false;

	QTextDocument *document = ABrowser->document();
	QTextBlock block = document->lastBlock();
	while (block.isValid() && block.length()<=1 && block.previous().isValid())
		block = block.previous(); // skip blocks created by a trailing <br>
	if (!block.isValid() || block.length()<=1)
		return false;

	QTextCursor cursor(block);
	cursor.movePosition(QTextCursor::EndOfBlock);
	cursor.insertText(messageMarkerToken(AMessageId));
	return true;
}

MessageChatMarkers::MessageChatMarkers()
{
	FPluginManager = NULL;
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FMessageProcessor = NULL;
	FMessageWidgets = NULL;
	FOptionsManager = NULL;
	FMarkerRecordsLoaded = false;

	FSaveTimer.setSingleShot(true);
	connect(&FSaveTimer,&QTimer::timeout,this,[this]() { saveMarkerRecords(); });
}

MessageChatMarkers::~MessageChatMarkers()
{
	if (FSaveTimer.isActive() && FMarkerRecordsLoaded)
		saveMarkerRecords();
	if (FMessageWidgets)
		FMessageWidgets->removeStyleHandler(MSHO_MESSAGECHATMARKERS,this);
	if (FOptionsManager)
		FOptionsManager->removeOptionsDialogHolder(this);
}

void MessageChatMarkers::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Chat Markers");
	APluginInfo->description = tr("Sends and receives message display notifications (XEP-0333)");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->dependences.append(QUuid(OPTIONSMANAGER_UUID));
}

bool MessageChatMarkers::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0,NULL);
	if (plugin)
	{
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0,NULL);
	if (plugin)
	{
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IMessageProcessor").value(0,NULL);
	if (plugin)
	{
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IMessageWidgets").value(0,NULL);
	if (plugin)
	{
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IOptionsManager").value(0,NULL);
	if (plugin)
	{
		FOptionsManager = qobject_cast<IOptionsManager *>(plugin->instance());
	}

	return FStanzaProcessor!=NULL && FMessageProcessor!=NULL && FMessageWidgets!=NULL;
}

bool MessageChatMarkers::initObjects()
{
	if (FDiscovery)
	{
		IDiscoFeature dfeature;
		dfeature.var = NS_MESSAGE_MARKERS;
		dfeature.active = true;
		dfeature.name = tr("Chat Markers");
		dfeature.description = tr("Supports the exchanging of the information about the displayed messages");
		FDiscovery->insertDiscoFeature(dfeature);
	}
	if (FMessageProcessor)
	{
		FMessageProcessor->insertMessageEditor(MEO_DEFAULT,this);
	}
	if (FMessageWidgets)
	{
		FMessageWidgets->insertStyleHandler(MSHO_MESSAGECHATMARKERS,this);
		connect(FMessageWidgets->instance(),SIGNAL(chatWindowCreated(IMessageChatWindow *)),SLOT(onChatWindowCreated(IMessageChatWindow *)));
		connect(FMessageWidgets->instance(),SIGNAL(tabWindowCreated(IMessageTabWindow *)),SLOT(onTabWindowCreated(IMessageTabWindow *)));
		foreach(IMessageTabWindow *tabWindow, FMessageWidgets->tabWindows())
			onTabWindowCreated(tabWindow);
	}
	return true;
}

bool MessageChatMarkers::initSettings()
{
	Options::setDefaultValue(OPV_MESSAGES_CHATMARKERSENABLED,true);

	if (FOptionsManager)
	{
		FOptionsManager->insertOptionsDialogHolder(this);
	}

	return true;
}

QMultiMap<int, IOptionsDialogWidget *> MessageChatMarkers::optionsDialogWidgets(const QString &ANodeId, QWidget *AParent)
{
	QMultiMap<int, IOptionsDialogWidget *> widgets;
	if (FOptionsManager && ANodeId==OPN_MESSAGES)
	{
		widgets.insert(OWO_MESSAGES_CHATMARKERSENABLED, FOptionsManager->newOptionsDialogWidget(Options::node(OPV_MESSAGES_CHATMARKERSENABLED),tr("Send read receipts when messages are displayed"),AParent));
	}
	return widgets;
}

bool MessageChatMarkers::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder);

	if (ADirection == IMessageProcessor::DirectionOut)
	{
		if (isOutgoingMarkable(AStreamJid,AMessage))
		{
			// A chat marker is always for a stanza id, so one has to be present.
			if (AMessage.id().isEmpty())
				AMessage.setRandomId();
			AMessage.stanza().addElement("markable",NS_MESSAGE_MARKERS);
			LOG_STRM_DEBUG(AStreamJid,QString("Chat marker requested, to=%1, id=%2").arg(AMessage.to(),AMessage.id()));
			emit messageMarkableRequested(AStreamJid,AMessage);
			setMarkerRecord(AStreamJid,AMessage.toJid(),AMessage.id(),AMessage.dateTime(),MarkerRequested);
			enqueueMarkerToken(AStreamJid,AMessage.id(),AMessage.toJid());
		}
	}
	else
	{
		QDomElement displayedElem = AMessage.stanza().firstElement("displayed",NS_MESSAGE_MARKERS);
		if (!displayedElem.isNull())
		{
			if (isIncomingDisplayed(AStreamJid,AMessage))
			{
				QString messageId = displayedElem.attribute("id");
				emit messageDisplayedReceived(AStreamJid,messageId,AMessage.fromJid());
				showDisplayedMarker(AStreamJid,messageId,AMessage.fromJid());
			}
		}
		else if (isIncomingMarkable(AStreamJid,AMessage))
		{
			LOG_STRM_DEBUG(AStreamJid,QString("Chat marker pending, from=%1, id=%2").arg(AMessage.from(),AMessage.id()));
			storePendingMarker(AStreamJid,AMessage);
		}
	}
	return false;
}

bool MessageChatMarkers::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AHtml);
	Q_UNUSED(AModifiedHtml);

	if (AOrder != MSHO_MESSAGECHATMARKERS)
		return false;

	if (AWidget == NULL)
		return true;

	// History messages are restored from the saved displayed state. The
	// invisible token is appended to the message html so the deferred patch
	// can find the exact message even if the whole history is rendered at once.
	if (AOptions.kind == IMessageStyleContentOptions::KindMessage
		&& AOptions.direction == IMessageStyleContentOptions::DirectionOut
		&& (AOptions.type & IMessageStyleContentOptions::TypeHistory))
	{
		if (!AOptions.messageId.isEmpty() && isEnabled() && isMessageDisplayed(AOptions.messageId)
			&& qobject_cast<QTextBrowser *>(AWidget)!=NULL)
		{
			AModifiedHtml += messageMarkerToken(AOptions.messageId);
			QPointer<QWidget> widget = AWidget;
			QString messageId = AOptions.messageId;
			QTimer::singleShot(0,this,[this,widget,messageId]() {
				QTextBrowser *browser = qobject_cast<QTextBrowser *>(widget.data());
				if (browser != NULL)
					patchDisplayedMarker(browser,messageId);
			});
		}
		return true;
	}

	// The message itself is not rendered yet, so schedule a deferred insertion
	// of the pending marker tokens into the style document.
	if (AOptions.kind == IMessageStyleContentOptions::KindMessage
		&& AOptions.direction == IMessageStyleContentOptions::DirectionOut
		&& !(AOptions.type & IMessageStyleContentOptions::TypeHistory))
	{
		MarkerTarget *target = findMarkerTarget(AWidget);
		if (target != NULL && !target->flushing && !target->pendingIds.isEmpty())
		{
			target->flushing = true;
			QPointer<QWidget> widget = AWidget;
			QTimer::singleShot(0,this,[this,widget]() {
				if (widget != NULL)
				{
					MarkerTarget *target = findMarkerTarget(widget);
					if (target != NULL)
						flushMarkerTarget(target);
				}
			});
		}
	}

	// The chat window is being rendered, so messages which are visible now
	// can be acknowledged as displayed to the sender.
	IMessageChatWindow *window = findChatWindow(AWidget);
	if (window != NULL && FPendingMarkers.contains(chatKey(window->streamJid(),window->contactJid())))
	{
		QPointer<QWidget> page = window->instance();
		QTimer::singleShot(0,this,[this,page]() {
			if (page != NULL)
			{
				IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(page.data());
				if (window != NULL)
					flushDisplayedMarkers(window);
			}
		});
	}
	return true;
}

bool MessageChatMarkers::isEnabled() const
{
	return Options::node(OPV_MESSAGES_CHATMARKERSENABLED).value().toBool();
}

bool MessageChatMarkers::isOutgoingMarkable(const Jid &AStreamJid, const Message &AMessage) const
{
	// Only one-to-one chat messages with visible content request a marker.
	if (!isEnabled() || AMessage.type()!=Message::Chat || AMessage.body().isEmpty())
		return false;

	// Carbon copies and chat markers themselves never request anything again.
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_RECEIPTS).isNull())
		return false;

	if (!AMessage.stanza().firstElement("markable",NS_MESSAGE_MARKERS).isNull()
		|| !AMessage.stanza().firstElement("displayed",NS_MESSAGE_MARKERS).isNull())
		return false;

	// Do not bother the peers which are known not to support the feature.
	return isSupportedByContact(AStreamJid,AMessage.toJid());
}

bool MessageChatMarkers::isIncomingMarkable(const Jid &AStreamJid, const Message &AMessage) const
{
	// Only one-to-one chat messages with visible content and an id can be
	// acknowledged with a chat marker.
	if (!isEnabled() || AMessage.type()!=Message::Chat || AMessage.body().isEmpty())
		return false;

	if (AMessage.id().isEmpty())
		return false;

	// Markers are never sent for our own messages or carbon copies.
	if (AStreamJid.isValid() && AMessage.fromJid().pBare()==AStreamJid.pBare())
		return false;
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull())
		return false;

	// Only messages which explicitly ask for a displayed marker are tracked.
	return !AMessage.stanza().firstElement("markable",NS_MESSAGE_MARKERS).isNull();
}

bool MessageChatMarkers::isIncomingDisplayed(const Jid &AStreamJid, const Message &AMessage) const
{
	// Groupchat markers are not supported yet.
	if (AMessage.type()==Message::GroupChat || AMessage.type()==Message::Error)
		return false;

	// Markers from our own resources are not read confirmations.
	if (AStreamJid.isValid() && AMessage.fromJid().pBare()==AStreamJid.pBare())
		return false;
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull())
		return false;

	return !AMessage.stanza().firstElement("displayed",NS_MESSAGE_MARKERS).attribute("id").isEmpty();
}

bool MessageChatMarkers::isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const
{
	if (FDiscovery && AContactJid.isValid())
	{
		IDiscoInfo info = FDiscovery->discoInfo(AStreamJid,AContactJid);
		if (!info.features.isEmpty())
			return info.features.contains(NS_MESSAGE_MARKERS);

		// The capabilities are not known yet: ask once and allow the first
		// message, the following ones will be limited by the disco info.
		QString key = AStreamJid.pBare()+"\n"+AContactJid.pBare();
		if (!FRequestedDisco.contains(key))
		{
			FRequestedDisco.insert(key);
			FDiscovery->requestDiscoInfo(AStreamJid,AContactJid);
		}
	}
	return true;
}

void MessageChatMarkers::sendDisplayedMarker(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId)
{
	if (FStanzaProcessor==NULL || AMessageId.isEmpty())
		return;

	Stanza marker(STANZA_KIND_MESSAGE);
	marker.setUniqueId().setTo(AContactJid.full());
	marker.addElement("displayed",NS_MESSAGE_MARKERS).setAttribute("id",AMessageId);
	// Sent directly, so the marker is not duplicated or displayed anywhere.
	if (FStanzaProcessor->sendStanzaOut(AStreamJid,marker))
	{
		LOG_STRM_DEBUG(AStreamJid,QString("Chat marker sent, to=%1, id=%2").arg(AContactJid.full(),AMessageId));
		emit messageDisplayedSent(AStreamJid,AMessageId,AContactJid);
	}
	else
	{
		LOG_STRM_WARNING(AStreamJid,QString("Failed to send chat marker, to=%1, id=%2").arg(AContactJid.full(),AMessageId));
	}
}

void MessageChatMarkers::showDisplayedMarker(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid)
{
	if (!isEnabled() || AMessageId.isEmpty())
		return;

	// The displayed state is saved even when the window is not open or the
	// referenced message is not in the current style document, so it can be
	// restored once the history is shown again.
	confirmDisplayedMarkers(AStreamJid,AContactJid,AMessageId);

	IMessageChatWindow *window = FMessageWidgets!=NULL ? FMessageWidgets->findChatWindow(AStreamJid,AContactJid,false) : NULL;
	if (window == NULL || window->viewWidget()==NULL)
	{
		LOG_STRM_INFO(AStreamJid,QString("Chat marker received with no chat window open, from=%1, id=%2").arg(AContactJid.full(),AMessageId));
		return;
	}

	QTextBrowser *browser = qobject_cast<QTextBrowser *>(window->viewWidget()->styleWidget());
	if (browser == NULL)
	{
		// Not a document-based style (e.g. a web view engine): display the
		// confirmation as a plain status line.
		IMessageStyleContentOptions options;
		options.kind = IMessageStyleContentOptions::KindStatus;
		options.direction = IMessageStyleContentOptions::DirectionOut;
		options.time = QDateTime::currentDateTime();
		QString html = QString("<span style=\"color:#1e8e3e;font-weight:bold;\">&#10004;&#10004;</span>&nbsp;<span>%1</span>")
			.arg(tr("Displayed").toHtmlEscaped());
		window->viewWidget()->appendHtml(html,options);
		return;
	}

	MarkerTarget *target = findMarkerTarget(browser);
	if (target == NULL)
	{
		// TEMP DIAGNOSTIC
		LOG_STRM_INFO(AStreamJid,QString("[MessageChatMarkers] marker target not found for the browser, id=%1").arg(AMessageId));
		return;
	}

	// Displayed markers are cumulative: everything up to the referenced
	// message is considered displayed. An id which is not known or which is
	// older than the last acknowledged one is ignored (XEP-0333 §5).
	int index = target->sentIds.indexOf(AMessageId);
	if (index < 0)
	{
		// TEMP DIAGNOSTIC
		LOG_STRM_INFO(AStreamJid,QString("[MessageChatMarkers] marker references unknown or already displayed message, id=%1, sentIds=%2").arg(AMessageId,target->sentIds.join(";")));
		return;
	}

	for (int i=0; i<=index; i++)
	{
		QString messageId = target->sentIds.takeAt(0);
		QTextCursor found = browser->document()->find(messageMarkerToken(messageId));
		if (found.isNull())
		{
			// TEMP DIAGNOSTIC
			LOG_STRM_INFO(AStreamJid,QString("[MessageChatMarkers] marker token not found in the document, id=%1").arg(messageId));
			continue;
		}

		found.removeSelectedText();
		QTextCharFormat format;
		format.setForeground(QColor(30,142,61));
		format.setFontWeight(QFont::Bold);
		// Placed right after the delivery check of the receipts plugin so
		// both marks read as a single "double check" pair.
		found.insertText(QChar(0x2714),format);
	}
}

void MessageChatMarkers::storePendingMarker(const Jid &AStreamJid, const Message &AMessage)
{
	PendingMarker marker;
	marker.streamJid = AStreamJid;
	marker.contactJid = AMessage.fromJid();
	marker.messageId = AMessage.id();
	// Only the most recent request per chat is relevant: the marker carries
	// the semantic of "all messages up to this point".
	FPendingMarkers.insert(chatKey(AStreamJid,AMessage.fromJid()),marker);
}

void MessageChatMarkers::flushDisplayedMarkers(IMessageChatWindow *AWindow)
{
	if (AWindow==NULL || !isEnabled())
		return;

	// The marker is only sent when the chat is really shown to the user.
	if (!isChatWindowDisplayed(AWindow))
		return;

	PendingMarker marker = FPendingMarkers.take(chatKey(AWindow->streamJid(),AWindow->contactJid()));
	if (!marker.messageId.isEmpty())
		sendDisplayedMarker(marker.streamJid,marker.contactJid,marker.messageId);
}

bool MessageChatMarkers::isChatWindowDisplayed(IMessageChatWindow *AWindow) const
{
	QWidget *page = AWindow!=NULL ? AWindow->instance() : NULL;
	if (page==NULL || !page->isVisible())
		return false;

	// The page is either a window itself or the current tab of its parent.
	QWidget *top = page->window();
	return top!=NULL && top->isVisible() && !top->isMinimized();
}

IMessageChatWindow *MessageChatMarkers::findChatWindow(QWidget *AStyleWidget) const
{
	if (FMessageWidgets!=NULL && AStyleWidget!=NULL)
	{
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
		{
			if (window->viewWidget()!=NULL && window->viewWidget()->styleWidget()==AStyleWidget)
				return window;
		}
	}
	return NULL;
}

MessageChatMarkers::MarkerTarget *MessageChatMarkers::findMarkerTarget(QWidget *AWidget)
{
	for (QList<MarkerTarget>::iterator it=FMarkerTargets.begin(); it!=FMarkerTargets.end(); ++it)
	{
		if (it->widget == AWidget)
			return &(*it);
	}
	return NULL;
}

void MessageChatMarkers::enqueueMarkerToken(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid)
{
	IMessageChatWindow *window = FMessageWidgets!=NULL ? FMessageWidgets->findChatWindow(AStreamJid,AContactJid,false) : NULL;
	QWidget *styleWidget = window!=NULL && window->viewWidget()!=NULL ? window->viewWidget()->styleWidget() : NULL;
	if (styleWidget == NULL || AMessageId.isEmpty())
		return;

	MarkerTarget *target = findMarkerTarget(styleWidget);
	if (target == NULL)
	{
		MarkerTarget newTarget;
		newTarget.widget = QPointer<QWidget>(styleWidget);
		newTarget.flushing = false;
		FMarkerTargets.append(newTarget);
		target = &FMarkerTargets.last();
	}
	target->pendingIds.append(AMessageId);

	pruneMarkerTargets();
}

void MessageChatMarkers::flushMarkerTarget(MarkerTarget *ATarget)
{
	ATarget->flushing = false;

	QTextBrowser *browser = ATarget->widget!=NULL ? qobject_cast<QTextBrowser *>(ATarget->widget) : NULL;
	while (!ATarget->pendingIds.isEmpty())
	{
		QString messageId = ATarget->pendingIds.takeFirst();
		bool inserted = browser!=NULL ? appendMessageMarkerToken(browser,messageId) : false;
		if (inserted)
			ATarget->sentIds.append(messageId);
		// TEMP DIAGNOSTIC
		LOG_INFO(QString("[MessageChatMarkers] marker token inserted, id=%1, ok=%2").arg(messageId).arg(inserted));
	}

	// Drop targets for widgets which are gone.
	pruneMarkerTargets();
}

void MessageChatMarkers::pruneMarkerTargets()
{
	QList<MarkerTarget> targets;
	foreach(const MarkerTarget &item, FMarkerTargets)
	{
		if (!item.widget.isNull())
			targets.append(item);
	}
	FMarkerTargets = targets;
}

void MessageChatMarkers::onChatWindowCreated(IMessageChatWindow *AWindow)
{
	connect(AWindow->instance(),SIGNAL(tabPageActivated()),SLOT(onChatWindowActivated()));
}

void MessageChatMarkers::onChatWindowActivated()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender());
	if (window != NULL)
		flushDisplayedMarkers(window);
}

void MessageChatMarkers::onTabWindowCreated(IMessageTabWindow *AWindow)
{
	connect(AWindow->instance(),SIGNAL(currentTabPageChanged(IMessageTabPage *)),SLOT(onCurrentTabPageChanged(IMessageTabPage *)));
}

void MessageChatMarkers::onCurrentTabPageChanged(IMessageTabPage *APage)
{
	IMessageChatWindow *window = APage!=NULL ? qobject_cast<IMessageChatWindow *>(APage->instance()) : NULL;
	if (window != NULL && FPendingMarkers.contains(chatKey(window->streamJid(),window->contactJid())))
		flushDisplayedMarkers(window);
}

QString MessageChatMarkers::chatKey(const Jid &AStreamJid, const Jid &AContactJid)
{
	return AStreamJid.pBare()+"\n"+AContactJid.pBare();
}

void MessageChatMarkers::patchDisplayedMarker(QTextBrowser *ABrowser, const QString &AMessageId) const
{
	if (ABrowser==NULL || AMessageId.isEmpty())
		return;

	QTextCursor found = ABrowser->document()->find(messageMarkerToken(AMessageId));
	if (found.isNull())
	{
		LOG_DEBUG(QString("Failed to find historical chat marker in style document, id=%1").arg(AMessageId));
		return;
	}

	QTextCharFormat format;
	format.setForeground(QColor(30,142,61));
	format.setFontWeight(QFont::Bold);
	// Placed right after the delivery check of the receipts plugin so both
	// marks read as a single "double check" pair.
	found.insertText(QChar(0x2714),format);
}

QString MessageChatMarkers::markerStateFilePath() const
{
	QString dir = Options::filesPath();
	if (dir.isEmpty() && FPluginManager!=NULL)
		dir = FPluginManager->homePath();
	return dir.isEmpty() ? QString() : dir + "/messagechatmarkers.state";
}

void MessageChatMarkers::loadMarkerRecords()
{
	if (FMarkerRecordsLoaded)
		return;
	FMarkerRecordsLoaded = true;

	QFile file(markerStateFilePath());
	if (!file.open(QFile::ReadOnly))
		return;

	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(file.readAll(),&error);
	if (error.error!=QJsonParseError::NoError || !doc.isObject())
	{
		LOG_WARNING(QString("Failed to parse chat markers state: %1").arg(error.errorString()));
		return;
	}

	foreach(const QJsonValue &value, doc.object().value("messages").toArray())
	{
		QJsonObject object = value.toObject();
		QString messageId = object.value("id").toString();
		if (messageId.isEmpty())
			continue;
		MarkerRecord record;
		record.chatKey = object.value("chat").toString();
		record.time = (qint64)object.value("time").toDouble();
		record.state = object.value("state").toInt();
		FMarkerRecords.insert(messageId,record);
	}
}

void MessageChatMarkers::scheduleMarkerRecordsSave()
{
	// Debounce the write: several markers may arrive in a burst and the whole
	// state is serialized on every change.
	if (!FSaveTimer.isActive())
		FSaveTimer.start(2000);
}

void MessageChatMarkers::saveMarkerRecords() const
{
	QString filePath = markerStateFilePath();
	if (filePath.isEmpty())
		return;

	QJsonArray messages;
	for (QHash<QString, MarkerRecord>::const_iterator it=FMarkerRecords.constBegin(); it!=FMarkerRecords.constEnd(); ++it)
	{
		QJsonObject object;
		object.insert("id",it.key());
		object.insert("chat",it->chatKey);
		object.insert("time",(double)it->time);
		object.insert("state",it->state);
		messages.append(object);
	}

	QJsonObject root;
	root.insert("version",1);
	root.insert("messages",messages);

	QSaveFile file(filePath);
	if (file.open(QFile::WriteOnly))
	{
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
		if (!file.commit())
			LOG_WARNING(QString("Failed to save chat markers state to file=%1: %2").arg(filePath,file.errorString()));
	}
	else
	{
		LOG_WARNING(QString("Failed to open chat markers state file=%1: %2").arg(filePath,file.errorString()));
	}
}

void MessageChatMarkers::pruneMarkerRecords()
{
	static const int MaxRecords = 1000;
	if (FMarkerRecords.count() <= MaxRecords)
		return;

	QList<QPair<qint64, QString> > sorted;
	for (QHash<QString, MarkerRecord>::const_iterator it=FMarkerRecords.constBegin(); it!=FMarkerRecords.constEnd(); ++it)
		sorted.append(qMakePair(it->time,it.key()));
	std::sort(sorted.begin(),sorted.end());

	int removeCount = FMarkerRecords.count()-MaxRecords;
	for (int i=0; i<removeCount; i++)
		FMarkerRecords.remove(sorted.at(i).second);
}

void MessageChatMarkers::setMarkerRecord(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId, const QDateTime &ATime, int AState)
{
	if (AMessageId.isEmpty())
		return;

	loadMarkerRecords();

	MarkerRecord &record = FMarkerRecords[AMessageId];
	record.chatKey = chatKey(AStreamJid,AContactJid);
	if (record.time <= 0)
		record.time = ATime.isValid() ? ATime.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
	record.state = AState;

	pruneMarkerRecords();
	scheduleMarkerRecordsSave();
}

bool MessageChatMarkers::isMessageDisplayed(const QString &AMessageId)
{
	if (AMessageId.isEmpty())
		return false;

	loadMarkerRecords();

	QHash<QString, MarkerRecord>::const_iterator it = FMarkerRecords.constFind(AMessageId);
	return it!=FMarkerRecords.constEnd() && it->state==MarkerDisplayed;
}

void MessageChatMarkers::confirmDisplayedMarkers(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId)
{
	if (AMessageId.isEmpty())
		return;

	loadMarkerRecords();

	QString key = chatKey(AStreamJid,AContactJid);
	QHash<QString, MarkerRecord>::const_iterator target = FMarkerRecords.constFind(AMessageId);
	qint64 targetTime = target!=FMarkerRecords.constEnd() ? target->time : 0;
	bool changed = false;

	if (targetTime > 0)
	{
		// Displayed markers are cumulative: everything sent to this chat not
		// later than the referenced message is considered displayed.
		for (QHash<QString, MarkerRecord>::iterator it=FMarkerRecords.begin(); it!=FMarkerRecords.end(); ++it)
		{
			if (it->chatKey==key && it->time<=targetTime && it->state!=MarkerDisplayed)
			{
				it->state = MarkerDisplayed;
				changed = true;
			}
		}
	}
	else
	{
		MarkerRecord record;
		record.chatKey = key;
		record.time = QDateTime::currentSecsSinceEpoch();
		record.state = MarkerDisplayed;
		FMarkerRecords.insert(AMessageId,record);
		changed = true;
	}

	if (changed)
	{
		pruneMarkerRecords();
		scheduleMarkerRecordsSave();
	}
}
