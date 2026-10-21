#include "messagecorrect.h"

#include <definitions/namespaces.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
#include <definitions/optionnodes.h>
#include <definitions/optionvalues.h>
#include <definitions/optionwidgetorders.h>
#include <utils/action.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextOption>
#include <QFontMetricsF>
#include <QColor>
#include <QKeyEvent>
#include <QTimer>

// XEP-0308 Last Message Correction for one-to-one chats.
//
// An outgoing correction carries <replace xmlns='urn:xmpp:message-correct:0'
// id='...'/> with the stanza id of the shown version of the same message.
// A received correction replaces the text of the referenced message: the
// body of the displayed message is replaced with the corrected body, so
// the conversation does not get an additional message. If the bodies can
// not be located in the document, the whole appended block is moved to the
// place of the old message instead.

// The number of the recent outgoing messages kept in a chat for corrections.
#define MAX_OUTGOING_RECORDS			20
// The maximum number of the displayed contents tracked for one style widget.
#define MAX_CONTENT_SLOTS				500
// The maximum number of the received correction references kept in memory.
#define MAX_CORRECTION_TARGETS			1000
// The maximum number of the message bodies kept for the display patching.
#define MAX_MESSAGE_BODIES				1000

// Builds an invisible token which pins the end of a content block. Every
// inserted content uses a unique seed, unlike the message id tokens of the
// other plugins, so the appended copy of a corrected message is never
// confused with the original message which it replaces.
static QString contentToken(quint64 &ASeed)
{
	static const ushort keys[] = {0x2060,0x2061,0x2062,0x2063,0x2064,0x2065,0x2066,0x2067,0x2068,0x2069,0x206A,0x206B,0x206C,0x206D,0x206E,0x206F};
	static const int KeyCount = 16;

	ASeed++;
	QString token;
	quint64 seed = ASeed;
	for (int i=0; i<6; i++)
	{
		token += QChar(keys[seed % KeyCount]);
		seed /= KeyCount;
	}
	return token;
}

// The first line of the message body: the plain text of the document is
// split into paragraphs, so the whole multiline body never matches.
static QString firstTextLine(const QString &AText)
{
	return AText.section('\n',0,0).trimmed();
}

// Searches the visible text of a message body within one displayed content
// block. The occurrence placed in the same paragraph as the block end token
// is preferred, so the body text is never confused with the text of the
// header placed in the preceding paragraphs.
static QTextCursor findBodyStart(QTextDocument *ADocument, const QString &AProbe, int AStart, int AEnd, int AAnchorBlock)
{
	if (AProbe.isEmpty())
		return QTextCursor();

	QTextCursor firstFound;
	int pos = AStart;
	while (pos + AProbe.length() <= AEnd)
	{
		QTextCursor found = ADocument->find(AProbe,pos);
		if (found.isNull() || found.selectionEnd() > AEnd)
			break;
		if (firstFound.isNull())
			firstFound = found;
		if (ADocument->findBlock(found.selectionStart()).blockNumber() == AAnchorBlock)
			return found;
		pos = found.selectionStart() + 1;
	}
	return firstFound;
}

// Builds the same invisible tag which the message chat markers plugin
// binds to the displayed state of a message (the algorithm has to stay
// identical to the one in messagechatmarkers.cpp).
static QString displayedMarkerToken(const QString &AMessageId)
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

// Appends the note about the edited version: a pencil glyph at the right
// border of the message line, the same way the receipt and chat marker
// glyphs of the other plugins are aligned.
static void appendEditedNote(QTextBrowser *ABrowser, QTextCursor &ACursor)
{
	const QChar pencil(0x270E); // ✎ lower right pencil
	QTextCharFormat format;
	format.setForeground(QColor(128,128,128));

	const qreal usableWidth = ABrowser->viewport()->width() - 2*ABrowser->document()->documentMargin();
	const qreal pencilWidth = QFontMetricsF(ABrowser->font()).horizontalAdvance(pencil) + 4;
	if (usableWidth > pencilWidth)
	{
		// The delivery marks of the other plugins are already aligned with
		// the right tab stops of the block, so their tab stops are shifted
		// left to make a room for the pencil at the right border.
		QTextBlockFormat blockFormat = ACursor.blockFormat();
		QList<QTextOption::Tab> tabs = blockFormat.tabPositions();
		for (int i=0; i<tabs.count(); i++)
			tabs[i].position = qBound<qreal>(pencilWidth,tabs.at(i).position-pencilWidth-4,usableWidth);
		QTextOption::Tab tab;
		tab.type = QTextOption::RightTab;
		tab.position = usableWidth - pencilWidth;
		blockFormat.setTabPositions(tabs << tab);
		ACursor.setBlockFormat(blockFormat);
		ACursor.insertText(QString(QChar(0x0009))+pencil,format);
	}
	else
	{
		// The line is too narrow for the tab alignment.
		ACursor.insertText(QString(" ") + pencil,format);
	}
}

