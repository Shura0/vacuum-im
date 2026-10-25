#ifndef MESSAGERECEIPTS_H
#define MESSAGERECEIPTS_H

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QDateTime>
#include <QTimer>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/imessagestylemanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/ixmppstreammanager.h>
#include <interfaces/imessagewidgets.h>
#include <utils/message.h>
#include <utils/stanza.h>

#define MESSAGERECEIPTS_UUID "{A4F4D311-4E5B-4E45-9DD8-9B4B4C81B0F3}"

class QTextBrowser;

// Kind of message the plugin acknowledges automatically (XEP-0184 §2.2.2):
// chat messages with a body and a stanza id, requested by the sender.
class MessageReceipts :
	public QObject,
	public IPlugin,
	public IMessageEditor,
	public IMessageStyleHandler
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IMessageEditor IMessageStyleHandler);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MessageReceipts");
public:
	MessageReceipts();
	~MessageReceipts();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MESSAGERECEIPTS_UUID); }
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
signals:
	//Message Delivery Receipts
	void messageReceiptRequested(const Jid &AStreamJid, const Message &AMessage);
	void messageReceiptReceived(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid);
protected:
	enum MarkerState {
		MarkerPending,
		MarkerDelivered
	};
	struct MarkerRecord
	{
		QString chatKey;
		qint64 time;
		int state;
	};
	bool isOutgoingRequestable(const Jid &AStreamJid, const Message &AMessage) const;
	bool isReceiptRequest(const Jid &AStreamJid, const Message &AMessage) const;
	void sendMessageReceipt(const Jid &AStreamJid, const Message &AMessage) const;
	void showMessageReceipt(const Jid &AStreamJid, const QString &AMessageId, const Jid &AContactJid);
	bool isSupportedByContact(const Jid &AStreamJid, const Jid &AContactJid) const;
	void applyReceiptMark(QTextBrowser *ABrowser, const QString &AMessageId, bool ADelivered) const;
	// Persistent state of the delivery marks. It is stored separately from the
	// history archive because a receipt arrives after the message was saved.
	QString markerStateFilePath() const;
	void loadMarkerRecords();
	void scheduleMarkerRecordsSave();
	void saveMarkerRecords() const;
	void pruneMarkerRecords();
	void setMarkerRecord(const Jid &AStreamJid, const Jid &AContactJid, const QString &AMessageId, const QDateTime &ATime, int AState);
	bool markerRecordState(const QString &AMessageId, int &AState);
	static QString markerChatKey(const Jid &AStreamJid, const Jid &AContactJid);
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IMessageProcessor *FMessageProcessor;
	IMessageWidgets *FMessageWidgets;
private:
	bool FMarkerRecordsLoaded;
	QHash<QString, MarkerRecord> FMarkerRecords;
	QTimer FSaveTimer;
	mutable QSet<QString> FRequestedDisco;
};

#endif // MESSAGERECEIPTS_H