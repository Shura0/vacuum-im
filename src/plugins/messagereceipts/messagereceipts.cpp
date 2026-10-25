#include "messagereceipts.h"

#include <definitions/namespaces.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
#include <definitions/messagemarks.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextOption>
#include <QFontMetricsF>
#include <QColor>
#include <QFont>
#include <QTimer>
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

// XEP-0184 Message Delivery Receipts.
//
// Outgoing chat messages get <request xmlns='urn:xmpp:receipts'/> and the
// peer confirms the delivery with a <received/> message. Inbound messages
// with a request are answered right from the message editor hook, so the
// reply does not go through the whole processing pipeline again.
//
// Delivery state is displayed as a mark placed by the message style template
// through the %contentSlot:marks% slot: gray check while waiting, green when a
// confirmation arrives. The mark carries a per-message locator (invisible
// zero-width characters) so a receipt is bound to the exact message in the
// style document. The mark is right aligned by a tab stop which is applied by
// a deferred pass, after the content is inserted into the style document.

// Invisible locator which binds a receipt mark to a given stanza id.
static QString receiptLocator(const QString &AMessageId)
{
	quint32 hash = qHash(AMessageId);
	QString token;
	static const ushort keys[] = {0x200B,0x200C,0x200D,0x2060,0x2061,0x2062,0x2063};
	const int keyCount = sizeof(keys)/sizeof(keys[0]);
	for (int i=0; i<6; i++)
	{
		token += QChar(keys[hash % keyCount]);
		hash >>= 3;
	}
	return token;
}

// Marker text used both for searching in the document and for the replacement
// after a receipt arrives. The check mark itself is visible, the locator binds
// it to the stanza id. Note: Qt normalizes no-break spaces of the html parser
// to a plain space, so the separator is added by the alignment pass instead.
static QString receiptToken(const QString &AMessageId, ushort AGlyph=0x2713)
{
	QString token;
	token += QChar(AGlyph);
	token += receiptLocator(AMessageId);
	return token;
}

static QString receiptMarkHtml(const QString &AMessageId, bool ADelivered)
{
	ushort glyph = ADelivered ? 0x2714 : 0x2713;
	const char *cssClass = ADelivered ? MSMC_MARK_DELIVERED : MSMC_MARK_PENDING;
	return QString("<span class=\"%1\">%2</span>").arg(cssClass).arg(QString(QChar(glyph))) + receiptLocator(AMessageId);
}

MessageReceipts::MessageReceipts()
{
	FPluginManager = NULL;
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FMessageProcessor = NULL;
	FMessageWidgets = NULL;
	FMarkerRecordsLoaded = false;

	FSaveTimer.setSingleShot(true);
	connect(&FSaveTimer,&QTimer::timeout,this,[this]() { saveMarkerRecords(); });
}

MessageReceipts::~MessageReceipts()
{
	if (FSaveTimer.isActive() && FMarkerRecordsLoaded)
		saveMarkerRecords();
	if (FMessageWidgets)
		FMessageWidgets->removeStyleHandler(MSHO_MESSAGERECEIPTS,this);
}

void MessageReceipts::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Message Receipts");
	APluginInfo->description = tr("Requests and sends message delivery receipts (XEP-0184)");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(XMPPSTREAMS_UUID));
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
}

bool MessageReceipts::initConnections(IPluginManager *APluginManager, int &AInitOrder)
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

	return FStanzaProcessor!=NULL && FMessageProcessor!=NULL && FMessageWidgets!=NULL;
}

bool MessageReceipts::initObjects()
{
	if (FMessageProcessor)
	{
		FMessageProcessor->insertMessageEditor(MEO_DEFAULT,this);
	}
	if (FMessageWidgets)
	{
		FMessageWidgets->insertStyleHandler(MSHO_MESSAGERECEIPTS,this);
	}
	return true;
}

bool MessageReceipts::initSettings()
{
	return true;
}