MessageCorrect::MessageCorrect()
{
	FPluginManager = NULL;
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FMessageProcessor = NULL;
	FMessageWidgets = NULL;
	FOptionsManager = NULL;
	FTokenCounter = 0;
}

MessageCorrect::~MessageCorrect()
{
	if (FMessageWidgets)
		FMessageWidgets->removeStyleHandler(MSHO_MESSAGECORRECT,this);
	if (FOptionsManager)
		FOptionsManager->removeOptionsDialogHolder(this);
	qDeleteAll(FContentTraces);
}

void MessageCorrect::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Message Correct");
	APluginInfo->description = tr("Allows to edit the sent chat messages (XEP-0308)");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->dependences.append(QUuid(OPTIONSMANAGER_UUID));
}

bool MessageCorrect::initConnections(IPluginManager *APluginManager, int &AInitOrder)
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

	return FMessageProcessor!=NULL && FMessageWidgets!=NULL;
}

bool MessageCorrect::initObjects()
{
	if (FDiscovery)
	{
		IDiscoFeature dfeature;
		dfeature.var = NS_MESSAGE_CORRECT;
		dfeature.active = true;
		dfeature.name = tr("Message Correct");
		dfeature.description = tr("Supports the editing of the sent chat messages");
		FDiscovery->insertDiscoFeature(dfeature);
	}
	if (FMessageProcessor)
	{
		FMessageProcessor->insertMessageEditor(MEO_MESSAGECORRECT,this);
	}
	if (FMessageWidgets)
	{
		FMessageWidgets->insertStyleHandler(MSHO_MESSAGECORRECT,this);
		connect(FMessageWidgets->instance(),SIGNAL(chatWindowCreated(IMessageChatWindow *)),SLOT(onChatWindowCreated(IMessageChatWindow *)));
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
			onChatWindowCreated(window);
	}
	return true;
}

bool MessageCorrect::initSettings()
{
	Options::setDefaultValue(OPV_MESSAGES_MESSAGECORRECTENABLED, true);

	if (FOptionsManager)
		FOptionsManager->insertOptionsDialogHolder(this);

	return true;
}

QMultiMap<int, IOptionsDialogWidget *> MessageCorrect::optionsDialogWidgets(const QString &ANodeId, QWidget *AParent)
{
	QMultiMap<int, IOptionsDialogWidget *> widgets;
	if (FOptionsManager && ANodeId==OPN_MESSAGES)
	{
		widgets.insert(OWO_MESSAGES_MESSAGECORRECTENABLED, FOptionsManager->newOptionsDialogWidget(Options::node(OPV_MESSAGES_MESSAGECORRECTENABLED),tr("Allow to edit and correct sent messages"),AParent));
	}
	return widgets;
}

bool MessageCorrect::isEnabled() const
{
	return Options::node(OPV_MESSAGES_MESSAGECORRECTENABLED).value().toBool();
}

bool MessageCorrect::isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const
{
	if (FDiscovery && AContactJid.isValid())
	{
		IDiscoInfo info = FDiscovery->discoInfo(AStreamJid,AContactJid);
		if (!info.features.isEmpty())
			return info.features.contains(NS_MESSAGE_CORRECT);

		// The capabilities are not known yet: ask once and allow the first
		// correction, the following ones will be limited by the disco info.
		QString key = AStreamJid.pBare()+"\n"+AContactJid.pBare();
		if (!FRequestedDisco.contains(key))
		{
			FRequestedDisco.insert(key);
			FDiscovery->requestDiscoInfo(AStreamJid,AContactJid);
		}
	}
	return true;
}

bool MessageCorrect::isOutgoingCorrection(const Jid &AStreamJid, const Message &AMessage) const
{
	// Only a message sent from a chat editor with the editing session for
	// that chat is a candidate for the <replace/> element.
	if (!FEditSessions.contains(chatKey(AStreamJid,AMessage.toJid())))
		return false;
	if (AMessage.body().isEmpty())
		return false;

	// Carbon copies and the elements of other plugins are not corrected.
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_RECEIPTS).isNull()
		|| !AMessage.stanza().firstElement("displayed",NS_MESSAGE_MARKERS).isNull())
		return false;

	// A message being sent never carries <replace/> yet - the candidate
	// is everything that survived the guards above.
	return AMessage.stanza().firstElement("replace",NS_MESSAGE_CORRECT).isNull();
}

bool MessageCorrect::isIncomingCorrection(const Jid &AStreamJid, const Message &AMessage) const
{
	Q_UNUSED(AStreamJid);

	// Group chats and error replies never carry a correction.
	if (AMessage.type()==Message::GroupChat || AMessage.type()==Message::Error)
		return false;
	if (AMessage.body().isEmpty())
		return false;

	// A carbon copy of an own correction is not a received correction.
	if (!AMessage.stanza().firstElement("sent",NS_MESSAGE_CARBONS).isNull()
		|| !AMessage.stanza().firstElement("received",NS_MESSAGE_CARBONS).isNull())
		return false;

	return !AMessage.stanza().firstElement("replace",NS_MESSAGE_CORRECT).isNull();
}

