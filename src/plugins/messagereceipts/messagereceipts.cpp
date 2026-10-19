#include "messagereceipts.h"

#include <definitions/namespaces.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
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
// Pending receipts are displayed as a check mark appended to the end of the
// last line of the sent message: gray while waiting, green when a
// confirmation arrives. The marker carries a per-message tag (invisible
// zero-width characters), so a receipt is bound to the exact message in the
// style document. The marker is inserted right after the content of the
// message is in the style view (schedule of a zero-delay timer, like the
// image viewer plugin does), so it always lands on the same line.

// Builds the tag which binds a receipt marker to a given stanza id.
// Only the leading check mark is visible; the rest is zero-width.
static QString receiptToken(const QString &AMessageId)
{
	quint32 hash = qHash(AMessageId);
	QString token;
	token += QChar(0x2713);
	static const ushort keys[] = {0x200B,0x200C,0x200D,0x2060,0x2061,0x2062,0x2063};
	for (int i=0; i<6; i++)
	{
		token += QChar(keys[hash & 0x07]);
		hash >>= 3;
	}
	return token;
}

// Inserts a gray marker with the given message id at the end of the last
// non-empty text block of the style document, aligned to the right border
// of the view by a right-aligned tab stop.
static bool appendReceiptMarker(QTextBrowser *ABrowser, const QString &AMessageId)
{
	if (ABrowser == NULL || AMessageId.isEmpty())
		return false;

	QTextDocument *document = ABrowser->document();
	QTextBlock block = document->lastBlock();
	while (block.isValid() && block.length()<=1 && block.previous().isValid())
		block = block.previous(); // skip blocks created by a trailing <br>
	if (!block.isValid() || block.length()<=1)
		return false;

	QTextCharFormat format;
	format.setForeground(QColor(176,176,176));
	QString marker;
	marker += QChar(0x00A0); // keep the mark detached from the message text
	marker += receiptToken(AMessageId);

	// Align the marker to the right border of the view with a tab stop.
	qreal usableWidth = ABrowser->viewport()->width() - 2*document->documentMargin();
	QFontMetricsF metrics(ABrowser->font());
	qreal markerWidth = metrics.horizontalAdvance(marker) + 8;
	if (usableWidth > markerWidth)
	{
		QTextOption::Tab tab;
		tab.type = QTextOption::RightTab;
		tab.position = usableWidth - markerWidth;

		QTextBlockFormat blockFormat = QTextCursor(block).blockFormat();
		blockFormat.setTabPositions(blockFormat.tabPositions() << tab);

		QTextCursor cursor(block);
		cursor.movePosition(QTextCursor::EndOfBlock);
		cursor.setBlockFormat(blockFormat);
		marker.prepend(QChar(0x0009)); // tab to the right-aligned position
		cursor.insertText(marker,format);
	}
	else
	{
		QTextCursor cursor(block);
		cursor.movePosition(QTextCursor::EndOfBlock);
		cursor.insertText(marker,format);
	}
	return true;
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
			AMessage.setRandomId();
			AMessage.stanza().addElement("request",NS_MESSAGE_RECEIPTS);
			LOG_STRM_DEBUG(AStreamJid,QString("Message receipt requested, to=%1, id=%2").arg(AMessage.to(),AMessage.id()));
			emit messageReceiptRequested(AStreamJid,AMessage);
			setMarkerRecord(AStreamJid,AMessage.toJid(),AMessage.id(),AMessage.dateTime(),MarkerPending);
			enqueueReceiptMarker(AStreamJid,AMessage.id(),AMessage.toJid());
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
	if (AMessageId.isEmpty())
		return;
	QTextCursor found = browser->document()->find(receiptToken(AMessageId));
	if (found.isNull())
	{
		LOG_STRM_DEBUG(AStreamJid,QString("Message receipt marker not found in style document, id=%1").arg(AMessageId));
		return;
	}
	QTextCharFormat format;
	format.setForeground(QColor(30,142,61));
	format.setFontWeight(QFont::Bold);
	found.insertText(QChar(0x2714),format);
}

