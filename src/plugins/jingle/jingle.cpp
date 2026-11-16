#include "jingle.h"
#include "callsoptionswidget.h"

#include <QDomDocument>

#include <utils/action.h>
#include <utils/iconstorage.h>
#include <utils/logger.h>
#include <utils/options.h>

#include <definitions/discofeaturehandlerorders.h>
#include <definitions/internalerrors.h>
#include <definitions/menuicons.h>
#include <definitions/namespaces.h>
#include <definitions/optionnodeorders.h>
#include <definitions/optionnodes.h>
#include <definitions/optionvalues.h>
#include <definitions/optionwidgetorders.h>
#include <definitions/resources.h>
#include <definitions/stanzahandlerorders.h>

#define SHC_JINGLE_IQ  "/iq[@type='set']/jingle[@xmlns='" NS_JINGLE "']"
#define SHC_EXTDISCO_IQ  "/iq[@type='set']/services[@xmlns='" NS_EXTERNAL_SERVICES "']"

#define ADR_STREAM_JID            Action::DR_StreamJid
#define ADR_CONTACT_JID           Action::DR_Parametr1

#define JINGLE_INITIATE_TIMEOUT   30000

Jingle::Jingle()
{
	FPluginManager = nullptr;
	FStanzaProcessor = nullptr;
	FXmppStreamManager = nullptr;
	FDiscovery = nullptr;
	FOptionsManager = nullptr;
	FRostersManager = nullptr;
}

Jingle::~Jingle()
{
	//Only what this object owns is released here. The plugin manager unloads
	//plugins in an arbitrary order, so a dependency (the stanza processor, the
	//service discovery) may already be destroyed by now, and calling into it
	//from this destructor dereferences freed memory - that was a crash on every
	//program exit. The per-stream registrations are removed in onStreamClosed().
	qDeleteAll(FSessions);
	FSessions.clear();
}

void Jingle::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Jingle (Voice Calls)");
	APluginInfo->description = tr("Allows to make and receive voice and video calls over XMPP");
	APluginInfo->version = "0.1";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = QUrl("http://www.vacuum-im.org");
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(XMPPSTREAMS_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
}

bool Jingle::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0, nullptr);
	if (plugin)
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IXmppStreamManager").value(0, nullptr);
	if (plugin)
		FXmppStreamManager = qobject_cast<IXmppStreamManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0, nullptr);
	if (plugin)
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IOptionsManager").value(0, nullptr);
	if (plugin)
		FOptionsManager = qobject_cast<IOptionsManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IRosterManager").value(0, nullptr);
	if (plugin)
		FRostersManager = qobject_cast<IRosterManager *>(plugin->instance());

	return FStanzaProcessor!=nullptr && FXmppStreamManager!=nullptr && FDiscovery!=nullptr;
}

