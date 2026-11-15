#include "jinglesession.h"
#include "jingle.h"

#include <QDomDocument>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <definitions/namespaces.h>
#include <definitions/menuicons.h>
#include <definitions/resources.h>
#include <utils/iconstorage.h>
#include <utils/logger.h>

#include "dtlssrtp.h"
#include "iceagent.h"

#define RTP_CONTENT_NAME "audio"

struct RtpPayloadType {
	int id;
	QString name;
	int clockrate;
	int channels;
};

//The offer we make in session-initiate/session-accept (XEP-0167). Opus is preferred
//by all modern clients; the codec is the only one we can actually decode yet.
static const RtpPayloadType kPayloadTypes[] = {
	{111, QStringLiteral("opus"), 48000, 2},
};

JingleSession::JingleSession(const Jid &AStreamJid, const Jid &ALocalJid, const Jid &AContactJid, const QString &AInitiatorJid, const QString &ASid, bool AInitiated, Jingle *AJingle, const QString &APeerName)
	: QObject(nullptr)
{
	FStreamJid = AStreamJid;
	FLocalJid = ALocalJid;
	FContactJid = AContactJid;
	FInitiator = AInitiated ? ALocalJid.full() : AInitiatorJid;
	FTargetJid = AInitiated ? AContactJid : Jid(AInitiatorJid);
	FSid = ASid;
	FInitiated = AInitiated;
	FResendId = 0;
	FContentName = QStringLiteral(RTP_CONTENT_NAME);
	FJingle = AJingle;
	FPeerName = APeerName;
	FTerminated = false;
	FSilent = false;
	FRemoteOpusPayloadType = -1;
	FRemoteOpusClockrate = 48000;
	FOfferPending = false;
	FAnswerPending = false;
	FTransportSent = false;

	FState = AInitiated ? IJingle::CallOutgoing : IJingle::CallRinging;

	FGatherTimer = new QTimer(this);
	FGatherTimer->setSingleShot(true);
	FGatherTimer->setInterval(3000);
	connect(FGatherTimer, &QTimer::timeout, this, &JingleSession::onGatherWaitTimeout);

	FMediaEngine = new MediaEngine(this);
	if (!FMediaEngine->initialize())
	{
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': media engine failed to initialise, "
		                                     "calls will be sent without DTLS-SRTP and may be rejected").arg(FSid));
		delete FMediaEngine;
		FMediaEngine = nullptr;
	}
	else
	{
		connect(FMediaEngine, &MediaEngine::failed, this, [this](const QString &AReason) {
			if (!FTerminated)
			{
				LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1' media failed: %2").arg(FSid, AReason));
				finishLocally("failed-application", AReason);
			}
		});
		connect(FMediaEngine, &MediaEngine::iceConnected, this, [this]() {
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1' ICE connected, starting DTLS").arg(FSid));
		});
		connect(FMediaEngine, &MediaEngine::dtlsConnected, this, [this]() {
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1' DTLS established, media secure").arg(FSid));
		});

		//The callee binds its ports immediately so that candidates can be offered
		FMediaEngine->iceAgent()->setNatConfig(FJingle->natConfig(FStreamJid));
		connect(FMediaEngine->iceAgent(), &IceAgent::gatheringFinished, this, &JingleSession::onGatheringFinished);
		connect(FMediaEngine->iceAgent(), &IceAgent::candidateGathered, this, &JingleSession::onCandidateGathered);
		if (!FInitiated)
			FMediaEngine->startIce(false);
	}

	FWindow = new CallWindow(FSid);
	FWindow->setPeer(FPeerName, FContactJid.full());
	FWindow->setWindowIcon(IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_JINGLE_CALL));

	connect(FWindow, &CallWindow::answerRequested, this, &JingleSession::answer);
	connect(FWindow, &CallWindow::declineRequested, this, &JingleSession::decline);
	connect(FWindow, &CallWindow::hangupRequested, this, [this]() { hangup(); });
	connect(FWindow, &CallWindow::userClosed, this, &JingleSession::setUserClosed);

	setState(FState);
}

JingleSession::~JingleSession()
{
	FWindow->closeBySession();
	delete FWindow;
}

QString JingleSession::generateSid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString JingleSession::sid() const
{
	return FSid;
}