bool MessageReceipts::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder);

	if (ADirection == IMessageProcessor::DirectionOut)
	{
		if (isOutgoingRequestable(AStreamJid,AMessage))
		{
			// A receipt is always for a stanza id, so one has to be present.
			// It is generated only when the id is empty, so the other
			// plugins which already queued their marks for this id (the chat
			// markers plugin does it before this editor) do not become
			// out of sync with the final id sent on the wire.
			if (AMessage.id().isEmpty())
				AMessage.setRandomId();
			AMessage.stanza().addElement("request",NS_MESSAGE_RECEIPTS);
			LOG_STRM_DEBUG(AStreamJid,QString("Message receipt requested, to=%1, id=%2").arg(AMessage.to(),AMessage.id()));
			emit messageReceiptRequested(AStreamJid,AMessage);
			setMarkerRecord(AStreamJid,AMessage.toJid(),AMessage.id(),AMessage.dateTime(),MarkerPending);
		}
	}
	else
	{
		QDomElement receivedElem = AMessage.stanza().firstElement("received",NS_MESSAGE_RECEIPTS);
		if (!receivedElem.isNull())
		{
			QString messageId = receivedElem.attribute("id");
			LOG_STRM_DEBUG(AStreamJid,QString("Message receipt received, from=%1, id=%2").arg(AMessage.from(),messageId));
			emit messageReceiptReceived(AStreamJid,messageId,AMessage.fromJid());
			showMessageReceipt(AStreamJid,messageId,AMessage.fromJid());
		}
		else if (isReceiptRequest(AStreamJid,AMessage))
		{
			sendMessageReceipt(AStreamJid,AMessage);
		}
	}
	return false;
}

bool MessageReceipts::isOutgoingRequestable(const Jid &AStreamJid, const Message &AMessage) const
{
	// Only chat messages with visible content request a receipt (XEP-0184 §2.2.1).
	if (AMessage.type()!=Message::Chat || AMessage.body().isEmpty())
		return false;

	// Carbon copies and receipt confirmations never carry a request again.
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_RECEIPTS).isNull())
		return false;

	if (!AMessage.stanza().firstElement("request",NS_MESSAGE_RECEIPTS).isNull())
		return false;

	// Do not bother the peers which are known not to support the feature.
	return isSupportedByContact(AStreamJid,AMessage.toJid());
}

bool MessageReceipts::isReceiptRequest(const Jid &AStreamJid, const Message &AMessage) const
{
	Q_UNUSED(AStreamJid);
	// Same content limits as for outgoing requests.
	if (AMessage.type()!=Message::Chat || AMessage.body().isEmpty())
		return false;

	// Mirrored carbon copies are processed by the client that received the
	// original message, so they must not be acknowledged here (XEP-0184 §8.2).
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull())
		return false;

	// Only messages which explicitly ask for a receipt are acknowledged.
	if (AMessage.stanza().firstElement("request",NS_MESSAGE_RECEIPTS).isNull())
		return false;

	// A receipt is only possible for a message with a stanza id.
	return !AMessage.id().isEmpty();
}

void MessageReceipts::sendMessageReceipt(const Jid &AStreamJid, const Message &AMessage) const
{
	if (FStanzaProcessor)
	{
		Stanza receipt(STANZA_KIND_MESSAGE);
		receipt.setUniqueId().setTo(AMessage.fromJid().full());
		receipt.addElement("received",NS_MESSAGE_RECEIPTS).setAttribute("id",AMessage.id());
		// Sent directly, so the receipt is not duplicated or displayed anywhere.
		if (FStanzaProcessor->sendStanzaOut(AStreamJid,receipt))
		{
			LOG_STRM_DEBUG(AStreamJid,QString("Message receipt sent, to=%1, id=%2").arg(AMessage.from(),AMessage.id()));
		}
		else
		{
			LOG_STRM_WARNING(AStreamJid,QString("Failed to send message receipt, to=%1, id=%2").arg(AMessage.from(),AMessage.id()));
		}
	}
}

