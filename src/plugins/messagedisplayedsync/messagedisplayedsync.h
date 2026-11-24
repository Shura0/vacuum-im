#ifndef MESSAGEDISPLAYEDSYNC_H
#define MESSAGEDISPLAYEDSYNC_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QDateTime>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/imessagestylemanager.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/imessagearchiver.h>
#include <interfaces/ixmppstreammanager.h>
#include <interfaces/ipepmanager.h>
#include <utils/jid.h>
#include <utils/message.h>
#include <utils/stanza.h>

#define MESSAGEDISPLAYEDSYNC_UUID "{C662A412-AA44-4B62-972B-B631519B55F3}"

class IMessageChatWindow;

// XEP-0490 Message Displayed Synchronization (sending side only).
//
// Vacuum tracks the XEP-0359 <stanza-id/> of the messages it receives and,
// whenever a chat is actually shown to the user, publishes the identifier of
// the newest received message into the 'urn:xmpp:mds:displayed:0' PEP node.
// The other clients of the same account (Snikket, Conversations, ...) listen
// to that node and clear their unread markers, so the displayed state stays
// synchronized across devices.
//
// Only the outgoing half of the specification is implemented: the plugin
// publishes its own state but ignores the states published by the other
// clients. That is enough to make a reading client clear the unread marks of
// the others, and it can not accidentally overwrite a newer state.
class MessageDisplayedSync :
	public QObject,
	public IPlugin,
	public IMessageEditor,
	public IMessageStyleHandler,
	public IOptionsDialogHolder,
	public IStanzaRequestOwner,
	public IPEPHandler
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IMessageEditor IMessageStyleHandler IOptionsDialogHolder IStanzaRequestOwner IPEPHandler);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MessageDisplayedSync");
public:
	MessageDisplayedSync();
	~MessageDisplayedSync();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MESSAGEDISPLAYEDSYNC_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin() { return true; }
	//IMessageEditor
	virtual bool messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection);
	//IMessageStyleHandler
	virtual bool messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions);
	virtual QString messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions);
	//IOptionsDialogHolder
	virtual QMultiMap<int, IOptionsDialogWidget *> optionsDialogWidgets(const QString &ANodeId, QWidget *AParent);
	//IStanzaRequestOwner
	virtual void stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza);
	//IPEPHandler
	virtual bool processPEPEvent(const Jid &AStreamJid, const Stanza &AStanza);
signals:
	void displayedStatePublished(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId);
	void displayedStateReceived(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId);
protected:
	struct ChatState
	{
		Jid streamJid;
		Jid contactJid;
		QString stanzaId;
		qint64 time = 0;
		QString published;
		qint64 publishedTime = 0;
		QString failed;
		QString received;
		qint64 receivedTime = 0;
	};
	struct PublishRequest
	{
		QString chatKey;
		QString stanzaId;
		qint64 time;
	};
	enum DiscoSupport {
		DiscoUnsupported,
		DiscoUnknown,
		DiscoSupported
	};
protected:
	bool isEnabled() const;
	DiscoSupport publishOptionsSupport(const Jid &AStreamJid);
	void recordStanzaId(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId, const QDateTime &ATime);
	void schedulePublish(const Jid &AStreamJid, const Jid &AContactJid);
	void publishDisplayedState(const QString &AChatKey);
	bool sendDisplayedState(const QString &AChatKey, const ChatState &AState);
	Jid activeStreamJid(const Jid &ABareJid) const;
	void subscribeAndCatchUp(const Jid &AStreamJid);
	void processDisplayedItems(const Jid &AStreamJid, const QDomElement &AItems);
	void applyDisplayedState(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId);
	bool isChatWindowDisplayed(IMessageChatWindow *AWindow) const;
	IMessageChatWindow *findChatWindow(QWidget *AStyleWidget) const;
	static QString chatKey(const Jid &AStreamJid, const Jid &AContactJid);
	// Persistent state. The stanza ids of the received messages and the last
	// published state must survive a restart, otherwise the oldest history
	// would be published again after every start.
	QString stateFilePath() const;
	void loadState();
	void scheduleStateSave();
	void saveState() const;
	void pruneState();
protected slots:
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onChatWindowActivated();
	void onTabWindowCreated(IMessageTabWindow *AWindow);
	void onCurrentTabPageChanged(IMessageTabPage *APage);
	void onCollectionLoaded(const QString &AId, const IArchiveCollection &ACollection);
	void onDiscoInfoReceived(const IDiscoInfo &AInfo);
	void onStreamOpened(IXmppStream *AStream);
	void onStreamClosed(IXmppStream *AStream);
	void onPublishTimeout();
	void onStateSaveTimeout();
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IMessageProcessor *FMessageProcessor;
	IMessageWidgets *FMessageWidgets;
	IOptionsManager *FOptionsManager;
	IMessageArchiver *FArchiver;
	IXmppStreamManager *FStreamManager;
	IPEPManager *FPEPManager;
private:
	bool FStateLoaded;
	int FPEPHandleId;
	QSet<QString> FRequestedDisco;
	QSet<QString> FPublishOptionsStreams;
	QHash<QString, ChatState> FChatStates;
	QHash<QString, PublishRequest> FPublishRequests;
	QHash<QString, QString> FCatchUpRequests;
	QSet<QString> FSubscribedStreams;
	QSet<QString> FPendingPublishes;
	QHash<QString, int> FRetryAttempts;
	QTimer FPublishTimer;
	QTimer FSaveTimer;
};

#endif // MESSAGEDISPLAYEDSYNC_H
