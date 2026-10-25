#ifndef MESSAGECHATMARKERS_H
#define MESSAGECHATMARKERS_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <QList>
#include <QStringList>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/imessagestylemanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/ioptionsmanager.h>
#include <utils/message.h>
#include <utils/stanza.h>

#define MESSAGECHATMARKERS_UUID "{EE444EB7-4046-4986-9EEB-1553D7439C7A}"

class QTextBrowser;

// XEP-0333 Chat Markers (Displayed Markers) for one-to-one chats.
//
// Outgoing chat messages get a <markable/> element and the peer confirms
// that the message was displayed with a <displayed/> marker. Received
// messages with a <markable/> request are acknowledged with a <displayed/>
// marker once the chat window is actually shown to the user.
class MessageChatMarkers :
	public QObject,
	public IPlugin,
	public IMessageEditor,
	public IMessageStyleHandler,
	public IOptionsDialogHolder
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IMessageEditor IMessageStyleHandler IOptionsDialogHolder);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MessageChatMarkers");
public:
	MessageChatMarkers();
	~MessageChatMarkers();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MESSAGECHATMARKERS_UUID); }
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
signals:
	void messageMarkableRequested(const Jid &AStreamJid, const Message &AMessage);
	void messageDisplayedReceived(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid);
	void messageDisplayedSent(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid);
protected:
	struct PendingMarker
	{
		Jid streamJid;
		Jid contactJid;
		QString messageId;
	};
	enum MarkerState {
		MarkerRequested,
		MarkerDisplayed
	};
	struct MarkerRecord
	{
		QString chatKey;
		qint64 time;
		int state;
	};
protected:
	bool isEnabled() const;
	bool isOutgoingMarkable(const Jid &AStreamJid, const Message &AMessage) const;
	bool isIncomingMarkable(const Jid &AStreamJid, const Message &AMessage) const;
	bool isIncomingDisplayed(const Jid &AStreamJid, const Message &AMessage) const;
	bool isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const;
	void sendDisplayedMarker(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId);
	void showDisplayedMarker(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid);
	void storePendingMarker(const Jid &AStreamJid, const Message &AMessage);
	void flushDisplayedMarkers(IMessageChatWindow *AWindow);
	bool isChatWindowDisplayed(IMessageChatWindow *AWindow) const;
	IMessageChatWindow *findChatWindow(QWidget *AStyleWidget) const;
	void patchDisplayedMarker(QTextBrowser *ABrowser, const QString &AMessageId) const;
	// Persistent state of the displayed marks. It is stored separately from
	// the history archive because a chat marker arrives after the message was
	// saved and it must survive an application restart.
	QString markerStateFilePath() const;
	void loadMarkerRecords();
	void scheduleMarkerRecordsSave();
	void saveMarkerRecords() const;
	void pruneMarkerRecords();
	void setMarkerRecord(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId, const QDateTime &ATime, int AState);
	void confirmDisplayedMarkers(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId);
	static QString chatKey(const Jid &AStreamJid, const Jid &AContactJid);
protected slots:
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onChatWindowActivated();
	void onTabWindowCreated(IMessageTabWindow *AWindow);
	void onCurrentTabPageChanged(IMessageTabPage *APage);
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IMessageProcessor *FMessageProcessor;
	IMessageWidgets *FMessageWidgets;
	IOptionsManager *FOptionsManager;
private:
	mutable QSet<QString> FRequestedDisco;
	QHash<QString, PendingMarker> FPendingMarkers;
	bool FMarkerRecordsLoaded;
	QHash<QString, MarkerRecord> FMarkerRecords;
	QTimer FSaveTimer;
};

#endif // MESSAGECHATMARKERS_H