void MessageCorrect::setCorrectionTarget(const QString &AMessageId, const QString &ATargetId)
{
	if (AMessageId.isEmpty() || ATargetId.isEmpty() || AMessageId==ATargetId)
		return;

	FCorrectionTargets.insert(AMessageId,ATargetId);
	FCorrectionOrder.removeAll(AMessageId);
	FCorrectionOrder.append(AMessageId);

	while (FCorrectionOrder.count() > MAX_CORRECTION_TARGETS)
	{
		const QString oldestId = FCorrectionOrder.takeFirst();
		FCorrectionTargets.remove(oldestId);
	}
}

void MessageCorrect::rememberMessageBody(const QString &AMessageId, const QString &ABody)
{
	if (AMessageId.isEmpty() || ABody.isEmpty())
		return;

	FMessageBodies.insert(AMessageId,ABody);
	FMessageBodiesOrder.removeAll(AMessageId);
	FMessageBodiesOrder.append(AMessageId);

	while (FMessageBodiesOrder.count() > MAX_MESSAGE_BODIES)
	{
		const QString oldestId = FMessageBodiesOrder.takeFirst();
		FMessageBodies.remove(oldestId);
	}
}

QString MessageCorrect::chatKey(const Jid &AStreamJid, const Jid &AContactJid) const
{
	return AStreamJid.pBare()+"\n"+AContactJid.full();
}

bool MessageCorrect::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder);

	if (!isEnabled() || AMessage.type()!=Message::Chat || AMessage.body().isEmpty())
		return false;

	if (ADirection == IMessageProcessor::DirectionOut)
	{
		if (AMessage.id().isEmpty())
			AMessage.setRandomId();

		rememberMessageBody(AMessage.id(),AMessage.body());

		if (isOutgoingCorrection(AStreamJid,AMessage))
		{
			// The message is sent as an edit of the version selected in the
			// editor, so the editing session is consumed on the send. Only
			// the last message version is correctable, so the target is
			// always the most recent record - index bookkeeping does not
			// take part, there is no separate editor work state to desync.
			const QList<OutgoingRecord> records = FOutgoingRecords.value(chatKey(AStreamJid,AMessage.toJid()));
			FEditSessions.remove(chatKey(AStreamJid,AMessage.toJid()));
			if (!records.isEmpty() && isSupportedByContact(AStreamJid,AMessage.toJid()))
			{
				const QString targetId = records.last().messageId;
				// The correction thread keeps one identity: every correction
				// references the id of the original message of the thread
				// (this is how the widespread clients behave), so the chain
				// of the already sent corrections is resolved first.
				QString threadId = targetId;
				while (FCorrectionTargets.contains(threadId))
					threadId = FCorrectionTargets.value(threadId);
				AMessage.stanza().addElement("replace",NS_MESSAGE_CORRECT).setAttribute("id",threadId);
				LOG_STRM_INFO(AStreamJid,QString("[MessageCorrect] correction sent, to=%1, id=%2, replaces=%3").arg(AMessage.to(),AMessage.id(),threadId));
				setCorrectionTarget(AMessage.id(),threadId);
				storeOutgoingRecord(AStreamJid,AMessage,threadId);
			}
			else
			{
				// No record to replace or the contact seems not to support
				// the corrections: the edited text is sent as a usual message.
				LOG_STRM_INFO(AStreamJid,QString("Message correction is not sent, records=%1, sent as a new message").arg(records.count()));
				storeOutgoingRecord(AStreamJid,AMessage,QString());
			}
		}
		else
		{
			// A plain message is remembered so it can be edited later.
			storeOutgoingRecord(AStreamJid,AMessage,QString());
		}
	}
	else if (isIncomingCorrection(AStreamJid,AMessage))
	{
		const QString targetId = AMessage.stanza().firstElement("replace",NS_MESSAGE_CORRECT).attribute("id");
		if (AMessage.id().isEmpty())
			AMessage.setRandomId();
		rememberMessageBody(AMessage.id(),AMessage.body());
		setCorrectionTarget(AMessage.id(),targetId);
		LOG_STRM_INFO(AStreamJid,QString("[MessageCorrect] correction received, from=%1, id=%2, replaces=%3").arg(AMessage.from(),AMessage.id(),targetId));
	}
	else if (!AMessage.id().isEmpty())
	{
		// A plain message is remembered so its body can be replaced when a
		// correction of it is received.
		rememberMessageBody(AMessage.id(),AMessage.body());
	}
	return false;
}

