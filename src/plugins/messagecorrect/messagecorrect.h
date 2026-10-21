#ifndef MESSAGECORRECT_H
#define MESSAGECORRECT_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QList>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/imessagestylemanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/ioptionsmanager.h>
#include <utils/message.h>
#include <utils/stanza.h>

#define MESSAGECORRECT_UUID "{58E83C93-1B79-4B28-9666-1F28C7E9B0A2}"

class QTextBrowser;


// XEP-0308 Last Message Correction for one-to-one chats.
//
// The user puts one of the recent outgoing messages back into the editor
// (Up key in an empty editor or the editor context menu) and sends it. The
// sent message carries a <replace id='...'/> element with the stanza id of
// the last shown version of that message, and the receiver must replace
// the text of the original message instead of showing another message.
//
// Every content appended by a document-based message style (a QTextBrowser
// view) is marked with invisible per-content pins. When a correction is
// appended to the end of a chat view, its whole content block is moved to
// the place of the old message body, so the conversation keeps a single
// message per id with a small "edited" note. If the original message is
// not visible (no window open, cleared view, a non-document style) the
// correction falls back to the regular display as a separate message.
struct OutgoingRecord
{
	QString messageId;
	QString body;
};

// A content block appended to a document-based chat view. An invisible
// token after the content html pins the end of the block, so the block
// (between the previous block end and this token) can be located and
// replaced later.
struct ContentSlot
{
	QString endToken;
	QString originalId;
	QString latestId;
};

// All content blocks appended to a single style widget in display order.
struct ContentTrace
{
	QPointer<QWidget> widget;
	QList<ContentSlot> slotList;
};

class MessageCorrect :
	public QObject,
	public IPlugin,
	public IMessageEditor,
	public IMessageStyleHandler,
	public IOptionsDialogHolder
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IMessageEditor IMessageStyleHandler IOptionsDialogHolder);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MessageCorrect");
public:
	MessageCorrect();
	~MessageCorrect();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MESSAGECORRECT_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin() { return true; }
	//IMessageEditor
	virtual bool messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection);
	//IMessageStyleHandler
	virtual bool messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions);
	//IOptionsDialogHolder
	virtual QMultiMap<int, IOptionsDialogWidget *> optionsDialogWidgets(const QString &ANodeId, QWidget *AParent);
protected: // correction protocol
	bool isEnabled() const;
	bool isOutgoingCorrection(const Jid &AStreamJid, const Message &AMessage) const;
	bool isIncomingCorrection(const Jid &AStreamJid, const Message &AMessage) const;
	bool isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const;
	void setCorrectionTarget(const QString &AMessageId, const QString &ATargetId);
	void rememberMessageBody(const QString &AMessageId, const QString &ABody);
protected: // editing outgoing messages
	QString chatKey(const Jid &AStreamJid, const Jid &AContactJid) const;
	void storeOutgoingRecord(const Jid &AStreamJid, const Message &AMessage, const QString &ATargetId);
	IMessageChatWindow *findEditWindow(QObject *AEditorInstance) const;
	void setEditorMessage(IMessageEditWidget *AEditor, const OutgoingRecord &ARecord);
	bool startMessageEditing(IMessageEditWidget *AEditor, const QString &AChatKey);
	void cancelMessageEditing(IMessageEditWidget *AEditor, const QString &AChatKey);
	void setEditorCorrectionMode(IMessageEditWidget *AEditor, bool AOn);
	QStringList rescuedMarkerTokens(QTextCursor &ARemoveCursor, QWidget *AStyleWidget, const QString &ANewMessageId) const;
protected: // displayed content slots
	ContentTrace *findContentTrace(QWidget *AWidget);
	void pruneContentTraces();
	void registerContentSlot(QTextBrowser *ABrowser, const QString &AMessageId, QString &AModifiedHtml);
	void patchCorrectedMessage(const QPointer<QWidget> &AWidget, const QString &AMessageId);
	bool patchCorrectedMessage(QTextBrowser *ABrowser, ContentTrace *ATrace, const QString &AMessageId);
protected slots:
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onEditorKeyEvent(QKeyEvent *AKeyEvent, bool &AHook);
	void onEditorContextMenuRequested(const QPoint &APosition, Menu *AMenu);
	void onEditMessageActionTriggered();
	void onEditorMessageSent();
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IMessageProcessor *FMessageProcessor;
	IMessageWidgets *FMessageWidgets;
	IOptionsManager *FOptionsManager;
private:
	mutable QSet<QString> FRequestedDisco;
	// Recent outgoing messages per chat, oldest first.
	QHash<QString, QList<OutgoingRecord> > FOutgoingRecords;
	// Chat key -> index in the records list the editor currently shows.
	QHash<QString, int> FEditSessions;
	// Message id -> the id of the message version it corrects.
	QHash<QString, QString> FCorrectionTargets;
	QList<QString> FCorrectionOrder;
	// Message id -> the plain text body shown for that message version.
	QHash<QString, QString> FMessageBodies;
	QList<QString> FMessageBodiesOrder;
	// Seeds of the invisible tokens inserted into style documents.
	quint64 FTokenCounter;
	QList<ContentTrace *> FContentTraces;
};

#endif // MESSAGECORRECT_H
