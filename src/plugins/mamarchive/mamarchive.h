#ifndef MAMARCHIVE_H
#define MAMARCHIVE_H

#include <QObject>
#include <QMap>
#include <QList>
#include <QSet>
#include <QPair>
#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QDomElement>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessagearchiver.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/ixmppstreammanager.h>
#include <interfaces/iaccountmanager.h>
#include <utils/message.h>
#include <utils/stanza.h>

#define MAMARCHIVE_UUID "{4B9E7B34-2C61-4B0E-9F3A-8D5C1E7A6F02}"

struct MamQueryRequest {
	QString id;
	Jid streamJid;
	QString namespaceUri;
	bool headers;
	IArchiveRequest request;
	IArchiveHeader header;
	QList<Message> messages;
	QString first;
	QString last;
	bool complete;
	int pages;
	int maxPages;
	QString iqId;
	MamQueryRequest() {
		headers = true;
		complete = false;
		pages = 0;
		maxPages = 0;
	}
};

struct MamCachedCollection {
	QList<Message> messages;
	QDateTime end;
};

class MamArchive :
	public QObject,
	public IPlugin,
	public IArchiveEngine,
	public IArchiveHandler,
	public IStanzaHandler,
	public IStanzaRequestOwner
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IArchiveEngine IArchiveHandler IStanzaHandler IStanzaRequestOwner);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MamArchive");
public:
	MamArchive();
	~MamArchive();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MAMARCHIVE_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings() { return true; }
	virtual bool startPlugin() { return true; }
	//IStanzaHandler
	virtual bool stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept);
	//IStanzaRequestOwner
	virtual void stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza);
	//IArchiveHandler
	virtual bool archiveMessageEdit(int AOrder, const Jid &AStreamJid, Message &AMessage, bool ADirectionIn);
	//IArchiveEngine
	virtual QUuid engineId() const;
	virtual QString engineName() const;
	virtual QString engineDescription() const;
	virtual IOptionsDialogWidget *engineSettingsWidget(QWidget *AParent);
	virtual quint32 capabilities(const Jid &AStreamJid = Jid::null) const;
	virtual bool isCapable(const Jid &AStreamJid, quint32 ACapability) const;
	virtual int capabilityOrder(quint32 ACapability, const Jid &AStreamJid = Jid::null) const;
	virtual bool saveMessage(const Jid &AStreamJid, const Message &AMessage, bool ADirectionIn);
	virtual bool saveNote(const Jid &AStreamJid, const Message &AMessage, bool ADirectionIn);
	virtual QString saveCollection(const Jid &AStreamJid, const IArchiveCollection &ACollection);
	virtual QString loadHeaders(const Jid &AStreamJid, const IArchiveRequest &ARequest);
	virtual QString loadCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader);
	virtual QString removeCollections(const Jid &AStreamJid, const IArchiveRequest &ARequest);
	virtual QString loadModifications(const Jid &AStreamJid, const QDateTime &AStart, int ACount, const QString &ANextRef);
signals:
	//IArchiveEngine
	void capabilitiesChanged(const Jid &AStreamJid);
	void requestFailed(const QString &AId, const XmppError &AError);
	void headersLoaded(const QString &AId, const QList<IArchiveHeader> &AHeaders);
	void collectionSaved(const QString &AId, const IArchiveCollection &ACollection);
	void collectionLoaded(const QString &AId, const IArchiveCollection &ACollection);
	void collectionsRemoved(const QString &AId, const IArchiveRequest &ARequest);
	void modificationsLoaded(const QString &AId, const IArchiveModifications &AModifications);
protected slots:
	void onXmppStreamOpened(IXmppStream *AXmppStream);
	void onXmppStreamClosed(IXmppStream *AXmppStream);
	void onDiscoInfoReceived(const IDiscoInfo &AInfo);
	void onDeferredTimerTimeout();
protected:
	QString mamNamespace(const Jid &AStreamJid) const;
	QString startIq(MamQueryRequest &ARequest);
	void processQueryResult(const Jid &AStreamJid, const Stanza &AStanza);
	void finishQuery(MamQueryRequest &ARequest, const XmppError &AError = XmppError());
	void appendStanzaResult(const Jid &AStreamJid, const QDomElement &AResultElem);
	Message resultToMessage(const Jid &AStreamJid, const QDomElement &AForwarded, const QDomElement &AMessageElem) const;
	Jid messagePartner(const Jid &AStreamJid, const Message &AMessage) const;
	QList<IArchiveHeader> groupHeaders(const Jid &AStreamJid, const QList<Message> &AMessages);
	void cacheCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader, const QList<Message> &AMessages, const QDateTime &AEnd);
	bool findCachedCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader, QList<Message> &AMessages, QDateTime &AEnd) const;
	QString collectionKey(const Jid &AStreamJid, const IArchiveHeader &AHeader) const;
	void emitCollectionDeferred(const QString &AId, const IArchiveCollection &ACollection);
	bool isServerMamActive(const Jid &AStreamJid) const;
	bool isArchiveDuplicationEnabled(const Jid &AStreamJid) const;
private:
	IMessageArchiver *FArchiver;
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IXmppStreamManager *FXmppStreamManager;
	IAccountManager *FAccountManager;
private:
	QMap<Jid,int> FSHIResults;
	QMap<Jid,bool> FSupported;
	QSet<Jid> FAccountDiscoRequested;
private:
	QMap<QString,MamQueryRequest> FQueries;
private:
	QMap<QString,MamCachedCollection> FCollections;
	QList<QString> FCollectionsOrder;
private:
	// End timestamps of the sessions served by MAM. Kept separately so a
	// collection can still be re-requested with a bounded window after the
	// cached messages were evicted.
	QMap<QString,QDateTime> FSessionEnds;
	QList<QString> FSessionEndsOrder;
private:
	QTimer FDeferredTimer;
	QList<QPair<QString,IArchiveCollection> > FDeferredCollections;
};

#endif // MAMARCHIVE_H