void MessageReceipts::showMessageReceipt(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid)
{
	if (AMessageId.isEmpty())
		return;

	// The delivery state is remembered even when there is no open chat window
	// so the mark can be restored once the history is shown again.
	setMarkerRecord(AStreamJid,AContactJid,AMessageId,QDateTime::currentDateTime(),MarkerDelivered);

	IMessageChatWindow *window = FMessageWidgets!=NULL ? FMessageWidgets->findChatWindow(AStreamJid,AContactJid,false) : NULL;
	if (window == NULL || window->viewWidget()==NULL)
	{
		LOG_STRM_INFO(AStreamJid,QString("Message receipt received with no chat window open, from=%1, id=%2").arg(AContactJid.full(),AMessageId));
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
		QString html = QString("<span style=\"color:#1e8e3e;font-weight:bold;\">&#10004;</span>&nbsp;<span>%1</span>")
			.arg(tr("Delivered").toHtmlEscaped());
		window->viewWidget()->appendHtml(html,options);
		return;
	}

	// Document-based style: patch the marker of the acknowledged message.
	QTextCursor found = browser->document()->find(receiptToken(AMessageId));
	if (found.isNull())
	{
		LOG_STRM_DEBUG(AStreamJid,QString("Message receipt marker not found in style document, id=%1").arg(AMessageId));
		return;
	}
	found.insertHtml(receiptMarkHtml(AMessageId,true));
}

bool MessageReceipts::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AWidget); Q_UNUSED(AHtml); Q_UNUSED(AModifiedHtml); Q_UNUSED(AOptions);
	return AOrder == MSHO_MESSAGERECEIPTS;
}

QString MessageReceipts::messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions)
{
	if (AOrder != MSHO_MESSAGERECEIPTS || !ASlotInTemplate || ASlotName != MSM_CONTENT_SLOT_MARKS)
		return QString();

	if (AWidget == NULL
		|| AOptions.kind != IMessageStyleContentOptions::KindMessage
		|| AOptions.direction != IMessageStyleContentOptions::DirectionOut
		|| AOptions.messageId.isEmpty())
		return QString();

	if (qobject_cast<QTextBrowser *>(AWidget) == NULL)
		return QString();

	int state;
	if (!markerRecordState(AOptions.messageId,state))
		return QString();

	// The mark itself is drawn by the template slot; the tab stop which aligns
	// it to the right border can only be applied after the html is inserted.
	QPointer<QWidget> widget = AWidget;
	QString messageId = AOptions.messageId;
	bool delivered = state==MarkerDelivered;
	QTimer::singleShot(0,this,[this,widget,messageId,delivered]() {
		QTextBrowser *browser = qobject_cast<QTextBrowser *>(widget.data());
		if (browser != NULL)
			applyReceiptMark(browser,messageId,delivered);
	});
	return receiptMarkHtml(AOptions.messageId,delivered);
}