bool Jingle::initObjects()
{
	XmppError::registerError(NS_INTERNAL_ERROR, IERR_JINGLE_CALL_FAILED, tr("Failed to start the call"));

	if (FDiscovery)
	{
		//The features peers check before they dare to call us: without the DTLS-SRTP and
		//ICE-UDP entries Swiften-based clients (Dino) refuse the session with a security-error.
		IDiscoFeature djt;
		djt.active = true;
		djt.icon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_JINGLE_CALL);

		djt.var = NS_JINGLE_APPS_RTP;
		djt.name = tr("Voice Calls");
		djt.description = tr("Supports voice and video calls over XMPP");
		FDiscovery->insertDiscoFeature(djt);

		djt.var = NS_JINGLE;
		djt.name = tr("Jingle Sessions");
		djt.description = tr("Supports Jingle session negotiation");
		FDiscovery->insertDiscoFeature(djt);

		djt.var = NS_JINGLE_APPS_RTP_AUDIO;
		djt.name = tr("Voice Call (audio)");
		djt.description = tr("Supports audio content in Jingle RTP sessions");
		FDiscovery->insertDiscoFeature(djt);

		djt.var = NS_JINGLE_TRANSPORTS_ICEUDP;
		djt.name = tr("Voice Call (ICE-UDP)");
		djt.description = tr("Supports the ICE-UDP transport for Jingle sessions");
		FDiscovery->insertDiscoFeature(djt);

		djt.var = NS_JINGLE_APPS_DTLS;
		djt.name = tr("Voice Call (DTLS-SRTP)");
		djt.description = tr("Supports DTLS-SRTP key negotiation for Jingle RTP sessions");
		FDiscovery->insertDiscoFeature(djt);

		//NOTE: urn:ietf:rfc:3264 is deliberately NOT advertised. Snikket/Conversations
		//then put their whole candidate list into the offer/answer and skip trickle, and
		//since their inline form is snapshotted before STUN/TURN gathering completes, a
		//peer reachable only through a relay ends up offering just its host candidates.
		//Trickle (transport-info) delivers the srflx/relay candidates as they appear.

		FDiscovery->insertFeatureHandler(NS_JINGLE_APPS_RTP, this, DFO_DEFAULT);
	}
	if (FOptionsManager)
	{
		IOptionsDialogNode callsNode = { ONO_CALLS, OPN_CALLS, MNI_JINGLE_CALL, tr("Calls") };
		FOptionsManager->insertOptionsDialogNode(callsNode);
		FOptionsManager->insertOptionsDialogHolder(this);
	}

	connect(FXmppStreamManager->instance(), SIGNAL(streamOpened(IXmppStream *)), SLOT(onStreamOpened(IXmppStream *)));
	connect(FXmppStreamManager->instance(), SIGNAL(streamClosed(IXmppStream *)), SLOT(onStreamClosed(IXmppStream *)));

	return true;
}

bool Jingle::initSettings()
{
	Options::setDefaultValue(OPV_JINGLE_ENABLED, true);
	Options::setDefaultValue(OPV_JINGLE_ALLOWUNKNOWN, false);
	Options::setDefaultValue(OPV_JINGLE_STUN_SERVER, QString());
	Options::setDefaultValue(OPV_JINGLE_TURN_SERVER, QString());
	Options::setDefaultValue(OPV_JINGLE_TURN_USERNAME, QString());
	Options::setDefaultValue(OPV_JINGLE_TURN_PASSWORD, QString());
	Options::setDefaultValue(OPV_JINGLE_USE_DISCOVERY, true);
	Options::setDefaultValue(OPV_JINGLE_FORCE_RELAY, false);
	return true;
}

bool Jingle::isSupported(const Jid &AStreamJid, const Jid &AContactJid) const
{
	if (FDiscovery && AStreamJid.isValid() && AContactJid.isValid() && Options::node(OPV_JINGLE_ENABLED).value().toBool())
		return FDiscovery->checkDiscoFeature(AStreamJid, AContactJid, NS_JINGLE_APPS_RTP);
	return false;
}

QString Jingle::call(const Jid &AStreamJid, const Jid &AContactJid)
{
	IXmppStream *xmppStream = FXmppStreamManager->findXmppStream(AStreamJid);
	if (!xmppStream || !AContactJid.isValid())
		return QString();

	JingleSession *existingSession = findActiveSession(AStreamJid, AContactJid);
	if (existingSession)
		return existingSession->sid();

	if (!isSupported(AStreamJid, AContactJid))
	{
		LOG_STRM_WARNING(AStreamJid, QString("Failed to call %1: Feature is not supported or disabled").arg(AContactJid.bare()));
		return QString();
	}

	Jid localJid = xmppStream->streamJid();
	JingleSession *session = createSession(AStreamJid, AContactJid, localJid, localJid.full(), JingleSession::generateSid(), true);
	session->initiate();

	emit callStarted(AStreamJid, AContactJid, session->sid());
	return session->sid();
}

bool Jingle::answerCall(const QString &ASid)
{
	JingleSession *session = findSession(ASid);
	if (session && session->state() == IJingle::CallRinging)
	{
		session->answer();
		return true;
	}
	return false;
}