IJingle::CallState JingleSession::state() const
{
	return FState;
}

Jid JingleSession::streamJid() const
{
	return FStreamJid;
}

Jid JingleSession::contactJid() const
{
	return FContactJid;
}

Jid JingleSession::localJid() const
{
	return FLocalJid;
}

QString JingleSession::peerName() const
{
	return FPeerName;
}

bool JingleSession::isInitiator() const
{
	return FInitiated;
}

CallWindow *JingleSession::window() const
{
	return FWindow;
}

void JingleSession::initiate()
{
	if (!FInitiated)
		return;

	if (FMediaEngine)
	{
		FMediaEngine->iceAgent()->setNatConfig(FJingle->natConfig(FStreamJid));
		FMediaEngine->startIce(true);
	}

	FOfferPending = true;
	continueInitiate();

	if (!FSilent)
	{
		FWindow->show();
		FWindow->raise();
		FWindow->activateWindow();
	}
}

void JingleSession::continueInitiate(bool AForce)
{
	if (!FOfferPending || FTerminated)
		return;

	if (!AForce && FMediaEngine && !FMediaEngine->iceAgent()->isGatheringFinished())
	{
		armGatherWait();
		return;
	}
	if (!AForce)
		FGatherTimer->stop();
	FOfferPending = false;

	if (FMediaEngine && FMediaEngine->iceAgent()->localCandidates().isEmpty())
	{
		finishLocally("failed-application", tr("No network candidates are available"));
		return;
	}

	QDomElement jingleElem;
	Stanza initiate = makeSessionAction("session-initiate", &jingleElem);
	addContentElement(jingleElem);
	FTransportSent = true;
	FJingle->sendSessionRequest(this, initiate);
}

void JingleSession::answer()
{
	if (FState != IJingle::CallRinging || FTerminated || FAnswerPending)
		return;

	if (!FMediaEngine)
	{
		finishLocally("unsupported-applications", tr("The media engine is not available"));
		return;
	}
	if (FRemoteOpusPayloadType < 0 || FPeerFingerprint.isEmpty())
	{
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1' offers no common codec/fingerprint, declining").arg(FSid));
		decline();
		return;
	}

	FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
	applyDtlsClientRole();

	FAnswerPending = true;
	continueAnswer();
}

void JingleSession::continueAnswer(bool AForce)
{
	if (!FAnswerPending || FTerminated)
		return;

	if (!AForce && FMediaEngine && !FMediaEngine->iceAgent()->isGatheringFinished())
	{
		armGatherWait();
		return;
	}
	if (!AForce)
		FGatherTimer->stop();
	FAnswerPending = false;

	if (FMediaEngine && FMediaEngine->iceAgent()->localCandidates().isEmpty())
	{
		finishLocally("failed-application", tr("No network candidates are available"));
		return;
	}

	QDomElement jingleElem;
	Stanza accept = makeSessionAction("session-accept", &jingleElem);
	addContentElement(jingleElem);
	FTransportSent = true;
	FJingle->sendSessionAction(this, accept);

	setState(IJingle::CallActive);
}

void JingleSession::armGatherWait()
{
	if (!FGatherTimer->isActive())
	{
		LOG_STRM_DEBUG(FStreamJid, QString("Jingle session '%1' waits for candidate gathering").arg(FSid));
		FGatherTimer->start();
	}
}

void JingleSession::onGatheringFinished()
{
	if (FTerminated)
		return;
	FGatherTimer->stop();
	if (FOfferPending)
		continueInitiate();
	if (FAnswerPending)
		continueAnswer();
}

void JingleSession::onGatherWaitTimeout()
{
	if (FTerminated)
		return;
	LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': candidate gathering is still running, sending the transport with the candidates known so far").arg(FSid));
	if (FOfferPending)
		continueInitiate(true);
	if (FAnswerPending)
		continueAnswer(true);
}

void JingleSession::onCandidateGathered(const IceCandidate &ACandidate)
{
	//Candidates found before the offer/answer went out are already in it; only
	//the late ones travel in a separate transport-info action
	if (!FTransportSent || FTerminated)
		return;

	QDomElement jingleElem;
	Stanza info = makeSessionAction("transport-info", &jingleElem);
	addTransportInfo(jingleElem, ACandidate);
	FJingle->sendSessionAction(this, info);
	LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': late %2 candidate %3:%4 sent in transport-info").
		arg(FSid, ACandidate.type, ACandidate.address).arg(ACandidate.port));
}

