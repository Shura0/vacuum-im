#include "jinglesession.h"
#include "jingle.h"

#include <QDomDocument>
#include <QStringList>
#include <QTimer>
#include <QUuid>

#include <definitions/namespaces.h>
#include <definitions/menuicons.h>
#include <definitions/optionvalues.h>
#include <definitions/resources.h>
#include <utils/iconstorage.h>
#include <utils/logger.h>
#include <utils/options.h>

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

//Receive-only for now: VP8 over the same ICE/DTLS transport as audio.
static const RtpPayloadType kVideoPayloadTypes[] = {
	{96, QStringLiteral("VP8"), 90000, 0},
};

//Only content elements that are direct children of <jingle>; elementsByTagName() would
//also return the <content> references inside a BUNDLE group.
static QList<QDomElement> directContents(const QDomElement &AJingleElem)
{
	QList<QDomElement> result;
	QDomNode node = AJingleElem.firstChild();
	while (!node.isNull())
	{
		if (node.isElement() && node.toElement().tagName() == QStringLiteral("content"))
			result.append(node.toElement());
		node = node.nextSibling();
	}
	return result;
}

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
	FRemoteVideoPayloadType = -1;
	FRemoteVideoContentName = QStringLiteral("video");
	FOfferPending = false;
	FAnswerPending = false;
	FTransportSent = false;
	FRemoteCandidateCount = 0;
	FIceRestartCount = 0;
	FIceRestartPending = false;
	FJmiAwaitingProceed = false;
	FJmiPending = false;
	FJmiWaitingInitiate = false;
	FJmiFinishSent = false;
	FJmiUsed = false;
	FJmiTransportReceived = false;

	FState = AInitiated ? IJingle::CallOutgoing : IJingle::CallRinging;

	FGatherTimer = new QTimer(this);
	FGatherTimer->setSingleShot(true);
	FGatherTimer->setInterval(3000);
	connect(FGatherTimer, &QTimer::timeout, this, &JingleSession::onGatherWaitTimeout);

	FJmiTimer = new QTimer(this);
	FJmiTimer->setSingleShot(true);
	connect(FJmiTimer, &QTimer::timeout, this, &JingleSession::onJmiTimeout);

	FPeerWaitTimer = new QTimer(this);
	FPeerWaitTimer->setSingleShot(true);
	FPeerWaitTimer->setInterval(8000);
	connect(FPeerWaitTimer, &QTimer::timeout, this, &JingleSession::onPeerWaitTimeout);

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
			if (FIceRestartPending && !FSilent)
			{
				FIceRestartPending = false;
				FWindow->setStatusText(tr("Connection restored"));
			}
		});
		connect(FMediaEngine, &MediaEngine::dtlsConnected, this, [this]() {
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1' DTLS established, media secure").arg(FSid));
		});
		connect(FMediaEngine, &MediaEngine::videoFrameReceived, this, [this](const QImage &AFrame) {
			if (!FSilent && FWindow != nullptr)
				FWindow->setRemoteVideoFrame(AFrame);
		});
		connect(FMediaEngine, &MediaEngine::localVideoFrame, this, [this](const QImage &AFrame) {
			if (!FSilent && FWindow != nullptr)
				FWindow->setLocalVideoFrame(AFrame);
		});

		//The callee binds its ports immediately so that candidates can be offered
		FMediaEngine->iceAgent()->setNatConfig(FJingle->natConfig(FStreamJid));
		connect(FMediaEngine->iceAgent(), &IceAgent::gatheringFinished, this, &JingleSession::onGatheringFinished);
		connect(FMediaEngine->iceAgent(), &IceAgent::candidateGathered, this, &JingleSession::onCandidateGathered);
		connect(FMediaEngine->iceAgent(), &IceAgent::restartNeeded, this, &JingleSession::onIceRestartNeeded);
		if (!FInitiated)
			FMediaEngine->startIce(false);
	}

	FWindow = new CallWindow(FSid);
	FWindow->setPeer(FPeerName, FContactJid.full());
	FWindow->setWindowIcon(IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_JINGLE_CALL));

	FSounds = FJingle->callSounds();

	connect(FWindow, &CallWindow::answerRequested, this, &JingleSession::answer);
	connect(FWindow, &CallWindow::declineRequested, this, &JingleSession::decline);
	connect(FWindow, &CallWindow::hangupRequested, this, [this]() { hangup(); });
	connect(FWindow, &CallWindow::muteToggled, this, [this](bool AMuted) {
		if (FMediaEngine)
			FMediaEngine->setAudioMuted(AMuted);
	});
	connect(FWindow, &CallWindow::cameraToggled, this, [this](bool AEnabled) {
		if (AEnabled && FWindow)
			FWindow->setScreenSharing(false);
		if (FMediaEngine)
		{
			FMediaEngine->setScreenSharing(false);
			FMediaEngine->setVideoEnabled(AEnabled);
		}
	});
	connect(FWindow, &CallWindow::screenToggled, this, [this](bool AEnabled) {
		if (AEnabled && FWindow)
			FWindow->setCameraEnabled(false);
		if (FMediaEngine)
			FMediaEngine->setScreenSharing(AEnabled);
	});
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

	//XEP-0353: announce the call in a message first so that all of the peer's
	//devices (and push services) can show it before the Jingle IQ arrives.
	if (!FJmiAwaitingProceed && !FJmiFinishSent)
	{
		FJmiAwaitingProceed = true;
		FJmiUsed = true;
		sendJmiMessage(QStringLiteral("propose"));
		FJmiTimer->setInterval(3000);
		FJmiTimer->start();
	}
	else
	{
		continueInitiate();
	}

	if (!FSilent)
	{
		FWindow->show();
		FWindow->raise();
		FWindow->activateWindow();
	}
}