bool Jingle::declineCall(const QString &ASid)
{
	JingleSession *session = findSession(ASid);
	if (session && (session->state() == IJingle::CallRinging || session->state() == IJingle::CallOutgoing || session->state() == IJingle::CallRingback))
	{
		session->decline();
		return true;
	}
	return false;
}

bool Jingle::hangupCall(const QString &ASid)
{
	JingleSession *session = findSession(ASid);
	if (session && session->state() != IJingle::CallFinished)
	{
		session->hangup();
		return true;
	}
	return false;
}

IJingle::CallState Jingle::callState(const QString &ASid) const
{
	JingleSession *session = findSession(ASid);
	return session ? session->state() : IJingle::CallFinished;
}

Jid Jingle::callPeer(const QString &ASid) const
{
	JingleSession *session = findSession(ASid);
	return session ? session->contactJid() : Jid::null;
}

bool Jingle::stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept)
{
	QList<int> *streamHandles = FStanzaHandles.contains(AStreamJid) ? &FStanzaHandles[AStreamJid] : nullptr;
	if (!streamHandles || !streamHandles->contains(AHandleId))
		return false;

	QDomElement jingleElem = AStanza.firstElement("jingle", NS_JINGLE);
	if (jingleElem.isNull())
		return processNatPush(AStreamJid, AStanza, AAccept);

	const QString action = jingleElem.attribute("action");
	const QString sid = jingleElem.attribute("sid");
	if (action.isEmpty() || sid.isEmpty())
	{
		AAccept = true;
		Stanza error = FStanzaProcessor->makeReplyError(AStanza, XmppStanzaError(XmppStanzaError::EC_BAD_REQUEST, tr("Malformed Jingle request")));
		FStanzaProcessor->sendStanzaOut(AStreamJid, error);
		LOG_STRM_WARNING(AStreamJid, QString("Failed to process Jingle stanza: Action or session id is empty"));
		return true;
	}

	if (JingleSession *session = findSession(AStreamJid, sid))
	{
		Stanza reply;
		if (session->processStanza(AStanza, reply))
		{
			AAccept = true;
			if (!reply.isNull())
				FStanzaProcessor->sendStanzaOut(AStreamJid, reply);
			LOG_STRM_INFO(AStreamJid, QString("Jingle action '%1' of session '%2' received from %3").arg(action, sid, AStanza.from()));
			LOG_STRM_DEBUG(AStreamJid, QString("Jingle incoming stanza: %1").arg(AStanza.toString(0).trimmed()));
			return true;
		}
		return false;
	}
	else if (action == "session-initiate")
	{
		return processSessionInitiate(AStreamJid, AStanza, jingleElem, AAccept);
	}

	// Unknown session - acknowledge the request without any state change (RFC style "unknown session-id" handling)
	AAccept = true;
	Stanza result = FStanzaProcessor->makeReplyResult(AStanza);
	FStanzaProcessor->sendStanzaOut(AStreamJid, result);
	LOG_STRM_WARNING(AStreamJid, QString("Ignoring Jingle action '%1' of unknown session '%2'").arg(action, sid));
	return true;
}

void Jingle::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	//XEP-0215 discovery and credential requests share the request owner with the
	//session-initiate acknowledgements
	if (FNatServiceRequests.contains(AStanza.id()))
	{
		const Jid streamJid = FNatServiceRequests.take(AStanza.id());
		onNatServicesResult(streamJid, AStanza);
		return;
	}
	if (FNatCredentialRequests.contains(AStanza.id()))
	{
		const NatService service = FNatCredentialRequests.take(AStanza.id());
		onNatCredentialsResult(AStreamJid, AStanza, service);
		return;
	}

	const QString sid = FPendingRequests.take(AStanza.id());
	if (sid.isEmpty())
		return;

	JingleSession *session = findSession(sid);
	if (!session)
		return;

	if (AStanza.isError())
	{
		const XmppStanzaError error(AStanza);
		session->onInitiateResult(false, error.conditionText());
		LOG_STRM_WARNING(AStreamJid, QString("Failed to initiate Jingle session '%1': %2").arg(sid, error.condition()));
	}
	else
	{
		session->onInitiateResult(true, QString());
		LOG_STRM_INFO(AStreamJid, QString("Jingle session '%1' signaling established").arg(sid));
	}
}