void MessageCorrect::storeOutgoingRecord(const Jid &AStreamJid, const Message &AMessage, const QString &ATargetId)
{
	const QString key = chatKey(AStreamJid,AMessage.toJid());
	QList<OutgoingRecord> &records = FOutgoingRecords[key];

	if (!ATargetId.isEmpty())
	{
		// The corrected message gets the most recent place again, and its
		// current id and text are used by the next correction.
		for (int i=records.count()-1; i>=0; i--)
		{
			if (records.at(i).messageId == ATargetId)
			{
				records.removeAt(i);
				break;
			}
		}
	}

	OutgoingRecord record;
	record.messageId = AMessage.id();
	record.body = AMessage.body();
	records.append(record);

	while (records.count() > MAX_OUTGOING_RECORDS)
		records.removeFirst();
}

IMessageChatWindow *MessageCorrect::findEditWindow(QObject *AEditorInstance) const
{
	if (FMessageWidgets && AEditorInstance!=NULL)
	{
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
		{
			if (window->editWidget()!=NULL && window->editWidget()->instance()==AEditorInstance)
				return window;
		}
	}
	return NULL;
}

void MessageCorrect::setEditorMessage(IMessageEditWidget *AEditor, const OutgoingRecord &ARecord)
{
	if (AEditor==NULL || AEditor->document()==NULL)
		return;

	AEditor->document()->setPlainText(ARecord.body);
	setEditorCorrectionMode(AEditor,true);
	QTextEdit *editor = AEditor->textEdit();
	if (editor != NULL)
	{
		editor->moveCursor(QTextCursor::End,QTextCursor::MoveAnchor);
		editor->setPlaceholderText(tr("Editing previous message, Esc - cancel"));
	}
}

bool MessageCorrect::startMessageEditing(IMessageEditWidget *AEditor, const QString &AChatKey)
{
	const QList<OutgoingRecord> records = FOutgoingRecords.value(AChatKey);
	if (records.isEmpty() || FEditSessions.contains(AChatKey))
		return false;

	// Only the last message version is correctable, so the editor always
	// gets the most recent record.
	FEditSessions.insert(AChatKey,records.count()-1);
	setEditorMessage(AEditor,records.last());
	LOG_INFO(QString("[MessageCorrect] editing started, chat=%1, id=%2").arg(QString(AChatKey).replace("\n","|"),records.last().messageId));
	return true;
}

void MessageCorrect::cancelMessageEditing(IMessageEditWidget *AEditor, const QString &AChatKey)
{
	FEditSessions.remove(AChatKey);
	setEditorCorrectionMode(AEditor,false);
	if (AEditor != NULL)
	{
		QTextEdit *editor = AEditor->textEdit();
		if (editor != NULL)
			editor->setPlaceholderText(QString());
		if (AEditor->document() != NULL)
			AEditor->document()->setPlainText(QString());
	}
}

void MessageCorrect::setEditorCorrectionMode(IMessageEditWidget *AEditor, bool AOn)
{
	QTextEdit *editor = AEditor!=NULL ? AEditor->textEdit() : NULL;
	if (editor == NULL)
		return;

	// The correction marker for the user: while the editor shows a previous
	// message its field is highlighted and its text is italic. The previous
	// look is stored on the editor itself and is restored when the editing
	// session ends, whatever way it ended.
	static const char *savedStyleProperty = "correctionSavedStyleSheet";
	if (AOn)
	{
		if (!editor->property(savedStyleProperty).isValid())
		{
			editor->setProperty(savedStyleProperty,editor->styleSheet());
			editor->setStyleSheet("background-color: #FFF59C; font-style: italic;");
		}
	}
	else
	{
		const QVariant saved = editor->property(savedStyleProperty);
		if (saved.isValid())
		{
			editor->setProperty(savedStyleProperty,QVariant());
			editor->setStyleSheet(saved.toString());
		}
	}
}

QStringList MessageCorrect::rescuedMarkerTokens(QTextCursor &ARemoveCursor, QWidget *AStyleWidget, const QString &ANewMessageId) const
{
	// The marker tokens which are still present in the replaced content
	// belong to the versions whose read marker has not arrived yet (the
	// widespread clients reference the id of the thread root in the
	// displayed markers, so a delayed acknowledgment is possible for any
	// version): the removal takes them out of the document, so they have
	// to be recovered inside the restored content.
	QStringList tokens;
	const QString removedText = ARemoveCursor.selectedText();
	if (removedText.isEmpty())
		return tokens;

	QStringList threadIds;
	if (FMessageWidgets!=NULL && AStyleWidget!=NULL)
	{
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
		{
			if (window->viewWidget()!=NULL && window->viewWidget()->styleWidget()==AStyleWidget)
			{
				const QList<OutgoingRecord> records = FOutgoingRecords.value(chatKey(window->streamJid(),window->contactJid()));
				for (int i=0; i<records.count(); i++)
					threadIds.append(records.at(i).messageId);
				break;
			}
		}
	}
	if (!ANewMessageId.isEmpty())
		threadIds.append(ANewMessageId);

	// Each version can be acknowledged by the id of its thread root, so
	// the roots are checked together with the version ids themselves.
	QStringList uniqueIds;
	for (int i=0; i<threadIds.count(); i++)
	{
		const QString id = threadIds.at(i);
		if (!uniqueIds.contains(id))
			uniqueIds.append(id);
		QString root = id;
		while (FCorrectionTargets.contains(root))
			root = FCorrectionTargets.value(root);
		if (!uniqueIds.contains(root))
			uniqueIds.append(root);
	}

	for (int i=0; i<uniqueIds.count(); i++)
	{
		const QString token = displayedMarkerToken(uniqueIds.at(i));
		if (!token.isEmpty() && removedText.contains(token) && !tokens.contains(token))
			tokens.append(token);
	}
	return tokens;
}