void JingleSession::decline()
{
	if (FTerminated)
		return;

	QDomElement jingleElem;
	Stanza terminate = makeSessionAction("session-terminate", &jingleElem);
	addReason(jingleElem, "decline", tr("Call declined"));
	FJingle->sendSessionAction(this, terminate);

	finishLocally("decline", tr("Call declined"));
}

void JingleSession::hangup(const QString &AText)
{
	if (FTerminated)
		return;

	QDomElement jingleElem;
	Stanza terminate = makeSessionAction("session-terminate", &jingleElem);
	addReason(jingleElem, "success", AText);
	FJingle->sendSessionAction(this, terminate);

	finishLocally("success", AText);
}

void JingleSession::setUserClosed()
{
	if (FTerminated)
		return;

	switch (FState)
	{
	case IJingle::CallRinging:
		decline();
		break;
	case IJingle::CallActive:
		hangup();
		break;
	default:
		finishLocally("success", QString());
		break;
	}
}

void JingleSession::setSilent(bool ASilent)
{
	FSilent = ASilent;
}

void JingleSession::showWindow()
{
	if (FSilent)
		return;

	FWindow->show();
	FWindow->raise();
	FWindow->activateWindow();
}

bool JingleSession::processStanza(const Stanza &AStanza, Stanza &AReply)
{
	const QDomElement jingleElem = AStanza.firstElement("jingle", NS_JINGLE);
	if (jingleElem.isNull())
		return false;

	if (FTerminated)
	{
		//Late resends of a terminated session - acknowledge and ignore
		AReply = FJingle->makeResultReply(AStanza);
		return true;
	}

	const QString action = jingleElem.attribute("action");
	AReply = FJingle->makeResultReply(AStanza);

	if (action == "session-accept")
	{
		const QDomNodeList contentList = jingleElem.elementsByTagName("content");
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i).toElement());
		if (FMediaEngine)
		{
			FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
			applyDtlsClientRole();
		}
		if (FInitiated && (FState == IJingle::CallOutgoing || FState == IJingle::CallRingback))
			setState(IJingle::CallActive);
	}
	else if (action == "session-terminate")
	{
		QString reason = "success";
		QString text;

		const QDomElement reasonElem = jingleElem.firstChildElement("reason");
		if (!reasonElem.isNull())
		{
			const QDomElement codeElem = reasonElem.firstChildElement();
			if (!codeElem.isNull())
				reason = codeElem.tagName();
			const QDomElement textElem = reasonElem.firstChildElement("text");
			if (!textElem.isNull())
				text = textElem.text();
		}
		finishLocally(reason, text);
	}
	else if (action == "content-reject")
	{
		finishLocally("decline", tr("Peer rejected the media content"));
	}
	else if (action == "ringing")
	{
		if (FInitiated && FState == IJingle::CallRingback && !FSilent)
			FWindow->setStatusText(tr("Peer is ringing..."));
	}
	else if (action == "transport-info" || action == "description-info" || action == "content-accept" || action == "content-modify")
	{
		const QDomNodeList contentList = jingleElem.elementsByTagName("content");
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i).toElement());
		if (FMediaEngine)
			FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
	}
	else if (action == "session-info")
	{
		if (!FSilent)
		{
			if (!jingleElem.firstChildElement("hold").isNull())
				FWindow->setStatusText(tr("Call on hold by peer"));
			else if (!jingleElem.firstChildElement("active").isNull())
				FWindow->setStatusText(tr("Call resumed by peer"));
			else if (!jingleElem.firstChildElement("mute").isNull())
				FWindow->setStatusText(tr("Peer muted"));
			else if (!jingleElem.firstChildElement("unmute").isNull())
				FWindow->setStatusText(tr("Peer is not muted"));
		}
	}
	else if (action == "security-info")
	{
		//acknowledge
	}
	else
	{
		AReply = FJingle->makeErrorReply(AStanza, XmppStanzaError(XmppStanzaError::EC_FEATURE_NOT_IMPLEMENTED, tr("Unsupported Jingle action '%1'").arg(action)));
	}
	return true;
}

