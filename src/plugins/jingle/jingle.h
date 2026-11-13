#ifndef JINGLE_H
#define JINGLE_H

#include <QHash>
#include <QObject>
#include <QUuid>

#include <interfaces/ijingle.h>
#include <interfaces/ipluginmanager.h>
#include <interfaces/irostermanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/ixmppstreammanager.h>

#include "jinglesession.h"

#define JINGLE_UUID "DB64AD0A-5ED3-450D-AC1A-FC4F7745F8D5"

class Jingle :
	public QObject,
	public IPlugin,
	public IJingle,
	public IStanzaHandler,
	public IStanzaRequestOwner,
	public IDiscoFeatureHandler,
	public IOptionsDialogHolder
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IJingle IStanzaHandler IStanzaRequestOwner IDiscoFeatureHandler IOptionsDialogHolder);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.Jingle");
public:
	Jingle();
	virtual ~Jingle();
	// IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(JINGLE_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin() { return true; }
	// IJingle
	virtual bool isSupported(const Jid &AStreamJid, const Jid &AContactJid) const;
	virtual QString call(const Jid &AStreamJid, const Jid &AContactJid);
	virtual bool answerCall(const QString &ASid);
	virtual bool declineCall(const QString &ASid);
	virtual bool hangupCall(const QString &ASid);
	virtual IJingle::CallState callState(const QString &ASid) const;
	virtual Jid callPeer(const QString &ASid) const;
	// IStanzaHandler
	virtual bool stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept);
	// IStanzaRequestOwner
	virtual void stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza);
	// IDiscoFeatureHandler
	virtual bool execDiscoFeature(const Jid &AStreamJid, const QString &AFeature, const IDiscoInfo &ADiscoInfo);
	virtual Action *createDiscoFeatureAction(const Jid &AStreamJid, const QString &AFeature, const IDiscoInfo &ADiscoInfo, QWidget *AParent);
	// IOptionsDialogHolder
	virtual QMultiMap<int, IOptionsDialogWidget *> optionsDialogWidgets(const QString &ANodeId, QWidget *AParent);
	//Session transport helpers (used by JingleSession)
	bool sendSessionAction(JingleSession *ASession, Stanza &AStanza);
	void sendSessionRequest(JingleSession *ASession, Stanza &AStanza);
	Stanza makeResultReply(const Stanza &AStanza) const;
	Stanza makeErrorReply(const Stanza &AStanza, const XmppStanzaError &AError) const;
	QString peerName(const Jid &AStreamJid, const Jid &AContactJid) const;
signals:
	// IJingle notifications
	void callStarted(const Jid &AStreamJid, const Jid &AContactJid, const QString &ASid);
	void callReceived(const Jid &AStreamJid, const Jid &AContactJid, const QString &ASid);
	void callStateChanged(const QString &ASid, IJingle::CallState AState);
	void callFinished(const QString &ASid, const QString &AReason);
private slots:
	void onStreamOpened(IXmppStream *AXmppStream);
	void onStreamClosed(IXmppStream *AXmppStream);
	void onCallByAction(bool AChecked);
	void onSessionStateChanged(JingleSession *ASession, IJingle::CallState AState);
	void onSessionFinished(JingleSession *ASession, const QString &AReason, const QString &AText);
private:
	JingleSession *findSession(const QString &ASid) const;
	JingleSession *findSession(const Jid &AStreamJid, const QString &ASid) const;
	JingleSession *findActiveSession(const Jid &AStreamJid, const Jid &AContactJid) const;
	bool processSessionInitiate(const Jid &AStreamJid, const Stanza &AStanza, const QDomElement &AJingleElem, bool &AAccept);
	void removeSession(JingleSession *ASession);
	JingleSession *createSession(const Jid &AStreamJid, const Jid &AContactJid, const Jid &ALocalJid, const QString &AInitiatorJid, const QString &ASid, bool AInitiated);
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IXmppStreamManager *FXmppStreamManager;
	IServiceDiscovery *FDiscovery;
	IOptionsManager *FOptionsManager;
	IRosterManager *FRostersManager;
	QHash<Jid, QList<int> > FStanzaHandles;    //streamJid -> registered stanza handle ids
	QList<JingleSession *> FSessions;
	QHash<QString, QString> FPendingRequests;  //iq id -> sid of a pending session-initiate
};

#endif // JINGLE_H