ContentTrace *MessageCorrect::findContentTrace(QWidget *AWidget)
{
	foreach(ContentTrace *trace, FContentTraces)
	{
		if (trace->widget == AWidget)
			return trace;
	}

	if (AWidget == NULL)
		return NULL;

	ContentTrace *trace = new ContentTrace;
	trace->widget = QPointer<QWidget>(AWidget);
	FContentTraces.append(trace);
	return trace;
}

void MessageCorrect::pruneContentTraces()
{
	for (int i=0; i<FContentTraces.count(); i++)
	{
		ContentTrace *trace = FContentTraces.at(i);
		if (trace->widget.isNull())
		{
			delete trace;
			FContentTraces.removeAt(i);
			i--;
		}
	}
}

void MessageCorrect::registerContentSlot(QTextBrowser *ABrowser, const QString &AMessageId, QString &AModifiedHtml)
{
	ContentTrace *trace = findContentTrace(ABrowser);
	if (trace == NULL)
		return;

	ContentSlot slot;
	slot.endToken = contentToken(FTokenCounter);
	slot.originalId = AMessageId;
	slot.latestId = AMessageId;
	trace->slotList.append(slot);
	while (trace->slotList.count() > MAX_CONTENT_SLOTS)
		trace->slotList.removeFirst();

	// The token is appended to the message html, so it is placed after the
	// last symbol of the content (chars before the html would start an
	// outdated empty paragraph in the template of the style).
	AModifiedHtml += slot.endToken;

	if (!AMessageId.isEmpty() && FCorrectionTargets.contains(AMessageId))
	{
		// The content is a correction: try to patch it into the place of
		// the displayed version of the message after the whole content is
		// appended to the document by the style.
		LOG_INFO(QString("[MessageCorrect] patch scheduled, id=%1").arg(AMessageId));
		QPointer<QWidget> widget(ABrowser);
		const QString messageId = AMessageId;
		QTimer::singleShot(0,this,[this,widget,messageId]() {
			if (!widget.isNull())
			{
				QTextBrowser *browser = qobject_cast<QTextBrowser *>(widget.data());
				ContentTrace *trace = findContentTrace(widget.data());
				if (browser!=NULL && trace!=NULL)
					patchCorrectedMessage(browser,trace,messageId);
			}
		});
	}
}

bool MessageCorrect::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AHtml);
	if (AOrder != MSHO_MESSAGECORRECT)
		return false;

	if (AWidget != NULL)
	{
		QTextBrowser *browser = qobject_cast<QTextBrowser *>(AWidget);
		if (browser != NULL)
			registerContentSlot(browser,AOptions.messageId,AModifiedHtml);
	}
	return true;
}