void JingleSession::storeInitiate(const QDomElement &AJingleElem)
{
	if (!AJingleElem.attribute("initiator").isEmpty())
	{
		FInitiator = AJingleElem.attribute("initiator");
		FTargetJid = Jid(FInitiator);
	}

	const QDomNodeList contentList = AJingleElem.elementsByTagName("content");
	if (contentList.count() > 0)
	{
		const QString contentName = contentList.at(0).toElement().attribute("name");
		if (!contentName.isEmpty())
			FContentName = contentName; //Answer must reuse the initiator's content name (e.g. "0")
	}
	for (int i = 0; i < contentList.count(); ++i)
		processContentElem(contentList.at(i).toElement());

	if (FMediaEngine)
		FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
}

void JingleSession::onInitiateResult(bool AOk, const QString &AErrorText)
{
	if (FTerminated)
		return;

	if (!AOk)
	{
		finishLocally("failed", AErrorText);
		return;
	}
	if (FState == IJingle::CallOutgoing)
		setState(IJingle::CallRingback);
}

void JingleSession::setState(IJingle::CallState AState)
{
	FState = AState;

	switch (FState)
	{
	case IJingle::CallOutgoing:
		FWindow->setStatusText(tr("Calling peer..."));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(true);
		FWindow->setHangupVisible(false);
		FWindow->setTimerActive(false);
		break;
	case IJingle::CallRinging:
		FWindow->setStatusText(tr("Incoming voice call"));
		FWindow->setAnswerVisible(true);
		FWindow->setDeclineVisible(true);
		FWindow->setHangupVisible(false);
		FWindow->setTimerActive(false);
		break;
	case IJingle::CallRingback:
		FWindow->setStatusText(tr("Waiting for peer to answer..."));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(true);
		FWindow->setHangupVisible(false);
		FWindow->setTimerActive(false);
		break;
	case IJingle::CallActive:
		FWindow->setStatusText(tr("Call in progress"));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(false);
		FWindow->setHangupVisible(true);
		FWindow->setTimerActive(true);
		break;
	case IJingle::CallFinished:
		FWindow->setStatusText(tr("Call closed"));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(false);
		FWindow->setHangupVisible(false);
		FWindow->setTimerActive(false);
		break;
	}

	emit stateChanged(FState);
}

void JingleSession::finishLocally(const QString &AReason, const QString &AText)
{
	if (FTerminated)
		return;

	if (FMediaEngine)
		FMediaEngine->stop();

	setState(IJingle::CallFinished);

	QString status = reasonText(AReason);
	if (!AText.isEmpty())
		status += ": " + AText;
	if (!FSilent)
	{
		FWindow->setStatusText(status);
		QTimer::singleShot(2000, FWindow, &CallWindow::closeBySession);
	}

	emit finished(AReason, AText);
}

Stanza JingleSession::makeSessionAction(const QString &AAction, QDomElement *AJingleElem)
{
	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET);
	iq.setTo(FTargetJid.full());
	iq.setUniqueId();

	QDomElement jingleElem = iq.addElement("jingle", NS_JINGLE);
	jingleElem.setAttribute("action", AAction);
	jingleElem.setAttribute("sid", FSid);
	if (!FInitiator.isEmpty())
		jingleElem.setAttribute("initiator", FInitiator);
	jingleElem.setAttribute("rid", QString::number(++FResendId));

	if (AJingleElem)
		*AJingleElem = jingleElem;
	return iq;
}

void JingleSession::addContentElement(QDomElement &AJingleElem) const
{
	QDomDocument doc = AJingleElem.ownerDocument();

	QDomElement contentElem = doc.createElement("content");
	contentElem.setAttribute("name", FContentName);

	addDescription(contentElem);
	addTransport(contentElem);

	AJingleElem.appendChild(contentElem);
}