bool Jingle::execDiscoFeature(const Jid &AStreamJid, const QString &AFeature, const IDiscoInfo &ADiscoInfo)
{
	if (AFeature == NS_JINGLE_APPS_RTP)
		return !call(AStreamJid, ADiscoInfo.contactJid).isEmpty();
	return false;
}

Action *Jingle::createDiscoFeatureAction(const Jid &AStreamJid, const QString &AFeature, const IDiscoInfo &ADiscoInfo, QWidget *AParent)
{
	if (AFeature == NS_JINGLE_APPS_RTP)
	{
		Action *action = new Action(AParent);
		action->setText(tr("Voice Call"));
		action->setIcon(RSR_STORAGE_MENUICONS, MNI_JINGLE_CALL);
		action->setData(ADR_STREAM_JID, AStreamJid.full());
		action->setData(ADR_CONTACT_JID, ADiscoInfo.contactJid.full());
		action->setEnabled(isSupported(AStreamJid, ADiscoInfo.contactJid));
		connect(action, SIGNAL(triggered(bool)), SLOT(onCallByAction(bool)));
		return action;
	}
	return nullptr;
}

QMultiMap<int, IOptionsDialogWidget *> Jingle::optionsDialogWidgets(const QString &ANodeId, QWidget *AParent)
{
	QMultiMap<int, IOptionsDialogWidget *> widgets;
	if (FOptionsManager && ANodeId == OPN_CALLS)
	{
		widgets.insert(OHO_CALLS_SETTINGS, FOptionsManager->newOptionsDialogHeader(tr("Voice and video calls"), AParent));
		widgets.insert(OWO_CALLS_ENABLED, new CallsOptionsWidget(AParent));
	}
	return widgets;
}

bool Jingle::sendSessionAction(JingleSession *ASession, Stanza &AStanza)
{
	QDomElement jingleElem = AStanza.firstElement("jingle", NS_JINGLE);
	LOG_STRM_INFO(ASession->streamJid(), QString("Jingle action '%1' of session '%2' sent to %3").arg(jingleElem.attribute("action"), jingleElem.attribute("sid"), AStanza.to()));
	return FStanzaProcessor->sendStanzaOut(ASession->streamJid(), AStanza);
}

void Jingle::sendSessionRequest(JingleSession *ASession, Stanza &AStanza)
{
	QDomElement jingleElem = AStanza.firstElement("jingle", NS_JINGLE);
	LOG_STRM_INFO(ASession->streamJid(), QString("Jingle action '%1' of session '%2' sent to %3, waiting for acknowledge").arg(jingleElem.attribute("action"), jingleElem.attribute("sid"), AStanza.to()));
	LOG_STRM_DEBUG(ASession->streamJid(), QString("Jingle outgoing stanza: %1").arg(AStanza.toString(0).trimmed()));
	FPendingRequests.insert(AStanza.id(), ASession->sid());
	FStanzaProcessor->sendStanzaRequest(this, ASession->streamJid(), AStanza, JINGLE_INITIATE_TIMEOUT);
}

Stanza Jingle::makeResultReply(const Stanza &AStanza) const
{
	return FStanzaProcessor->makeReplyResult(AStanza);
}

Stanza Jingle::makeErrorReply(const Stanza &AStanza, const XmppStanzaError &AError) const
{
	return FStanzaProcessor->makeReplyError(AStanza, AError);
}

QString Jingle::peerName(const Jid &AStreamJid, const Jid &AContactJid) const
{
	if (FRostersManager)
	{
		IRoster *roster = FRostersManager->findRoster(AStreamJid);
		if (roster)
		{
			const IRosterItem item = roster->findItem(AContactJid);
			if (!item.name.isEmpty())
				return item.name;
		}
	}
	return AContactJid.bare();
}

