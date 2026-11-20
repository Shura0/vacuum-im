#ifndef JINGLESESSION_H
#define JINGLESESSION_H

#include <QDateTime>
#include <QObject>
#include <QStringList>

#include <interfaces/ijingle.h>
#include <utils/jid.h>
#include <utils/stanza.h>

#include "callwindow.h"
#include "callsounds.h"
#include "mediaengine.h"

class Jingle;
class QTimer;

class JingleSession :
	public QObject
{
	Q_OBJECT;
public:
	JingleSession(const Jid &AStreamJid, const Jid &ALocalJid, const Jid &AContactJid, const QString &AInitiatorJid, const QString &ASid, bool AInitiated, Jingle *AJingle, const QString &APeerName);
	virtual ~JingleSession();

	static QString generateSid();

	QString sid() const;
	IJingle::CallState state() const;
	Jid streamJid() const;
	Jid contactJid() const;
	Jid localJid() const;
	QString peerName() const;
	bool isInitiator() const;

	CallWindow *window() const;

	//Outgoing actions
	void initiate();
	void answer();
	void decline();
	void hangup(const QString &AText = QString());
	//Incoming actions; AReply receives the acknowledgement/error to send back
	bool processStanza(const Stanza &AStanza, Stanza &AReply);
	//Remembers the content description of a received session-initiate
	void storeInitiate(const QDomElement &AJingleElem);
	//XEP-0353 Jingle Message Initiation
	void storeJmiPropose(const QDomElement &ADescriptionElem);
	bool processJmiMessage(const QString &AAction, const QDomElement &AJmiElem);
	//Called when the server/peer acknowledges our session-initiate (sendStanzaRequest result)
	void onInitiateResult(bool AOk, const QString &AErrorText);
	void setSilent(bool ASilent);
	void showWindow();

public slots:
	void setUserClosed();

signals:
	void stateChanged(IJingle::CallState AState);
	void finished(const QString &AReason, const QString &AText);

private slots:
	void onIceRestartNeeded();
	void onJmiTimeout();
private:
	void setState(IJingle::CallState AState);
	void finishLocally(const QString &AReason, const QString &AText);
	Stanza makeSessionAction(const QString &AAction, QDomElement *AJingleElem = nullptr);
	void addContentElement(QDomElement &AJingleElem) const;
	void appendContentElem(QDomElement &AJingleElem, const QString &AName, bool AVideo) const;
	void addDescription(QDomElement &AContentElem, bool AVideo = false) const;
	void addTransport(QDomElement &AContentElem) const;
	void addRestartTransport(QDomElement &AJingleElem) const;
	void appendCandidateElement(QDomElement &ATransportElem, const IceCandidate &ACandidate) const;
	void addTransportInfo(QDomElement &AJingleElem, const IceCandidate &ACandidate) const;
	void addReason(QDomElement &AJingleElem, const QString &ACode, const QString &AText) const;
	void processContentElem(const QDomElement &AContentElem);
	void continueInitiate(bool AForce = false);
	void continueAnswer(bool AForce = false);
	void armGatherWait();
	void onGatheringFinished();
	void onGatherWaitTimeout();
	void onCandidateGathered(const IceCandidate &ACandidate);
	void armPeerWait();
	void onPeerWaitTimeout();
	QString ourDtlsSetup() const;
	void applyDtlsClientRole();
	void sendJmiMessage(const QString &AAction, const QString &AReasonCode = QString(), const QString &AText = QString());
	void resendInitiate();
	void finishJmiSession(const QString &AReasonCode, const QString &AText);
	QString reasonText(const QString &AReason) const;
	//Text of the one-line call event that goes into the chat history
	QString callEventText(const QString &AReason) const;

private:
	Jid FStreamJid;
	Jid FLocalJid;
	Jid FContactJid;
	Jid FTargetJid; //Where session actions are sent (the initiator of the session)
	QString FInitiator; //The value of the 'initiator' attribute we exchange in every action
	QString FPeerName;
	QString FSid;
	bool FInitiated;
	bool FSilent;
	int FResendId;
	QString FContentName; //Echo the initiator's content name (e.g. "0") in our answer
	IJingle::CallState FState;
	Jingle *FJingle;
	CallWindow *FWindow;
	CallSounds *FSounds;
	MediaEngine *FMediaEngine;
	QTimer *FGatherTimer;
	QTimer *FPeerWaitTimer;
	QTimer *FJmiTimer;
	bool FJmiAwaitingProceed;
	bool FJmiPending;
	bool FJmiWaitingInitiate;
	bool FJmiFinishSent;
	bool FJmiUsed;
	bool FJmiTransportReceived;
	int FRemoteCandidateCount;
	int FIceRestartCount;
	bool FIceRestartPending;
	bool FOfferPending;
	bool FAnswerPending;
	bool FTransportSent;

	QString FPeerFingerprint;
	QString FPeerDtlsSetup;
	QList<QString> FRemotePayloadNames; //Codec names offered by the peer
	int FRemoteOpusPayloadType;
	int FRemoteOpusClockrate;
	int FRemoteVideoPayloadType;
	QString FRemoteVideoContentName;
	QStringList FRemoteContentOrder;
	QDateTime FActiveSince; //When the call became active, invalid if it never did
	bool FTerminated;
};

#endif // JINGLESESSION_H