void JingleSession::addDescription(QDomElement &AContentElem) const
{
	QDomDocument doc = AContentElem.ownerDocument();

	QDomElement descriptionElem = doc.createElementNS(NS_JINGLE_APPS_RTP, "description");
	descriptionElem.setAttribute("media", RTP_CONTENT_NAME);

	for (const RtpPayloadType &pt : kPayloadTypes)
	{
		//When answering, offer only the codecs the peer is able to receive
		if (!FInitiated && !FRemotePayloadNames.isEmpty() && !FRemotePayloadNames.contains(pt.name.toLower()))
			continue;

		QDomElement payloadElem = doc.createElement("payload-type");
		payloadElem.setAttribute("id", QString::number(pt.id));
		payloadElem.setAttribute("name", pt.name);
		payloadElem.setAttribute("clockrate", QString::number(pt.clockrate));
		if (pt.channels > 1)
			payloadElem.setAttribute("channels", QString::number(pt.channels));

		QDomElement paramElem = doc.createElement("parameter");
		paramElem.setAttribute("name", "minptime");
		paramElem.setAttribute("value", "20");
		payloadElem.appendChild(paramElem);

		descriptionElem.appendChild(payloadElem);
	}

	if (FMediaEngine)
	{
		//XEP-0360 places <encryption> inside the description, but Swiften's content parser
		//expects only SDES <crypto> there and declines DTLS-in-description offers; its own
		//offers carry the fingerprint as a BARE <fingerprint hash= setup=> child of
		//<transport> (XEP-0320 heritage). Match that shape exactly: description keeps no
		//encryption element, transport carries the single bare fingerprint.
		QDomElement ssrcElem = doc.createElement("ssrc");
		ssrcElem.setAttribute("xmlns", NS_JINGLE_APPS_RTP_SSMA);
		ssrcElem.setAttribute("id", QString::number(FMediaEngine->localSsrc(), 16).rightJustified(8, '0'));
		ssrcElem.setAttribute("type", RTP_CONTENT_NAME);
		QDomElement cnameElem = doc.createElement("parameter");
		cnameElem.setAttribute("name", "cname");
		cnameElem.setAttribute("value", FSid);
		ssrcElem.appendChild(cnameElem);
		descriptionElem.appendChild(ssrcElem);

		QDomElement rtcpMuxElem = doc.createElement("rtcp-mux");
		descriptionElem.appendChild(rtcpMuxElem);
	}

	AContentElem.appendChild(descriptionElem);
}

void JingleSession::addTransport(QDomElement &AContentElem) const
{
	QDomDocument doc = AContentElem.ownerDocument();

	QDomElement transportElem = doc.createElementNS(NS_JINGLE_TRANSPORTS_ICEUDP, "transport");

	if (FMediaEngine)
	{
		IceAgent *agent = FMediaEngine->iceAgent();
		transportElem.setAttribute("ufrag", agent->localUfrag());
		transportElem.setAttribute("pwd", agent->localPwd());

		for (const IceCandidate &candidate : agent->localCandidates())
			appendCandidateElement(transportElem, candidate);

		//Conversations/Smack + Swiften variant: a BARE <fingerprint> child of <transport>
		//carrying the setup attribute itself, exactly as those clients serialize it.
		//Swiften's transport parser tolerates no <encryption> wrapper here.
		DtlsSrtp *dtls = FMediaEngine->dtlsSrtp();
		QDomElement bareFingerprintElem = doc.createElement("fingerprint");
		bareFingerprintElem.setAttribute("xmlns", NS_JINGLE_APPS_DTLS);
		bareFingerprintElem.setAttribute("type", "sha-256");
		bareFingerprintElem.setAttribute("hash", "sha-256");
		bareFingerprintElem.setAttribute("setup", ourDtlsSetup());
		bareFingerprintElem.appendChild(doc.createTextNode(dtls->localFingerprint()));
		transportElem.appendChild(bareFingerprintElem);
	}

	AContentElem.appendChild(transportElem);
}

