#ifndef JINGLE_H
#define JINGLE_H

#include <QHash>
#include <QObject>
#include <QUuid>

#include <interfaces/ijingle.h>
#include <interfaces/imessagearchiver.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/inotifications.h>
#include <interfaces/ipluginmanager.h>
#include <interfaces/irostermanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/ixmppstreammanager.h>

#include "jinglesession.h"
#include "natservice.h"

class Action;

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
	bool isVideoSupported(const Jid &AStreamJid, const Jid &AContactJid) const;
	virtual QString call(const Jid &AStreamJid, const Jid &AContactJid);
	QString callWithVideo(const Jid &AStreamJid, const Jid &AContactJid);
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
	//Records the outcome of a call in the chat with the contact: a one-line note
	//that the message handler renders as a status line and the archive keeps.
	void reportCallEvent(const Jid &AStreamJid, const Jid &AContactJid, const QString &AText);
	//Sounds shared by all sessions (the ringtone must outlive the session that
	//ends the call, otherwise the call-ended tone is cut by the session deletion)
	CallSounds *callSounds() const { return FSounds; }
	//Effective STUN/TURN configuration for a stream: the manual options first,
	//then the services discovered with XEP-0215.
	NatConfig natConfig(const Jid &AStreamJid) const;
	//XEP-0353 Jingle Message Initiation: <propose>/<proceed>/<finish> messages
	bool sendJmiMessage(const Jid &AStreamJid, const Jid &ATo, const QString &AAction, const QString &ASid,
		const QString &AReasonCode = QString(), const QString &AText = QString(), bool AWithVideo = false);
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
	void onVideoCallByAction(bool AChecked);
	void onChatCallByAction(bool AChecked);
	void onChatVideoCallByAction(bool AChecked);
	void onToolBarWidgetCreated(IMessageToolBarWidget *AWidget);
	void onToolBarWidgetDestroyed(QObject *AObject);
	void updateToolBarActions();
	void onSessionStateChanged(JingleSession *ASession, IJingle::CallState AState);
	void onSessionFinished(JingleSession *ASession, const QString &AReason, const QString &AText);
private:
	JingleSession *findSession(const QString &ASid) const;
	JingleSession *findSession(const Jid &AStreamJid, const QString &ASid) const;
	JingleSession *findActiveSession(const Jid &AStreamJid, const Jid &AContactJid) const;
	void updateToolBarAction(IMessageToolBarWidget *AWidget);
	bool processSessionInitiate(const Jid &AStreamJid, const Stanza &AStanza, const QDomElement &AJingleElem, bool &AAccept);
	bool processJmiMessage(const Jid &AStreamJid, const Stanza &AStanza, const QDomElement &AJmiElem, const QString &AAction);
	bool processNatPush(const Jid &AStreamJid, const Stanza &AStanza, bool &AAccept);
	void requestNatServices(const Jid &AStreamJid);
	void requestNatCredentials(const Jid &AStreamJid, const NatService &AService);
	void onNatServicesResult(const Jid &AStreamJid, const Stanza &AStanza);
	void onNatCredentialsResult(const Jid &AStreamJid, const Stanza &AStanza, const NatService &AService);
	void removeSession(JingleSession *ASession);
	JingleSession *createSession(const Jid &AStreamJid, const Jid &AContactJid, const Jid &ALocalJid, const QString &AInitiatorJid, const QString &ASid, bool AInitiated, bool AWithVideo = false);
	QString startCall(const Jid &AStreamJid, const Jid &AContactJid, bool AWithVideo);
private:
	IPluginManager *FPluginManager;
	IStanzaProcessor *FStanzaProcessor;
	IXmppStreamManager *FXmppStreamManager;
	IServiceDiscovery *FDiscovery;
	IOptionsManager *FOptionsManager;
	IRosterManager *FRostersManager;
	IMessageWidgets *FMessageWidgets;
	IMessageArchiver *FMessageArchiver;
	INotifications *FNotifications;
	QHash<IMessageToolBarWidget *, Action *> FToolBarActions;
	QHash<IMessageToolBarWidget *, Action *> FToolBarVideoActions;
	QList<IMessageToolBarWidget *> FToolBarWidgets;
	QHash<Jid, QList<int> > FStanzaHandles;    //streamJid -> registered stanza handle ids
	QList<JingleSession *> FSessions;
	CallSounds *FSounds;
	QHash<QString, QString> FPendingRequests;  //iq id -> sid of a pending session-initiate
	QHash<QString, QList<NatService> > FNatServices; //stream full jid -> known external services
	QHash<QString, Jid> FNatServiceRequests;   //iq id -> stream full jid of a discovery request
	QHash<QString, NatService> FNatCredentialRequests; //iq id -> service awaiting credentials
};

#endif // JINGLE_H
