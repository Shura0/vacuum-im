#ifndef IJINGLE_H
#define IJINGLE_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <utils/jid.h>

#define IJINGLE_UUID "{164f76b6-327b-43da-9cca-24068c295598}"

class IJingle
{
public:
	enum CallState {
		CallOutgoing,  // session-initiate has been sent, waiting for the peer to acknowledge the signaling
		CallRinging,   // an incoming call is ringing locally
		CallRingback,  // the signaling is established, waiting for the peer to answer
		CallActive,    // both parties accepted the session (media is established separately)
		CallFinished   // the session is terminated
	};

	virtual QObject *instance() =0;
	//Calls
	virtual bool isSupported(const Jid &AStreamJid, const Jid &AContactJid) const =0;
	virtual QString call(const Jid &AStreamJid, const Jid &AContactJid) =0;
	virtual bool answerCall(const QString &ASid) =0;
	virtual bool declineCall(const QString &ASid) =0;
	virtual bool hangupCall(const QString &ASid) =0;
	virtual CallState callState(const QString &ASid) const =0;
	virtual Jid callPeer(const QString &ASid) const =0;
protected:
	virtual void callStarted(const Jid &AStreamJid, const Jid &AContactJid, const QString &ASid) =0;
	virtual void callReceived(const Jid &AStreamJid, const Jid &AContactJid, const QString &ASid) =0;
	virtual void callStateChanged(const QString &ASid, IJingle::CallState AState) =0;
	virtual void callFinished(const QString &ASid, const QString &AReason) =0;
};

Q_DECLARE_METATYPE(IJingle::CallState);
Q_DECLARE_INTERFACE(IJingle,"Vacuum.Plugin.IJingle/1.0");

#endif // IJINGLE_H