void JingleSession::appendCandidateElement(QDomElement &ATransportElem, const IceCandidate &ACandidate) const
{
	QDomDocument doc = ATransportElem.ownerDocument();

	QDomElement candidateElem = doc.createElement("candidate");
	candidateElem.setAttribute("foundation", QString::number(ACandidate.componentId));
	candidateElem.setAttribute("component", QString::number(ACandidate.componentId));
	candidateElem.setAttribute("generation", "0");
	candidateElem.setAttribute("id", QString("v%1c%2").arg(ACandidate.port).arg(ACandidate.componentId));
	candidateElem.setAttribute("network", "0");
	candidateElem.setAttribute("priority", QString::number(ACandidate.priority));
	candidateElem.setAttribute("type", ACandidate.type);
	candidateElem.setAttribute("ip", ACandidate.address);
	candidateElem.setAttribute("address", ACandidate.address); //XEP-0176 spelling, for older parsers
	candidateElem.setAttribute("port", QString::number(ACandidate.port));
	if (!ACandidate.relatedAddress.isEmpty() && ACandidate.relatedPort != 0)
	{
		//Both spellings: the ICE RFC one and the XEP-0176 heritage one
		candidateElem.setAttribute("raddr", ACandidate.relatedAddress);
		candidateElem.setAttribute("rport", QString::number(ACandidate.relatedPort));
		candidateElem.setAttribute("rel-addr", ACandidate.relatedAddress);
		candidateElem.setAttribute("rel-port", QString::number(ACandidate.relatedPort));
	}
	ATransportElem.appendChild(candidateElem);
}

void JingleSession::addTransportInfo(QDomElement &AJingleElem, const IceCandidate &ACandidate) const
{
	QDomDocument doc = AJingleElem.ownerDocument();

	QDomElement contentElem = doc.createElement("content");
	contentElem.setAttribute("name", FContentName);

	QDomElement transportElem = doc.createElementNS(NS_JINGLE_TRANSPORTS_ICEUDP, "transport");
	if (FMediaEngine)
	{
		IceAgent *agent = FMediaEngine->iceAgent();
		transportElem.setAttribute("ufrag", agent->localUfrag());
		transportElem.setAttribute("pwd", agent->localPwd());
	}
	appendCandidateElement(transportElem, ACandidate);

	//Some parsers (Swiften) insist on a non-empty content, keep <transport> alone
	contentElem.appendChild(transportElem);
	AJingleElem.appendChild(contentElem);
}

void JingleSession::addReason(QDomElement &AJingleElem, const QString &ACode, const QString &AText) const
{
	QDomDocument doc = AJingleElem.ownerDocument();

	QDomElement reasonElem = doc.createElement("reason");
	QDomElement codeElem = doc.createElement(ACode);
	reasonElem.appendChild(codeElem);

	if (!AText.isEmpty())
	{
		QDomElement textElem = doc.createElement("text");
		textElem.appendChild(doc.createTextNode(AText));
		reasonElem.appendChild(textElem);
	}

	AJingleElem.appendChild(reasonElem);
}