bool MessageCorrect::patchCorrectedMessage(QTextBrowser *ABrowser, ContentTrace *ATrace, const QString &AMessageId)
{
	if (ABrowser==NULL || ATrace==NULL || AMessageId.isEmpty())
		return false;

	// Remove the traces of the contents which were cut off from the document
	// (e.g. after the view was cleared or the style message limit reached):
	// only the contents present in the document can be patched properly.
	for (int i=0; i<ATrace->slotList.count(); i++)
	{
		if (ABrowser->document()->find(ATrace->slotList.at(i).endToken).isNull())
		{
			ATrace->slotList.removeAt(i);
			i--;
		}
	}

	// The appended block is the last content displayed by the style: it is
	// bounded by the end of the previous content and by the end of the
	// document. If there is no previous content (or the reference is gone)
	// the correction stays displayed as an ordinary message.
	int appendIndex = -1;
	for (int i=ATrace->slotList.count()-1; i>=0; i--)
	{
		if (ATrace->slotList.at(i).originalId == AMessageId)
		{
			appendIndex = i;
			break;
		}
	}
	if (appendIndex <= 0)
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: appended content not found, id=%1").arg(AMessageId));
		return false;
	}

	QTextDocument *document = ABrowser->document();
	QTextCursor prevToken = document->find(ATrace->slotList.at(appendIndex-1).endToken);
	if (prevToken.isNull())
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: previous content token missing, id=%1").arg(AMessageId));
		return false;
	}

	// Resolve the chain of the correction references until a displayed
	// message is found: a correction may be sent about a version of the
	// message which is already shown replaced with another correction.
	QString listedId = FCorrectionTargets.value(AMessageId,AMessageId);
	int targetIndex = -1;
	for (int attempts=0; targetIndex<0 && attempts<16; attempts++)
	{
		for (int i=0; i<appendIndex; i++)
		{
			const ContentSlot &slot = ATrace->slotList.at(i);
			if (slot.originalId==listedId || slot.latestId==listedId)
			{
				targetIndex = i;
				break;
			}
		}
		if (targetIndex >= 0 || !FCorrectionTargets.contains(listedId))
			break;
		listedId = FCorrectionTargets.value(listedId);
	}
	if (targetIndex < 0)
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: corrected message not displayed, id=%1, target=%2").arg(AMessageId,listedId));
		return false;
	}

	QTextCursor targetEndToken = document->find(ATrace->slotList.at(targetIndex).endToken);
	QTextCursor newToken = document->find(ATrace->slotList.at(appendIndex).endToken);
	if (targetEndToken.isNull() || newToken.isNull())
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: target tokens missing, id=%1").arg(AMessageId));
		return false;
	}

	// A content block of the style starts after the whole block of the
	// previous content: every plugin appends its trailing marks (the
	// receipt and the chat marker tokens) to the end of the body block, so
	// they must not be cut off together with the appended block.
	QTextBlock appendPrevBlock = document->findBlock(prevToken.selectionEnd());
	const int appendContentStart = appendPrevBlock.isValid() ? appendPrevBlock.position()+appendPrevBlock.length() : prevToken.selectionEnd();
	QTextBlock oldBodyBlock = document->findBlock(targetEndToken.selectionStart());
	const int oldBlockEnd = oldBodyBlock.isValid() ? oldBodyBlock.position()+oldBodyBlock.length()-1 : targetEndToken.selectionStart();
	QTextBlock newBodyBlock = document->findBlock(newToken.selectionStart());
	const int newBundleEnd = newBodyBlock.isValid() ? newBodyBlock.position()+newBodyBlock.length()-1 : document->characterCount();

	// Try to replace only the body of the displayed message, so the header
	// with the sender and time of the first version stays in the place.
	const int targetBlockStart = targetIndex>0 ? document->find(ATrace->slotList.at(targetIndex-1).endToken).selectionEnd() : 0;
	const int newAnchorBlock = document->findBlock(newToken.selectionStart()).blockNumber();
	const int oldAnchorBlock = oldBodyBlock.blockNumber();
	QTextCursor newBodyStart = findBodyStart(document,FMessageBodies.value(AMessageId).trimmed(),appendContentStart,newToken.selectionStart(),newAnchorBlock);
	if (newBodyStart.isNull())
		newBodyStart = findBodyStart(document,firstTextLine(FMessageBodies.value(AMessageId)),appendContentStart,newToken.selectionStart(),newAnchorBlock);
	QTextCursor oldBodyStart = findBodyStart(document,FMessageBodies.value(ATrace->slotList.at(targetIndex).latestId).trimmed(),targetBlockStart,oldBlockEnd,oldAnchorBlock);
	if (oldBodyStart.isNull())
		oldBodyStart = findBodyStart(document,firstTextLine(FMessageBodies.value(ATrace->slotList.at(targetIndex).latestId)),targetBlockStart,oldBlockEnd,oldAnchorBlock);

	if (!newBodyStart.isNull() && !oldBodyStart.isNull())
	{
		const int scrollPosition = ABrowser->verticalScrollBar()->sliderPosition();

		// The end of the body fragment is trimmed: it must not carry a
		// paragraph break into the target line, the trailing marks have to
		// stay on the same line as the message text.
		int newBodyEnd = newToken.selectionStart();
		while (newBodyEnd>newBodyStart.selectionStart())
		{
			const QChar lastChar = document->characterAt(newBodyEnd-1);
			if (lastChar!=QChar(0x2029) && lastChar!='\n' && lastChar!='\r' && !lastChar.isSpace())
				break;
			newBodyEnd--;
		}
		if (newBodyEnd <= newBodyStart.selectionStart())
			newBodyEnd = newToken.selectionStart();

		QTextCursor newBodyCursor(document);
		newBodyCursor.setPosition(newBodyStart.selectionStart());
		newBodyCursor.setPosition(newBodyEnd,QTextCursor::KeepAnchor);
		const QTextDocumentFragment newBodyFragment = newBodyCursor.selection();

		// The trailing marks of the corrected message are moved together
		// with the body, so the receipt and the chat marker plugins will
		// still find their tokens and will show the marks for the new
		// version of the message.
		QTextCursor newBundleCursor(document);
		newBundleCursor.setPosition(newToken.selectionStart());
		newBundleCursor.setPosition(newBundleEnd,QTextCursor::KeepAnchor);
		const QTextDocumentFragment newBundleFragment = newBundleCursor.selection();

		// The chat markers plugin consumes the sent message ids
		// sequentially: if the peer acknowledges the versions which were
		// sent before the correction (or references the old id), the token
		// of the replaced version has to stay present in the document.
		const QString oldDisplayedId = ATrace->slotList.at(targetIndex).latestId;

		// The appended correction block is not displayed any more.
		QTextCursor appendCursor(document);
		appendCursor.setPosition(appendContentStart);
		appendCursor.movePosition(QTextCursor::End,QTextCursor::KeepAnchor);
		appendCursor.removeSelectedText();

		// The displayed body of the corrected message is replaced with the
		// corrected body, the old trailing marks are replaced with the
		// marks of the corrected message, and the header stays untouched.
		QTextCursor replaceCursor(document);
		replaceCursor.setPosition(oldBodyStart.selectionStart());
		replaceCursor.setPosition(oldBlockEnd,QTextCursor::KeepAnchor);
		// The marker tokens of the thread versions whose read marker has
		// not arrived yet stay valid: they are rescued from the removed
		// content and are kept inside the restored line.
		const QStringList rescuedTokens = rescuedMarkerTokens(replaceCursor,ABrowser,AMessageId);
		replaceCursor.removeSelectedText();
		replaceCursor.insertFragment(newBodyFragment);
		replaceCursor.insertFragment(newBundleFragment);
		// The rescued and the old version's marker tokens are placed after
		// the bundle, inside the right-tab aligned zone together with the
		// new marks: a struck check before the receipt tab would look
		// unaligned.
		for (int i=0; i<rescuedTokens.count(); i++)
			replaceCursor.insertText(rescuedTokens.at(i));
		if (!oldDisplayedId.isEmpty() && oldDisplayedId!=AMessageId
			&& !rescuedTokens.contains(displayedMarkerToken(oldDisplayedId)))
			replaceCursor.insertText(displayedMarkerToken(oldDisplayedId));
		appendEditedNote(ABrowser,replaceCursor);

		// The end token of the replaced block pins the trailing marks of
		// the new version now.
		ATrace->slotList[targetIndex].endToken = ATrace->slotList.at(appendIndex).endToken;
		ATrace->slotList[targetIndex].latestId = AMessageId;
		ATrace->slotList.removeAt(appendIndex);
		pruneContentTraces();
		LOG_INFO(QString("[MessageCorrect] message body replaced in place, id=%1").arg(AMessageId));
		// TEMP DIAGNOSTIC
		LOG_INFO(QString("[MessageCorrect] marker tokens in document, old=%1, new=%2").arg(
			document->find(displayedMarkerToken(oldDisplayedId)).isNull()?"missing":"present",
			document->find(displayedMarkerToken(AMessageId)).isNull()?"missing":"present"));
		ABrowser->verticalScrollBar()->setSliderPosition(scrollPosition);
		return true;
	}

	// The bodies can not be located in the document: move the whole
	// appended block of the corrected message to the place of the old
	// message. The replaced block spans the whole content block of the
	// corrected message: its header with the shown version of the sender
	// and time.
	LOG_INFO(QString("[MessageCorrect] patch fallback: message body not located, id=%1, target=%2").arg(AMessageId,listedId));
	if (targetIndex <= 0)
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: corrected message not displayed, id=%1, target=%2").arg(AMessageId,listedId));
		return false;
	}
	QTextCursor targetPrevToken = document->find(ATrace->slotList.at(targetIndex-1).endToken);
	if (targetPrevToken.isNull())
	{
		LOG_INFO(QString("[MessageCorrect] patch fallback: target tokens missing, id=%1").arg(AMessageId));
		return false;
	}

	const int scrollPosition = ABrowser->verticalScrollBar()->sliderPosition();

	// The old version marker token is kept for the sequential check of the
	// chat markers plugin (see the note in the replacement branch above).
	const QString oldDisplayedId = ATrace->slotList.at(targetIndex).latestId;

	QTextCursor moveCursor(document);
	moveCursor.setPosition(appendContentStart);
	moveCursor.movePosition(QTextCursor::End,QTextCursor::KeepAnchor);
	if (moveCursor.hasSelection())
	{
		const QTextDocumentFragment fragment = moveCursor.selection();
		moveCursor.removeSelectedText();

		QTextCursor targetCursor(document);
		targetCursor.setPosition(targetPrevToken.selectionEnd());
		targetCursor.setPosition(oldBlockEnd,QTextCursor::KeepAnchor);
		// The removed target content may carry the marker tokens of the
		// older versions (see the note in the replacement branch above).
		const QStringList rescuedTokens = rescuedMarkerTokens(targetCursor,ABrowser,AMessageId);
		targetCursor.removeSelectedText();
		targetCursor.insertFragment(fragment);
		for (int i=0; i<rescuedTokens.count(); i++)
			targetCursor.insertText(rescuedTokens.at(i));
		if (!oldDisplayedId.isEmpty() && oldDisplayedId!=AMessageId
			&& !rescuedTokens.contains(displayedMarkerToken(oldDisplayedId)))
			targetCursor.insertText(displayedMarkerToken(oldDisplayedId));
		appendEditedNote(ABrowser,targetCursor);

		// The identity of the replaced block points to the new version, and
		// its end token pins the trailing marks of the new version now.
		ATrace->slotList[targetIndex].endToken = ATrace->slotList.at(appendIndex).endToken;
		ATrace->slotList[targetIndex].latestId = AMessageId;
		ATrace->slotList.removeAt(appendIndex);
		pruneContentTraces();
		LOG_INFO(QString("[MessageCorrect] message patched in place, id=%1").arg(AMessageId));
	}

	// The document was cut and recombined: restore the view position.
	ABrowser->verticalScrollBar()->setSliderPosition(scrollPosition);
	return true;
}