bool MessageReceipts::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AHtml);

	if (AOrder != MSHO_MESSAGERECEIPTS)
		return false;

	if (AWidget == NULL
		|| AOptions.kind != IMessageStyleContentOptions::KindMessage
		|| AOptions.direction != IMessageStyleContentOptions::DirectionOut)
		return true;

	// History messages are restored from the previously saved delivery state.
	// The invisible token is appended to the message html so a deferred patch
	// can locate the exact message even when the whole history is rendered in
	// one go (the "last block" heuristic of the live path would not work).
	if (AOptions.type & IMessageStyleContentOptions::TypeHistory)
	{
		int state;
		if (!AOptions.messageId.isEmpty() && markerRecordState(AOptions.messageId,state)
			&& qobject_cast<QTextBrowser *>(AWidget)!=NULL)
		{
			AModifiedHtml += receiptToken(AOptions.messageId);
			QPointer<QWidget> widget = AWidget;
			QString messageId = AOptions.messageId;
			bool delivered = state==MarkerDelivered;
			QTimer::singleShot(0,this,[this,widget,messageId,delivered]() {
				QTextBrowser *browser = qobject_cast<QTextBrowser *>(widget.data());
				if (browser != NULL)
					patchReceiptMarker(browser,messageId,delivered);
			});
		}
		return true;
	}

	// AModifiedHtml already contains what other handlers made of the html,
	// so it must not be reset here (it would, e.g., undo the placeholders
	// the image viewer replaced the image urls with). The message itself is
	// not rendered yet either, so nothing is appended to the html: instead,
	// schedule a deferred insertion of pending markers into the style
	// document when the message content is appended.
	ReceiptMarkerTarget *target = findMarkerTarget(AWidget);
	if (target != NULL && !target->flushing && !target->ids.isEmpty())
	{
		target->flushing = true;
		QPointer<QWidget> widget = AWidget;
		QTimer::singleShot(0,this,[this,widget]() {
			if (widget != NULL)
			{
				ReceiptMarkerTarget *target = findMarkerTarget(widget);
				if (target != NULL)
					flushReceiptMarkers(target);
			}
		});
	}
	return true;
}

void MessageReceipts::patchReceiptMarker(QTextBrowser *ABrowser, const QString &AMessageId, bool ADelivered) const
{
	if (ABrowser==NULL || AMessageId.isEmpty())
		return;

	QTextCursor found = ABrowser->document()->find(receiptToken(AMessageId));
	if (found.isNull())
	{
		LOG_DEBUG(QString("Failed to find historical receipt marker in style document, id=%1").arg(AMessageId));
		return;
	}

	QTextCursor cursor(found);
	cursor.removeSelectedText();

	QTextCharFormat format;
	format.setForeground(ADelivered ? QColor(30,142,61) : QColor(176,176,176));
	if (ADelivered)
		format.setFontWeight(QFont::Bold);

	QString marker;
	marker += QChar(0x00A0);
	marker += QChar(0x2713);

	qreal usableWidth = ABrowser->viewport()->width() - 2*ABrowser->document()->documentMargin();
	QFontMetricsF metrics(ABrowser->font());
	qreal markerWidth = metrics.horizontalAdvance(marker) + 8;
	if (usableWidth > markerWidth)
	{
		QTextOption::Tab tab;
		tab.type = QTextOption::RightTab;
		tab.position = usableWidth - markerWidth;

		QTextBlockFormat blockFormat = cursor.blockFormat();
		blockFormat.setTabPositions(blockFormat.tabPositions() << tab);
		cursor.setBlockFormat(blockFormat);
		marker.prepend(QChar(0x0009));
	}
	cursor.insertText(marker,format);
}

void MessageReceipts::flushReceiptMarkers(ReceiptMarkerTarget *ATarget)
{
	ATarget->flushing = false;

	QTextBrowser *browser = ATarget->widget!=NULL ? qobject_cast<QTextBrowser *>(ATarget->widget) : NULL;
	while (!ATarget->ids.isEmpty())
	{
		QString messageId = ATarget->ids.takeFirst();
		bool inserted = false;
		if (browser != NULL)
			inserted = appendReceiptMarker(browser,messageId);
		if (!inserted)
			LOG_DEBUG(QString("Failed to insert receipt marker to style document, id=%1").arg(messageId));
	}

	// Drop targets for widgets which are gone.
	QList<ReceiptMarkerTarget> targets;
	foreach(const ReceiptMarkerTarget &item, FPendingMarkers)
	{
		if (!item.widget.isNull())
			targets.append(item);
	}
	FPendingMarkers = targets;
}

void MessageReceipts::enqueueReceiptMarker(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid)
{
	IMessageChatWindow *window = FMessageWidgets!=NULL ? FMessageWidgets->findChatWindow(AStreamJid,AContactJid,false) : NULL;
	QWidget *styleWidget = window!=NULL && window->viewWidget()!=NULL ? window->viewWidget()->styleWidget() : NULL;
	if (styleWidget == NULL || AMessageId.isEmpty())
		return;
	ReceiptMarkerTarget *target = findMarkerTarget(styleWidget);
	if (target != NULL)
	{
		target->ids.append(AMessageId);
	}
	else
	{
		ReceiptMarkerTarget newTarget;
		newTarget.widget = QPointer<QWidget>(styleWidget);
		newTarget.ids.append(AMessageId);
		newTarget.flushing = false;
		FPendingMarkers.append(newTarget);
	}

	// Drop targets for widgets which are gone.
	QList<ReceiptMarkerTarget> targets;
	foreach(const ReceiptMarkerTarget &item, FPendingMarkers)
	{
		if (!item.widget.isNull())
			targets.append(item);
	}
	FPendingMarkers = targets;
}

MessageReceipts::ReceiptMarkerTarget *MessageReceipts::findMarkerTarget(QWidget *AWidget)
{
	for (QList<ReceiptMarkerTarget>::iterator it=FPendingMarkers.begin(); it!=FPendingMarkers.end(); ++it)
	{
		if (it->widget == AWidget)
			return &(*it);
	}
	return NULL;
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