//Accepts "host", "host:port", "turn:host:port" and "turn://host:port"; the
//default port is the RFC 5766 one (3478).
static bool parseServerSetting(const QString &ASetting, QString &AHost, quint16 &APort)
{
	QString value = ASetting.trimmed();
	if (value.isEmpty())
		return false;

	const int schemeEnd = value.indexOf("://");
	if (schemeEnd >= 0)
		value = value.mid(schemeEnd + 3);
	else if (value.startsWith("turn:") || value.startsWith("stun:"))
		value = value.mid(5);

	const int colon = value.lastIndexOf(':');
	if (colon > 0 && value.indexOf(':') == colon)
	{
		AHost = value.left(colon).trimmed();
		APort = quint16(value.mid(colon + 1).toUShort());
	}
	else
	{
		AHost = value;
		APort = 0;
	}
	if (APort == 0)
		APort = 3478;
	return !AHost.isEmpty();
}

NatConfig Jingle::natConfig(const Jid &AStreamJid) const
{
	NatConfig config;
	config.forceRelay = Options::node(OPV_JINGLE_FORCE_RELAY).value().toBool();

	//Manual settings take precedence over the discovered services
	parseServerSetting(Options::node(OPV_JINGLE_STUN_SERVER).value().toString(), config.stunHost, config.stunPort);
	parseServerSetting(Options::node(OPV_JINGLE_TURN_SERVER).value().toString(), config.turnHost, config.turnPort);
	if (config.hasTurn())
	{
		config.turnUsername = Options::node(OPV_JINGLE_TURN_USERNAME).value().toString();
		config.turnPassword = Options::node(OPV_JINGLE_TURN_PASSWORD).value().toString();
	}

	if (!config.hasStun() || !config.hasTurn())
	{
		const QList<NatService> services = FNatServices.value(AStreamJid.full());
		if (!config.hasStun())
		{
			for (const NatService &service : services)
			{
				if (NatDiscovery::isUsableStun(service))
				{
					config.stunHost = service.host;
					config.stunPort = service.port;
					break;
				}
			}
		}
		if (!config.hasTurn())
		{
			//Prefer a relay we already have credentials for (restricted services
			//answer a separate credential request), fall back to an open one
			NatService chosen;
			bool found = false;
			for (const NatService &service : services)
			{
				if (NatDiscovery::isUsableTurn(service) && !service.username.isEmpty())
				{
					chosen = service;
					found = true;
					break;
				}
			}
			if (!found)
			{
				for (const NatService &service : services)
				{
					if (NatDiscovery::isUsableTurn(service))
					{
						chosen = service;
						found = true;
						break;
					}
				}
			}
			if (found)
			{
				config.turnHost = chosen.host;
				config.turnPort = chosen.port;
				config.turnUsername = chosen.username;
				config.turnPassword = chosen.password;
			}
		}
	}

	if (config.hasStun() || config.hasTurn())
	{
		LOG_DEBUG(QString("Jingle: NAT config for %1: stun=%2:%3 turn=%4:%5 forceRelay=%6").
			arg(AStreamJid.full()).
			arg(config.stunHost).arg(config.stunPort).
			arg(config.turnHost).arg(config.turnPort).arg(config.forceRelay));
	}
	return config;
}

void Jingle::requestNatServices(const Jid &AStreamJid)
{
	if (!Options::node(OPV_JINGLE_USE_DISCOVERY).value().toBool())
		return;

	Stanza request(STANZA_KIND_IQ);
	request.setType(STANZA_TYPE_GET);
	request.setTo(AStreamJid.domain());
	request.setUniqueId();
	request.addElement("services", NS_EXTERNAL_SERVICES);

	FNatServiceRequests.insert(request.id(), AStreamJid);
	FStanzaProcessor->sendStanzaRequest(this, AStreamJid, request, JINGLE_INITIATE_TIMEOUT);
	LOG_STRM_DEBUG(AStreamJid, QString("Requested external services (XEP-0215)"));
}