void MessageCorrect::onChatWindowCreated(IMessageChatWindow *AWindow)
{
	if (AWindow!=NULL && AWindow->editWidget()!=NULL)
	{
		QObject *editor = AWindow->editWidget()->instance();
		const bool keyConnected = connect(editor,SIGNAL(keyEventReceived(QKeyEvent *,bool &)),SLOT(onEditorKeyEvent(QKeyEvent *,bool &)));
		const bool menuConnected = connect(editor,SIGNAL(contextMenuRequested(const QPoint &,Menu *)),SLOT(onEditorContextMenuRequested(const QPoint &,Menu *)));
		const bool sentConnected = connect(editor,SIGNAL(messageSent()),SLOT(onEditorMessageSent()));
		LOG_INFO(QString("[MessageCorrect] chat window hooked, contact=%1, connections=%2%3%4").arg(AWindow->contactJid().full()).arg(keyConnected).arg(menuConnected).arg(sentConnected));
	}
}

void MessageCorrect::onEditorKeyEvent(QKeyEvent *AKeyEvent, bool &AHook)
{
	IMessageEditWidget *editor = qobject_cast<IMessageEditWidget *>(sender());
	IMessageChatWindow *window = findEditWindow(sender());
	if (editor==NULL || window==NULL || !isEnabled())
		return;

	const QString key = chatKey(window->streamJid(),window->contactJid());

	if (AKeyEvent->key()==Qt::Key_Escape && FEditSessions.contains(key))
	{
		AHook = true;
		cancelMessageEditing(editor,key);
	}
	else if (AKeyEvent->key()==Qt::Key_Up && AKeyEvent->modifiers()==Qt::NoModifier
		&& !FOutgoingRecords.value(key).isEmpty())
	{
		// An empty editor with Up loads the latest sent message. Further Up
		// presses change nothing: only the last message is correctable.
		QTextDocument *document = editor->document();
		if (document!=NULL && (FEditSessions.contains(key) || document->toPlainText().isEmpty()))
		{
			AHook = true;
			if (!FEditSessions.contains(key))
				startMessageEditing(editor,key);
		}
	}
	else if (AKeyEvent->key()==Qt::Key_Down && AKeyEvent->modifiers()==Qt::NoModifier
		&& FEditSessions.contains(key))
	{
		// Down leaves the correcting state and returns the empty editor.
		AHook = true;
		cancelMessageEditing(editor,key);
	}
}