void MessageReceipts::applyReceiptMark(QTextBrowser *ABrowser, const QString &AMessageId, bool ADelivered) const
{
	if (ABrowser==NULL || AMessageId.isEmpty())
		return;

	QTextDocument *document = ABrowser->document();
	if (document == NULL)
		return;

	const QString locator = receiptLocator(AMessageId);
	QTextCursor loc = document->find(locator);
	if (loc.isNull())
	{
		LOG_DEBUG(QString("Failed to find receipt mark in style document, id=%1").arg(AMessageId));
		return;
	}

	// Restore the saved delivery state: the style slot always renders the
	// pending mark, the delivered one is applied here.
	if (ADelivered)
	{
		QTextCursor token = document->find(receiptToken(AMessageId));
		if (!token.isNull())
		{
			token.insertHtml(receiptMarkHtml(AMessageId,true));
			loc = document->find(locator);
			if (loc.isNull())
				return;
		}
	}

	// The glyph is the character right before the locator.
	int glyphPos = loc.selectionStart()-1;
	if (glyphPos < 0)
		return;

	qreal usableWidth = ABrowser->viewport()->width() - 2*document->documentMargin();
	QFontMetricsF metrics(ABrowser->font());
	QTextBlock glyphBlock = document->findBlock(glyphPos);
	QString glyph = glyphBlock.text().mid(glyphPos-glyphBlock.position(),1);
	qreal markerWidth = metrics.horizontalAdvance(glyph) + 8;

	// The mark is aligned to the right border of the view by a tab stop: the
	// tab itself is added here because the html parser drops tab characters.
	QTextCursor prev(document);
	prev.setPosition(glyphPos);
	prev.movePosition(QTextCursor::PreviousCharacter,QTextCursor::KeepAnchor);
	if (prev.selectedText()!=QChar(0x0009))
	{
		QTextCursor tabCursor(document);
		tabCursor.setPosition(glyphPos);
		tabCursor.insertText(usableWidth>markerWidth ? QChar(0x0009) : QChar(0x0020));
	}

	if (usableWidth <= markerWidth)
		return;

	QTextCursor blockCursor(document);
	blockCursor.setPosition(glyphPos);
	QTextBlockFormat blockFormat = blockCursor.block().blockFormat();
	QList<QTextOption::Tab> tabs = blockFormat.tabPositions();
	int rightTab = -1;
	for (int i=0; i<tabs.count(); i++)
	{
		if (tabs.at(i).type == QTextOption::RightTab)
		{
			rightTab = i;
			break;
		}
	}
	QTextOption::Tab tab;
	tab.type = QTextOption::RightTab;
	tab.position = usableWidth - markerWidth;
	if (rightTab>=0)
		tabs[rightTab] = tab;
	else
		tabs.append(tab);
	blockFormat.setTabPositions(tabs);
	blockCursor.setBlockFormat(blockFormat);
}

bool MessageReceipts::isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const
{
	if (FDiscovery && AContactJid.isValid())
	{
		IDiscoInfo info = FDiscovery->discoInfo(AStreamJid,AContactJid);
		if (!info.features.isEmpty())
			return info.features.contains(NS_MESSAGE_RECEIPTS);

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

QString MessageReceipts::markerChatKey(const Jid &AStreamJid, const Jid &AContactJid)
{
	return AStreamJid.pBare()+"\n"+AContactJid.pBare();
}

QString MessageReceipts::markerStateFilePath() const
{
	QString dir = Options::filesPath();
	if (dir.isEmpty() && FPluginManager!=NULL)
		dir = FPluginManager->homePath();
	return dir.isEmpty() ? QString() : dir + "/messagereceipts.state";
}

void MessageReceipts::loadMarkerRecords()
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
		LOG_WARNING(QString("Failed to parse message receipts state: %1").arg(error.errorString()));
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

void MessageReceipts::scheduleMarkerRecordsSave()
{
	// Debounce the write: several markers may arrive in a burst and the whole
	// state is serialized on every change.
	if (!FSaveTimer.isActive())
		FSaveTimer.start(2000);
}

void MessageReceipts::saveMarkerRecords() const
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
			LOG_WARNING(QString("Failed to save message receipts state to file=%1: %2").arg(filePath,file.errorString()));
	}
	else
	{
		LOG_WARNING(QString("Failed to open message receipts state file=%1: %2").arg(filePath,file.errorString()));
	}
}

void MessageReceipts::pruneMarkerRecords()
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

void MessageReceipts::setMarkerRecord(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId, const QDateTime &ATime, int AState)
{
	if (AMessageId.isEmpty())
		return;

	loadMarkerRecords();

	MarkerRecord &record = FMarkerRecords[AMessageId];
	record.chatKey = markerChatKey(AStreamJid,AContactJid);
	if (record.time <= 0)
		record.time = ATime.isValid() ? ATime.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
	record.state = AState;

	pruneMarkerRecords();
	scheduleMarkerRecordsSave();
}

bool MessageReceipts::markerRecordState(const QString &AMessageId, int &AState)
{
	if (AMessageId.isEmpty())
		return false;

	loadMarkerRecords();

	QHash<QString, MarkerRecord>::const_iterator it = FMarkerRecords.constFind(AMessageId);
	if (it == FMarkerRecords.constEnd())
		return false;

	AState = it->state;
	return true;
}