void Jingle::requestNatCredentials(const Jid &AStreamJid, const NatService &AService)
{
	Stanza request(STANZA_KIND_IQ);
	request.setType(STANZA_TYPE_GET);
	request.setTo(AStreamJid.domain());
	request.setUniqueId();
	QDomElement credentialsElem = request.addElement("credentials", NS_EXTERNAL_SERVICES);
	QDomElement serviceElem = credentialsElem.ownerDocument().createElement("service");
	serviceElem.setAttribute("type", AService.type);
	serviceElem.setAttribute("host", AService.host);
	serviceElem.setAttribute("port", QString::number(AService.port));
	serviceElem.setAttribute("transport", AService.transport.isEmpty() ? QStringLiteral("udp") : AService.transport);
	credentialsElem.appendChild(serviceElem);

	FNatCredentialRequests.insert(request.id(), AService);
	FStanzaProcessor->sendStanzaRequest(this, AStreamJid, request, JINGLE_INITIATE_TIMEOUT);
	LOG_STRM_DEBUG(AStreamJid, QString("Requested credentials for the TURN service %1").arg(NatDiscovery::describe(AService)));
}

void Jingle::onNatServicesResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (AStanza.isError())
	{
		const XmppStanzaError error(AStanza);
		LOG_STRM_WARNING(AStreamJid, QString("External service discovery failed: %1").arg(error.condition()));
		return;
	}

	const QDomElement servicesElem = AStanza.firstElement("services", NS_EXTERNAL_SERVICES);
	if (servicesElem.isNull())
		return;

	QList<NatService> services = NatDiscovery::parseServices(servicesElem);
	FNatServices[AStreamJid.full()] = services;

	QStringList described;
	for (const NatService &service : services)
		described << NatDiscovery::describe(service);
	LOG_STRM_INFO(AStreamJid, QString("External services discovered: %1").arg(described.join("; ")));

	for (const NatService &service : services)
	{
		if (NatDiscovery::isUsableTurn(service) && service.restricted && service.username.isEmpty())
			requestNatCredentials(AStreamJid, service);
	}
}

void Jingle::onNatCredentialsResult(const Jid &AStreamJid, const Stanza &AStanza, const NatService &AService)
{
	if (AStanza.isError())
	{
		const XmppStanzaError error(AStanza);
		LOG_STRM_WARNING(AStreamJid, QString("Failed to get credentials for the TURN service %1: %2").
			arg(NatDiscovery::describe(AService), error.condition()));
		return;
	}

	const QDomElement credentialsElem = AStanza.firstElement("credentials", NS_EXTERNAL_SERVICES);
	if (credentialsElem.isNull())
		return;

	const QList<NatService> credentials = NatDiscovery::parseCredentials(credentialsElem);
	QList<NatService> &services = FNatServices[AStreamJid.full()];
	for (const NatService &credential : credentials)
	{
		for (NatService &cached : services)
		{
			const bool sameService = NatDiscovery::serviceKey(cached) == NatDiscovery::serviceKey(credential);
			const bool sameKeyAsRequest = NatDiscovery::serviceKey(cached) == NatDiscovery::serviceKey(AService);
			if (sameService || (credentials.count() == 1 && sameKeyAsRequest))
			{
				cached.username = credential.username;
				cached.password = credential.password;
				cached.expires = credential.expires;
				LOG_STRM_INFO(AStreamJid, QString("TURN credentials received for %1, expires %2").
					arg(NatDiscovery::describe(cached), cached.expires.isValid() ? cached.expires.toString(Qt::ISODate) : QString("never")));
			}
		}
	}
}