void MessageCorrect::onEditorContextMenuRequested(const QPoint &APosition, Menu *AMenu)
{
	Q_UNUSED(APosition);

	IMessageEditWidget *editor = qobject_cast<IMessageEditWidget *>(sender());
	IMessageChatWindow *window = findEditWindow(sender());
	if (editor==NULL || window==NULL || AMenu==NULL || !isEnabled())
		return;

	const QString key = chatKey(window->streamJid(),window->contactJid());
	QTextDocument *document = editor->document();
	if (FOutgoingRecords.contains(key) && document!=NULL && document->toPlainText().isEmpty())
	{
		Action *action = new Action(AMenu);
		action->setText(tr("Edit last message"));
		action->setData(0,key);
		connect(action,SIGNAL(triggered(bool)),SLOT(onEditMessageActionTriggered()));
		AMenu->addAction(action);
	}
}

void MessageCorrect::onEditorMessageSent()
{
	// The editor message buffer state is not relevant after sending: hide
	// the editing hint even when the send consumed another session.
	IMessageEditWidget *editor = qobject_cast<IMessageEditWidget *>(sender());
	if (editor!=NULL && editor->textEdit()!=NULL)
	{
		editor->textEdit()->setPlaceholderText(QString());
		setEditorCorrectionMode(editor,false);
	}
}

void MessageCorrect::onEditMessageActionTriggered()
{
	Action *action = qobject_cast<Action *>(sender());
	if (action == NULL)
		return;

	const QString key = action->data(0).toString();
	foreach(IMessageChatWindow *window, FMessageWidgets!=NULL ? FMessageWidgets->chatWindows() : QList<IMessageChatWindow *>())
	{
		if (window->editWidget()!=NULL && chatKey(window->streamJid(),window->contactJid())==key)
		{
			startMessageEditing(window->editWidget(),key);
			return;
		}
	}
}
