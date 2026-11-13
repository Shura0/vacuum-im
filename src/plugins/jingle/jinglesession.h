#ifndef JINGLESESSION_H
#define JINGLESESSION_H

#include <QObject>
#include <QStringList>

#include <interfaces/ijingle.h>
#include <utils/jid.h>
#include <utils/stanza.h>

#include "callwindow.h"
#include "mediaengine.h"

class Jingle;

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
	//Called when the server/peer acknowledges our session-initiate (sendStanzaRequest result)
	void onInitiateResult(bool AOk, const QString &AErrorText);
	void setSilent(bool ASilent);
	void showWindow();

public slots:
	void setUserClosed();

signals:
	void stateChanged(IJingle::CallState AState);
	void finished(const QString &AReason, const QString &AText);

private:
	void setState(IJingle::CallState AState);
	void finishLocally(const QString &AReason, const QString &AText);
	Stanza makeSessionAction(const QString &AAction, QDomElement *AJingleElem = nullptr);
	void addContentElement(QDomElement &AJingleElem) const;
	void addDescription(QDomElement &AContentElem) const;
	void addTransport(QDomElement &AContentElem) const;
	void addReason(QDomElement &AJingleElem, const QString &ACode, const QString &AText) const;
	void processContentElem(const QDomElement &AContentElem);
	QString ourDtlsSetup() const;
	void applyDtlsClientRole();
	QString reasonText(const QString &AReason) const;

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
	MediaEngine *FMediaEngine;

	QString FPeerFingerprint;
	QString FPeerDtlsSetup;
	QList<QString> FRemotePayloadNames; //Codec names offered by the peer
	int FRemoteOpusPayloadType;
	int FRemoteOpusClockrate;
	bool FTerminated;
};

#endif // JINGLESESSION_H