void JingleSession::continueInitiate(bool AForce)
{
	if (!FOfferPending || FTerminated || (FJmiAwaitingProceed && !AForce))
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

void JingleSession::resendInitiate()
{
	if (FTerminated || !FMediaEngine)
		return;

	QDomElement jingleElem;
	Stanza initiate = makeSessionAction("session-initiate", &jingleElem);
	addContentElement(jingleElem);
	FJingle->sendSessionRequest(this, initiate);
	LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': session-initiate resent after JMI proceed").arg(FSid));
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

	if (FJmiPending)
	{
		//XEP-0353: the responder accepts the proposal first; the initiator then
		//sends the real session-initiate, which we answer as usual.
		sendJmiMessage(QStringLiteral("proceed"));
		FJmiPending = false;
		if (FJmiTransportReceived)
		{
			FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
			applyDtlsClientRole();
			FAnswerPending = true;
			continueAnswer();
			return;
		}
		FJmiWaitingInitiate = true;
		FMediaEngine->iceAgent()->setNatConfig(FJingle->natConfig(FStreamJid));
		FMediaEngine->startIce(false);
		FJmiTimer->setInterval(30000);
		FJmiTimer->start();
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
	if (FOfferPending && !FJmiAwaitingProceed)
		continueInitiate(true);
	if (FAnswerPending)
		continueAnswer(true);
}

void JingleSession::armPeerWait()
{
	if (!FPeerWaitTimer->isActive())
		FPeerWaitTimer->start();
}

void JingleSession::onPeerWaitTimeout()
{
	if (FTerminated || FRemoteCandidateCount > 0)
		return;
	//A peer without any candidate cannot even start connectivity checks, the call
	//then hangs in "connecting" until one of the sides gives up.
	LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': no candidates received from %2 within 8s of the answer, the call cannot connect").
		arg(FSid, FContactJid.full()));
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

void JingleSession::onIceRestartNeeded()
{
	if (FTerminated || FMediaEngine == nullptr || FIceRestartCount >= 2)
		return;

	IceAgent *agent = FMediaEngine->iceAgent();
	if (!agent->restart())
	{
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': ICE restart failed").arg(FSid));
		FIceRestartCount = 2;
		return;
	}

	QDomElement jingleElem;
	Stanza replace = makeSessionAction("transport-replace", &jingleElem);
	addRestartTransport(jingleElem);
	FJingle->sendSessionAction(this, replace);

	++FIceRestartCount;
	FIceRestartPending = true;
	LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': ICE restart requested, generation=%2, attempt=%3").
		arg(FSid).arg(agent->generation()).arg(FIceRestartCount));
	if (!FSilent)
		FWindow->setStatusText(tr("Reconnecting..."));
}

void JingleSession::decline()
{
	if (FTerminated)
		return;

	if (FJmiPending)
	{
		sendJmiMessage(QStringLiteral("reject"), QStringLiteral("busy"), tr("Call declined"));
		finishLocally("decline", tr("Call declined"));
		return;
	}

	QDomElement jingleElem;
	Stanza terminate = makeSessionAction("session-terminate", &jingleElem);
	addReason(jingleElem, "decline", tr("Call declined"));
	FJingle->sendSessionAction(this, terminate);

	finishLocally("decline", tr("Call declined"));
}

void JingleSession::storeJmiPropose(const QDomElement &ADescriptionElem)
{
	Q_UNUSED(ADescriptionElem);
	//The call window already shows the incoming ringing state from the
	//constructor. The real Jingle transport arrives only after we send proceed.
	FJmiUsed = true;
	FJmiPending = true;
}

bool JingleSession::processJmiMessage(const QString &AAction, const QDomElement &AJmiElem)
{
	Q_UNUSED(AJmiElem);

	if (AAction == QStringLiteral("ringing"))
	{
		if (FJmiAwaitingProceed)
		{
			FJmiTimer->stop();
			FJmiTimer->setInterval(60000); //wait for the user to answer
			FJmiTimer->start();
			if (!FSilent)
				FWindow->setStatusText(tr("Peer is ringing..."));
		}
	}
	else if (AAction == QStringLiteral("proceed") || AAction == QStringLiteral("accept"))
	{
		if (FJmiAwaitingProceed)
		{
			FJmiAwaitingProceed = false;
			FJmiTimer->stop();
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': JMI call accepted by the peer (%2)").arg(FSid, AAction));
			if (FOfferPending)
				continueInitiate();
			else if (FTransportSent && (FState==IJingle::CallOutgoing || FState==IJingle::CallRingback))
				resendInitiate();
		}
	}
	else if (AAction == QStringLiteral("reject") || AAction == QStringLiteral("retract"))
	{
		FJmiAwaitingProceed = false;
		FJmiTimer->stop();
		LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': call proposal was rejected").arg(FSid));
		finishLocally("decline", tr("Call declined"));
	}
	else if (AAction == QStringLiteral("finish"))
	{
		FJmiFinishSent = true; //do not echo the finish back
		finishLocally("success", QString());
	}
	return true;
}

void JingleSession::sendJmiMessage(const QString &AAction, const QString &AReasonCode, const QString &AText)
{
	const Jid peer = FInitiated ? FTargetJid : Jid(FInitiator);
	const Jid to = (AAction==QStringLiteral("propose") || AAction==QStringLiteral("finish") ||
	                AAction==QStringLiteral("retract")) ? Jid(peer.bare()) : peer;
	FJingle->sendJmiMessage(FStreamJid, to, AAction, FSid, AReasonCode, AText);
}

void JingleSession::finishJmiSession(const QString &AReason, const QString &AText)
{
	FJmiFinishSent = true;
	QString code = QStringLiteral("success");
	if (AReason == QStringLiteral("no-answer") || AReason == QStringLiteral("expired") || AReason == QStringLiteral("cancel"))
		code = QStringLiteral("expired");
	else if (AReason == QStringLiteral("failed") || AReason == QStringLiteral("failed-application") ||
	         AReason == QStringLiteral("connectivity-error"))
		code = QStringLiteral("connectivity-error");
	sendJmiMessage(QStringLiteral("finish"), code, AText.isEmpty() ? reasonText(AReason) : AText);
}

void JingleSession::onJmiTimeout()
{
	if (FTerminated)
		return;

	if (FJmiAwaitingProceed)
	{
		//A peer without JMI support will never answer the proposal: fall back
		//to the plain session-initiate we have always sent.
		FJmiAwaitingProceed = false;
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': no JMI ringing/proceed, falling back to session-initiate").arg(FSid));
		continueInitiate();
	}
	else if (FJmiWaitingInitiate)
	{
		FJmiWaitingInitiate = false;
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': the peer did not send session-initiate after proceed").arg(FSid));
		finishLocally("no-answer", tr("The peer did not start the call"));
	}
	else
	{
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': the peer did not answer the call").arg(FSid));
		finishLocally("no-answer", tr("No answer"));
	}
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

	if (FState == IJingle::CallRinging)
		FSounds->playRingtone();
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

	if (action == "session-initiate" && FJmiPending)
	{
		//Some clients send the Jingle session right after the proposal, before
		//the user has answered. Keep the transport and let answer() use it.
		const QList<QDomElement> contentList = directContents(jingleElem);
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i));
		FJmiTransportReceived = true;
		if (FMediaEngine)
		{
			FMediaEngine->iceAgent()->setNatConfig(FJingle->natConfig(FStreamJid));
			FMediaEngine->startIce(false);
		}
		LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': session-initiate arrived with the JMI proposal").arg(FSid));
		return true;
	}

	if (action == "session-initiate" && FJmiWaitingInitiate)
	{
		//XEP-0353: the initiator sends the real session only after we said
		//proceed. Answer it exactly like a normal incoming session-initiate.
		const QList<QDomElement> contentList = directContents(jingleElem);
		FRemoteContentOrder.clear();
		for (int i = 0; i < contentList.count(); ++i)
		{
			const QString name = contentList.at(i).attribute("name");
			if (!name.isEmpty())
				FRemoteContentOrder.append(name);
		}
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i));
		if (FMediaEngine)
		{
			FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
			applyDtlsClientRole();
		}
		FJmiWaitingInitiate = false;
		FJmiTimer->stop();

		QDomElement acceptElem;
		Stanza accept = makeSessionAction("session-accept", &acceptElem);
		addContentElement(acceptElem);
		FTransportSent = true;
		FJingle->sendSessionAction(this, accept);
		setState(IJingle::CallActive);
		LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': JMI proposal completed with session-initiate").arg(FSid));
		return true;
	}

	if (action == "session-accept")
	{
		const QList<QDomElement> contentList = directContents(jingleElem);
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i));
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
		if (action == "transport-info" && FMediaEngine)
		{
			//After an ICE restart a trickle from the old generation must not
			//replace the fresh remote credentials with stale ones.
			const QString currentUfrag = FMediaEngine->iceAgent()->remoteUfrag();
			const QDomNodeList transports = jingleElem.elementsByTagName("transport");
			for (int i = 0; i < transports.count(); ++i)
			{
				const QString ufrag = transports.at(i).toElement().attribute("ufrag");
				if (!currentUfrag.isEmpty() && !ufrag.isEmpty() && ufrag != currentUfrag)
				{
					LOG_STRM_DEBUG(FStreamJid, QString("Jingle session '%1': ignoring transport-info from generation '%2', current remote ufrag is '%3").
						arg(FSid, ufrag, currentUfrag));
					return true;
				}
			}
		}
		const QList<QDomElement> contentList = directContents(jingleElem);
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i));
		if (FMediaEngine)
			FMediaEngine->setRemoteCodec(FRemoteOpusPayloadType, FRemoteOpusClockrate);
	}
	else if (action == "transport-replace")
	{
		//The peer asks for a new ICE generation: restart our side first so the
		//transport-accept advertises the new credentials and candidates.
		const QList<QDomElement> contentList = directContents(jingleElem);
		if (!FMediaEngine || contentList.isEmpty() || FTerminated)
		{
			QDomElement rejectElem;
			Stanza reject = makeSessionAction("transport-reject", &rejectElem);
			addReason(rejectElem, "out-of-order", tr("ICE restart is not possible"));
			FJingle->sendSessionAction(this, reject);
			return true;
		}
		if (!FMediaEngine->iceAgent()->restart())
		{
			QDomElement rejectElem;
			Stanza reject = makeSessionAction("transport-reject", &rejectElem);
			addReason(rejectElem, "failed-application", tr("Failed to restart ICE"));
			FJingle->sendSessionAction(this, reject);
			return true;
		}
		FMediaEngine->iceAgent()->clearRemoteCandidates();
		processContentElem(contentList.at(0));
		FRemoteCandidateCount = 0;

		QDomElement acceptElem;
		Stanza accept = makeSessionAction("transport-accept", &acceptElem);
		addRestartTransport(acceptElem);
		FJingle->sendSessionAction(this, accept);

		LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': accepted ICE restart, generation=%2").
			arg(FSid).arg(FMediaEngine->iceAgent()->generation()));
		if (!FSilent)
			FWindow->setStatusText(tr("Reconnecting..."));
		FIceRestartPending = true;
		FIceRestartCount = qMax(FIceRestartCount, 1);
	}
	else if (action == "transport-accept")
	{
		//Our restart was accepted; the peer's new credentials/candidates replace
		//the old generation completely.
		if (FMediaEngine)
			FMediaEngine->iceAgent()->clearRemoteCandidates();
		const QList<QDomElement> contentList = directContents(jingleElem);
		for (int i = 0; i < contentList.count(); ++i)
			processContentElem(contentList.at(i));
		FRemoteCandidateCount = 0;
		LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': ICE restart accepted by the peer").arg(FSid));
	}
	else if (action == "transport-reject")
	{
		LOG_STRM_WARNING(FStreamJid, QString("Jingle session '%1': peer rejected the ICE restart").arg(FSid));
		FIceRestartCount = 2;
		FIceRestartPending = false;
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

	const QList<QDomElement> contentList = directContents(AJingleElem);
	FRemoteContentOrder.clear();
	for (int i = 0; i < contentList.count(); ++i)
	{
		const QString name = contentList.at(i).attribute("name");
		if (!name.isEmpty())
			FRemoteContentOrder.append(name);
	}
	if (contentList.count() > 0)
	{
		const QString contentName = contentList.at(0).attribute("name");
		if (!contentName.isEmpty())
			FContentName = contentName; //Answer must reuse the initiator's content name (e.g. "0")
	}
	for (int i = 0; i < contentList.count(); ++i)
		processContentElem(contentList.at(i));

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

	const bool active = FState == IJingle::CallActive;
	if (FWindow)
	{
		FWindow->setMuteVisible(active);
		const bool videoEnabled = Options::node(OPV_JINGLE_VIDEO).value().toBool();
		FWindow->setCameraVisible(active && videoEnabled);
		FWindow->setScreenVisible(active && videoEnabled && FMediaEngine != nullptr && FMediaEngine->hasScreenShare());
		if (!active)
			FWindow->setScreenSharing(false);
		const bool camera = FMediaEngine != nullptr && FMediaEngine->hasVideoCamera();
		FWindow->setNoCameraVisible(active && videoEnabled && !camera);
		FWindow->setCameraEnabled(camera);
	}

	//The ringtone belongs to the ringing state only; playRingtone() starts it
	//when the window of an incoming call is shown. The dial tone accompanies an
	//outgoing call from the first stanza until the peer answers.
	if (FState != IJingle::CallRinging)
		FSounds->stopRingtone();
	if (FState != IJingle::CallOutgoing && FState != IJingle::CallRingback)
		FSounds->stopDialtone();

	switch (FState)
	{
	case IJingle::CallOutgoing:
		FWindow->setStatusText(tr("Calling peer..."));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(true);
		FWindow->setHangupVisible(false);
		FWindow->setTimerActive(false);
		if (!FSilent)
			FSounds->playDialtone();
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
		if (!FSilent)
			FSounds->playDialtone();
		break;
	case IJingle::CallActive:
		if (!FActiveSince.isValid())
			FActiveSince = QDateTime::currentDateTime();
		FWindow->setStatusText(tr("Call in progress"));
		FWindow->setAnswerVisible(false);
		FWindow->setDeclineVisible(false);
		FWindow->setHangupVisible(true);
		FWindow->setTimerActive(true);
		armPeerWait();
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
	FTerminated = true;
	FJmiTimer->stop();

	if (FJmiUsed && !FJmiFinishSent)
		finishJmiSession(AReason, AText);

	if (FMediaEngine)
		FMediaEngine->stop();

	setState(IJingle::CallFinished);

	//The call-ended tone is only for a call that was actually established
	if (FActiveSince.isValid())
		FSounds->playHangup();

	FJingle->reportCallEvent(FStreamJid, FContactJid, callEventText(AReason));

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
	const bool withVideo = Options::node(OPV_JINGLE_VIDEO).value().toBool() &&
	                       (FInitiated || FRemoteVideoPayloadType >= 0);
	QStringList order;
	if (!FInitiated && !FRemoteContentOrder.isEmpty())
		order = FRemoteContentOrder;
	else
	{
		order.append(FContentName);
		if (withVideo)
			order.append(FRemoteVideoContentName);
	}
	for (const QString &name : order)
		appendContentElem(AJingleElem, name, name == FRemoteVideoContentName && withVideo);

	if (withVideo)
	{
		//XEP-0320 BUNDLE: both contents share the single ICE/DTLS transport.
		//Without this group a WebRTC peer treats them as two independent m-lines
		//with the same port and fails with "SessionDescription is NULL".
		QDomDocument doc = AJingleElem.ownerDocument();
		QDomElement groupElem = doc.createElementNS(NS_JINGLE_APPS_GROUPING, "group");
		groupElem.setAttribute("semantics", QStringLiteral("BUNDLE"));
		for (const QString &name : order)
		{
			QDomElement contentElem = doc.createElement("content");
			contentElem.setAttribute("name", name);
			groupElem.appendChild(contentElem);
		}
		AJingleElem.appendChild(groupElem);
	}
}

void JingleSession::appendContentElem(QDomElement &AJingleElem, const QString &AName, bool AVideo) const
{
	QDomDocument doc = AJingleElem.ownerDocument();

	QDomElement contentElem = doc.createElement("content");
	contentElem.setAttribute("name", AName);
	//Siskin/Martin (and XEP-0166) require the creator attribute: without it the
	//whole <content> is rejected and the phone silently ignores session-initiate.
	contentElem.setAttribute("creator", FInitiated ? QStringLiteral("initiator") : QStringLiteral("responder"));
	//Video is bidirectional now; the default senders="both" applies.

	addDescription(contentElem, AVideo);
	addTransport(contentElem);

	AJingleElem.appendChild(contentElem);
}

void JingleSession::addDescription(QDomElement &AContentElem, bool AVideo) const
{
	QDomDocument doc = AContentElem.ownerDocument();

	QDomElement descriptionElem = doc.createElementNS(NS_JINGLE_APPS_RTP, "description");
	descriptionElem.setAttribute("media", AVideo ? QStringLiteral("video") : QStringLiteral(RTP_CONTENT_NAME));

	const RtpPayloadType *payloads = AVideo ? kVideoPayloadTypes : kPayloadTypes;
	const int payloadCount = AVideo ? int(sizeof(kVideoPayloadTypes)/sizeof(kVideoPayloadTypes[0])) :
	                                  int(sizeof(kPayloadTypes)/sizeof(kPayloadTypes[0]));
	for (int i = 0; i < payloadCount; ++i)
	{
		const RtpPayloadType &pt = payloads[i];
		//When answering, offer only the codecs the peer is able to receive
		//(the remote payload-name list is for audio only; video is separate).
		if (!AVideo && !FInitiated && !FRemotePayloadNames.isEmpty() && !FRemotePayloadNames.contains(pt.name.toLower()))
			continue;

		QDomElement payloadElem = doc.createElement("payload-type");
		payloadElem.setAttribute("id", QString::number(pt.id));
		payloadElem.setAttribute("name", pt.name);
		payloadElem.setAttribute("clockrate", QString::number(pt.clockrate));
		if (pt.channels > 1)
			payloadElem.setAttribute("channels", QString::number(pt.channels));

		if (!AVideo)
		{
			QDomElement paramElem = doc.createElement("parameter");
			paramElem.setAttribute("name", "minptime");
			paramElem.setAttribute("value", "20");
			payloadElem.appendChild(paramElem);
		}

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
		ssrcElem.setAttribute("id", QString::number(AVideo && FMediaEngine ? FMediaEngine->localVideoSsrc() : (FMediaEngine ? FMediaEngine->localSsrc() : 0), 16).rightJustified(8, '0'));
		ssrcElem.setAttribute("type", AVideo ? QStringLiteral("video") : QStringLiteral(RTP_CONTENT_NAME));
		QDomElement cnameElem = doc.createElement("parameter");
		cnameElem.setAttribute("name", "cname");
		cnameElem.setAttribute("value", AVideo ? FSid + QStringLiteral("-video") : FSid);
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

//A transport-replace/transport-accept carries a fresh transport definition in
//its own content element; unlike session-initiate it has no description part.
void JingleSession::addRestartTransport(QDomElement &AJingleElem) const
{
	QDomDocument doc = AJingleElem.ownerDocument();
	QDomElement contentElem = doc.createElement("content");
	contentElem.setAttribute("name", FContentName);
	contentElem.setAttribute("creator", FInitiated ? QStringLiteral("initiator") : QStringLiteral("responder"));
	addTransport(contentElem);
	AJingleElem.appendChild(contentElem);
}

void JingleSession::appendCandidateElement(QDomElement &ATransportElem, const IceCandidate &ACandidate) const
{
	QDomDocument doc = ATransportElem.ownerDocument();

	QDomElement candidateElem = doc.createElement("candidate");
	candidateElem.setAttribute("foundation", QString::number(ACandidate.componentId));
	candidateElem.setAttribute("component", QString::number(ACandidate.componentId));
	candidateElem.setAttribute("generation", QString::number(FMediaEngine ? FMediaEngine->iceAgent()->generation() : 0));
	candidateElem.setAttribute("id", QString("v%1c%2").arg(ACandidate.port).arg(ACandidate.componentId));
	candidateElem.setAttribute("network", "0");
	candidateElem.setAttribute("priority", QString::number(ACandidate.priority));
	//XEP-0176 requires the transport protocol, and strict parsers refuse the whole
	//candidate without it: Snikket/Conversations drops every candidate that lacks
	//protocol ("ignoring invalid ICE candidate Parameter protocol is missing or empty"),
	//so the peer never learns our addresses, never creates TURN permissions for them
	//and the media path stays dead even though signalling succeeds.
	candidateElem.setAttribute("protocol", "udp");
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
	const QString descriptionMedia = descriptionElem.attribute("media");
	if (!descriptionElem.isNull())
	{
		const bool videoDescription = descriptionMedia == QStringLiteral("video");
		if (videoDescription)
			FRemoteVideoContentName = AContentElem.attribute("name", FRemoteVideoContentName);
		else if (!FInitiated)
			//A JMI proposal opens the session before we see the real
			//session-initiate; learn its audio content name for the answer.
			FContentName = AContentElem.attribute("name", FContentName);
		if (!videoDescription)
		{
			FRemotePayloadNames.clear();
			FRemoteOpusPayloadType = -1;
		}

		const QDomNodeList payloadList = descriptionElem.elementsByTagName("payload-type");
		for (int i = 0; i < payloadList.count(); ++i)
		{
			const QDomElement payloadElem = payloadList.at(i).toElement();
			const QString name = payloadElem.attribute("name").toLower();
			if (videoDescription)
			{
				if (name == QStringLiteral("vp8"))
				{
					FRemoteVideoPayloadType = payloadElem.attribute("id").toInt();
					if (FMediaEngine)
						FMediaEngine->setRemoteVideoPayloadType(FRemoteVideoPayloadType);
					LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': peer offers VP8 video, payload=%2").arg(FSid).arg(FRemoteVideoPayloadType));
				}
				continue;
			}
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
		int hostCount = 0;
		int srflxCount = 0;
		int relayCount = 0;
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
			FRemoteCandidateCount++;
			if (candidate.type == "srflx")
				srflxCount++;
			else if (candidate.type == "relay" || candidate.type == "prflx")
				relayCount++;
			else
				hostCount++;
		}
		//Peers that advertise the SDP offer/answer model put everything into a single
		//transport, peers without it trickle every candidate in its own action. Log the
		//summary of both so a missing candidate on either side is easy to spot.
		if (candidateList.count() > 0)
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': remote transport, content='%2' ufrag='%3' candidates=%4 (host=%5 srflx=%6 relay=%7)").
				arg(FSid, AContentElem.attribute("name"), transportElem.attribute("ufrag")).
				arg(candidateList.count()).arg(hostCount).arg(srflxCount).arg(relayCount));
		else
			LOG_STRM_INFO(FStreamJid, QString("Jingle session '%1': remote transport, content='%2' ufrag='%3' carries no candidates").
				arg(FSid, AContentElem.attribute("name"), transportElem.attribute("ufrag")));
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
	FMediaEngine->applyDtlsParameters(asClient, FPeerFingerprint);
}

QString JingleSession::callEventText(const QString &AReason) const
{
	//An answered call is described by its duration, everything else by the reason
	if (FActiveSince.isValid())
	{
		const int secs = qMax(0, int(FActiveSince.secsTo(QDateTime::currentDateTime())));
		const QString duration = QTime(0, 0).addSecs(secs).toString(secs >= 3600 ? "h:mm:ss" : "m:ss");
		return tr("Voice call, %1").arg(duration);
	}

	if (AReason == "decline")
		return FInitiated ? tr("Voice call declined") : tr("Voice call rejected");
	if (AReason == "busy")
		return tr("Voice call, peer is busy");
	if (AReason == "no-answer" || AReason == "expired" || AReason == "gone")
		return FInitiated ? tr("Voice call, no answer") : tr("Missed voice call");
	if (AReason == "cancel" || AReason == "success")
		return FInitiated ? tr("Voice call cancelled") : tr("Missed voice call");
	if (AReason == "failed" || AReason == "failed-application" || AReason == "connectivity-error")
		return tr("Voice call failed");
	return tr("Voice call ended");
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