void JingleSession::processContentElem(const QDomElement &AContentElem)
{
	if (AContentElem.isNull())
		return;

	const QDomElement descriptionElem = AContentElem.firstChildElement("description");
	if (!descriptionElem.isNull())
	{
		FRemotePayloadNames.clear();
		FRemoteOpusPayloadType = -1;

		const QDomNodeList payloadList = descriptionElem.elementsByTagName("payload-type");
		for (int i = 0; i < payloadList.count(); ++i)
		{
			const QDomElement payloadElem = payloadList.at(i).toElement();
			const QString name = payloadElem.attribute("name").toLower();
			FRemotePayloadNames.append(name);
			if (name == "opus")
			{
				FRemoteOpusPayloadType = payloadElem.attribute("id").toInt();
				FRemoteOpusClockrate = payloadElem.attribute("clockrate", "48000").toInt();
			}
		}

		const QDomElement encryptionElem = descriptionElem.firstChildElement("encryption");
		if (!encryptionElem.isNull())
		{
			if (!encryptionElem.attribute("setup").isEmpty())
				FPeerDtlsSetup = encryptionElem.attribute("setup");
			const QDomElement fingerprintElem = encryptionElem.firstChildElement("fingerprint");
			if (!fingerprintElem.isNull() && fingerprintElem.attribute("type", "sha-256") == "sha-256")
				FPeerFingerprint = fingerprintElem.text().simplified();
		}
	}

	const QDomElement transportElem = AContentElem.firstChildElement("transport");
	if (!transportElem.isNull() && FMediaEngine)
	{
		IceAgent *agent = FMediaEngine->iceAgent();
		if (!transportElem.attribute("ufrag").isEmpty())
			agent->setRemoteCredentials(transportElem.attribute("ufrag"), transportElem.attribute("pwd"));

		//XEP-0320 heritage clients (Conversations/Smack, Swiften) place the DTLS
		//fingerprint inside the transport; pick it up from there too.
		const QDomElement transportEncryptionElem = transportElem.firstChildElement("encryption");
		if (!transportEncryptionElem.isNull())
		{
			if (!transportEncryptionElem.attribute("setup").isEmpty())
				FPeerDtlsSetup = transportEncryptionElem.attribute("setup");
			const QDomElement transportFingerprintElem = transportEncryptionElem.firstChildElement("fingerprint");
			if (!transportFingerprintElem.isNull() && FPeerFingerprint.isEmpty())
				FPeerFingerprint = transportFingerprintElem.text().simplified();
		}
		const QDomElement bareFingerprintElem = transportElem.firstChildElement("fingerprint");
		if (!bareFingerprintElem.isNull() && FPeerFingerprint.isEmpty())
		{
			FPeerFingerprint = bareFingerprintElem.text().simplified();
			if (FPeerDtlsSetup.isEmpty() && !bareFingerprintElem.attribute("setup").isEmpty())
				FPeerDtlsSetup = bareFingerprintElem.attribute("setup");
		}

		const QDomNodeList candidateList = transportElem.elementsByTagName("candidate");
		for (int i = 0; i < candidateList.count(); ++i)
		{
			const QDomElement candidateElem = candidateList.at(i).toElement();
			IceCandidate candidate;
			candidate.componentId = candidateElem.attribute("component", "1").toInt();
			candidate.address = candidateElem.attribute("ip", candidateElem.attribute("address"));
			candidate.port = quint16(candidateElem.attribute("port").toUInt());
			candidate.priority = candidateElem.attribute("priority").toUInt();
			candidate.ufrag = transportElem.attribute("ufrag");
			if (candidate.address.isEmpty() || candidate.port == 0)
				continue;
			if (candidate.componentId != 1 && candidate.componentId != 2)
				continue;
			candidate.type = candidateElem.attribute("type", "host");
			if (candidate.type != "host" && candidate.type != "srflx" &&
				candidate.type != "relay" && candidate.type != "prflx")
			{
				LOG_STRM_DEBUG(FStreamJid, QString("Jingle session '%1': skipping the unsupported %2 candidate %3:%4").
					arg(FSid, candidate.type, candidate.address).arg(candidate.port));
				continue;
			}
			candidate.relatedAddress = candidateElem.attribute("raddr", candidateElem.attribute("rel-addr"));
			agent->addRemoteCandidate(candidate);
		}
	}
}

QString JingleSession::ourDtlsSetup() const
{
	if (FInitiated)
		return QStringLiteral("actpass");
	//As the answerer we pick the role the peer's offer allows
	return FPeerDtlsSetup == "active" ? QStringLiteral("passive") : QStringLiteral("active");
}

void JingleSession::applyDtlsClientRole()
{
	//XEP-0359: the side that ends up with setup="active" is the DTLS client.
	//As the offerer we are the client only if the peer's answer was "passive".
	const bool asClient = FInitiated ? (FPeerDtlsSetup == "passive") : (FPeerDtlsSetup != "active");
	FMediaEngine->setDtlsClientRole(asClient);
	FMediaEngine->dtlsSrtp()->setPeerFingerprint(FPeerFingerprint);
}

QString JingleSession::reasonText(const QString &AReason) const
{
	if (AReason == "success")
		return tr("Call closed");
	if (AReason == "decline")
		return tr("Call declined");
	if (AReason == "busy")
		return tr("Peer is busy");
	if (AReason == "gone")
		return tr("Peer has gone");
	if (AReason == "no-answer")
		return tr("No answer");
	if (AReason == "cancel")
		return tr("Call cancelled by peer");
	if (AReason == "failed" || AReason == "failed-application")
		return tr("Call failed");
	if (AReason == "connectivity-error")
		return tr("Connection could not be established");
	if (AReason == "unsupported-applications" || AReason == "incompatible-application")
		return tr("Peer uses an incompatible application");
	if (AReason == "expired")
		return tr("Call expired");
	if (AReason == "security-soft" || AReason == "security-hard")
		return tr("Call terminated for security reasons");
	return tr("Call terminated");
}