bool Jingle::processNatPush(const Jid &AStreamJid, const Stanza &AStanza, bool &AAccept)
{
	const QDomElement servicesElem = AStanza.firstElement("services", NS_EXTERNAL_SERVICES);
	if (servicesElem.isNull())
		return false;

	AAccept = true;
	Stanza result = FStanzaProcessor->makeReplyResult(AStanza);
	FStanzaProcessor->sendStanzaOut(AStreamJid, result);

	const Jid from = AStanza.fromJid();
	if (from.isValid() && from.domain() != AStreamJid.domain())
	{
		LOG_STRM_WARNING(AStreamJid, QString("Ignoring an external services push from %1").arg(from.full()));
		return true;
	}

	QList<NatService> &services = FNatServices[AStreamJid.full()];
	const QList<NatService> update = NatDiscovery::parseServices(servicesElem);
	NatDiscovery::applyUpdate(services, update);
	LOG_STRM_INFO(AStreamJid, QString("External services push applied (%1 entries)").arg(update.count()));

	for (const NatService &service : services)
	{
		if (NatDiscovery::isUsableTurn(service) && service.restricted && service.username.isEmpty())
			requestNatCredentials(AStreamJid, service);
	}
	return true;
}

void Jingle::onStreamOpened(IXmppStream *AXmppStream)
{
	IStanzaHandle shandle;
	shandle.handler = this;
	shandle.order = SHO_JINGLE;
	shandle.direction = IStanzaHandle::DirectionIn;
	shandle.streamJid = AXmppStream->streamJid();
	shandle.conditions.append(SHC_JINGLE_IQ);
	shandle.conditions.append(SHC_EXTDISCO_IQ);

	FStanzaHandles[shandle.streamJid].append(FStanzaProcessor->insertStanzaHandle(shandle));

	requestNatServices(shandle.streamJid);
}

void Jingle::onStreamClosed(IXmppStream *AXmppStream)
{
	const Jid streamJid = AXmppStream->streamJid();

	foreach(int handleId, FStanzaHandles.take(streamJid))
		FStanzaProcessor->removeStanzaHandle(handleId);

	FNatServices.remove(streamJid.full());
	for (auto it = FNatServiceRequests.begin(); it != FNatServiceRequests.end(); )
	{
		if (it.value() == streamJid)
			it = FNatServiceRequests.erase(it);
		else
			++it;
	}

	QList<JingleSession *> streamSessions;
	foreach(JingleSession *session, FSessions)
	{
		if (session->streamJid().bare() == streamJid.bare())
			streamSessions.append(session);
	}
	foreach(JingleSession *session, streamSessions)
	{
		LOG_STRM_INFO(streamJid, QString("Removing Jingle session '%1' on stream close").arg(session->sid()));
		removeSession(session);
	}
}

void Jingle::onCallByAction(bool AChecked)
{
	Q_UNUSED(AChecked);

	Action *action = qobject_cast<Action *>(sender());
	if (action)
		call(Jid(action->data(ADR_STREAM_JID).toString()), Jid(action->data(ADR_CONTACT_JID).toString()));
}

void Jingle::onSessionStateChanged(JingleSession *ASession, IJingle::CallState AState)
{
	emit callStateChanged(ASession->sid(), AState);
}

void Jingle::onSessionFinished(JingleSession *ASession, const QString &AReason, const QString &AText)
{
	LOG_STRM_INFO(ASession->streamJid(), QString("Jingle session '%1' finished, reason=%2, text=%3").arg(ASession->sid(), AReason, AText));
	emit callFinished(ASession->sid(), AReason);
	removeSession(ASession);
}

bool Jingle::processSessionInitiate(const Jid &AStreamJid, const Stanza &AStanza, const QDomElement &AJingleElem, bool &AAccept)
{
	const QString sid = AJingleElem.attribute("sid");
	const Jid initiatorJid = AStanza.fromJid().isValid() ? AStanza.fromJid() : Jid(AJingleElem.attribute("initiator"));
	const Jid contactJid = Jid(initiatorJid.bare());

	AAccept = true;

	if (!Options::node(OPV_JINGLE_ENABLED).value().toBool())
	{
		Stanza error = FStanzaProcessor->makeReplyError(AStanza, XmppStanzaError(XmppStanzaError::EC_SERVICE_UNAVAILABLE, tr("Calls are disabled")));
		FStanzaProcessor->sendStanzaOut(AStreamJid, error);
		return true;
	}

	IXmppStream *xmppStream = FXmppStreamManager->findXmppStream(AStreamJid);
	if (!xmppStream)
	{
		Stanza error = FStanzaProcessor->makeReplyError(AStanza, XmppStanzaError(XmppStanzaError::EC_SERVICE_UNAVAILABLE, tr("Stream is not ready")));
		FStanzaProcessor->sendStanzaOut(AStreamJid, error);
		return true;
	}

	if (findSession(AStreamJid, sid))
	{
		// Duplicate/resent session-initiate - just acknowledge it
		Stanza result = FStanzaProcessor->makeReplyResult(AStanza);
		FStanzaProcessor->sendStanzaOut(AStreamJid, result);
		return true;
	}

	bool isAuthorized = true;
	if (FRostersManager)
	{
		IRoster *roster = FRostersManager->findRoster(AStreamJid);
		if (roster)
		{
			const IRosterItem item = roster->findItem(contactJid);
			isAuthorized = item.subscription == SUBSCRIPTION_BOTH || item.subscription == SUBSCRIPTION_FROM;
		}
	}

	if (!Options::node(OPV_JINGLE_ALLOWUNKNOWN).value().toBool() && !isAuthorized)
	{
		LOG_STRM_WARNING(AStreamJid, QString("Declining call from not authorized contact %1").arg(contactJid.bare()));
		Stanza result = FStanzaProcessor->makeReplyResult(AStanza);
		FStanzaProcessor->sendStanzaOut(AStreamJid, result);

		JingleSession *session = createSession(AStreamJid, contactJid, xmppStream->streamJid(), initiatorJid.full(), sid, false);
		session->setSilent(true);
		session->storeInitiate(AJingleElem);
		session->decline();
		return true;
	}

	Stanza result = FStanzaProcessor->makeReplyResult(AStanza);
	FStanzaProcessor->sendStanzaOut(AStreamJid, result);

	JingleSession *session = createSession(AStreamJid, contactJid, xmppStream->streamJid(), initiatorJid.full(), sid, false);
	session->storeInitiate(AJingleElem);
	session->showWindow();

	LOG_STRM_INFO(AStreamJid, QString("Jingle session-initiate received from %1").arg(initiatorJid.full()));
	LOG_STRM_DEBUG(AStreamJid, QString("Jingle incoming stanza: %1").arg(AStanza.toString(0).trimmed()));
	emit callReceived(AStreamJid, contactJid, sid);
	return true;
}

JingleSession *Jingle::findSession(const QString &ASid) const
{
	foreach(JingleSession *session, FSessions)
	{
		if (session->sid() == ASid)
			return session;
	}
	return nullptr;
}

JingleSession *Jingle::findSession(const Jid &AStreamJid, const QString &ASid) const
{
	foreach(JingleSession *session, FSessions)
	{
		if (session->sid() == ASid && session->streamJid().bare() == AStreamJid.bare())
			return session;
	}
	return nullptr;
}

JingleSession *Jingle::findActiveSession(const Jid &AStreamJid, const Jid &AContactJid) const
{
	foreach(JingleSession *session, FSessions)
	{
		if (session->state() != IJingle::CallFinished &&
			session->streamJid().bare() == AStreamJid.bare() &&
			session->contactJid().bare() == AContactJid.bare())
			return session;
	}
	return nullptr;
}

JingleSession *Jingle::createSession(const Jid &AStreamJid, const Jid &AContactJid, const Jid &ALocalJid, const QString &AInitiatorJid, const QString &ASid, bool AInitiated)
{
	JingleSession *session = new JingleSession(AStreamJid, ALocalJid, AContactJid, AInitiatorJid, ASid, AInitiated, this, peerName(AStreamJid, AContactJid));

	connect(session, &JingleSession::stateChanged, this, [this, session](IJingle::CallState AState) {
		onSessionStateChanged(session, AState);
	});
	connect(session, &JingleSession::finished, this, [this, session](const QString &AReason, const QString &AText) {
		onSessionFinished(session, AReason, AText);
	});

	FSessions.append(session);
	return session;
}

void Jingle::removeSession(JingleSession *ASession)
{
	FSessions.removeAll(ASession);
	ASession->deleteLater();
}
