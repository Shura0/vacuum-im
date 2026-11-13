#ifndef ICEAGENT_H
#define ICEAGENT_H

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

class QUdpSocket;
class QTimer;

struct IceCandidate
{
	IceCandidate() {
		componentId = 0;
		port = 0;
		priority = 0;
	}

	int componentId; //1 - RTP, 2 - RTCP
	QString address; //IPv4 (XEP-0176 transport)
	quint16 port;
	quint32 priority;
	QString ufrag;   //The owner's local ufrag for remote candidates

	inline bool isNull() const {
		return address.isEmpty() || port == 0;
	}
};

/*
 * Minimal host-candidate ICE agent for the XEP-0176 ICE-UDP transport (RFC 5245-lite,
 * like most XMPP clients implement it): gathers host candidates over two UDP sockets
 * (RTP/RTCP components), performs STUN binding checks with MESSAGE-INTEGRITY keyed by
 * the ICE password, and nominates a pair on both roles (controlling side nominates
 * actively, controlled side reacts on the USE-CANDIDATE flag).
 */
class IceAgent :
	public QObject
{
	Q_OBJECT;
public:
	explicit IceAgent(QObject *AParent = nullptr);
	virtual ~IceAgent();

	void setControlling(bool AControlling);
	bool isControlling() const;

	QString localUfrag() const;
	QString localPwd() const;

	bool start(); //Binds sockets and gathers host candidates
	void stop();

	QList<IceCandidate> localCandidates() const; //Valid after start()

	void setRemoteCredentials(const QString &AUfrag, const QString &APwd);
	void addRemoteCandidate(const IceCandidate &ACandidate);
	void clearRemoteCandidates();

	bool writeMedia(const QByteArray &AData, int AComponent);

signals:
	void connected();
	void disconnected(const QString &AReason);
	void mediaReceived(const QByteArray &AData, int AComponent);

private slots:
	void onUdpReadyRead();
	void onCheckTimer();

private:
	struct CheckPair {
		IceCandidate local;
		IceCandidate remote;
		QByteArray transactionId; //Pending request, empty when not waiting for a response
		QByteArray lastPacket;    //Serialized request, retransmitted byte-identically (RFC 5389 RTO)
		int attempts;             //Retransmissions of the pending transaction
		qint64 rtoMs;             //Current retransmission timeout, doubles per attempt
		bool verified;
		bool nominated;
		qint64 lastSent;
	};

	quint32 hostPriority(int AComponent, int AInterfaceIndex) const;
	QUdpSocket *socketForComponent(int AComponent) const;
	bool sendCheck(CheckPair &APair, bool ANominate);
	void handleStunRequest(const QByteArray &AData, const QHostAddress &AFrom, quint16 APort, int AComponent);
	void logErrorCode(const QByteArray &AData);
	void sendStunError(const QByteArray &ARequest, const QHostAddress &ATo, quint16 APort, int AComponent, int ACode, const QString &AReason);
	void handleStunResponse(const QByteArray &AData, int AComponent);
	void updateNominated(const IceCandidate &ARemote, int AComponent);
	void tryNominate();
	void resetConsent();

private:
	QUdpSocket *FRtpSocket;
	QUdpSocket *FRtcpSocket;
	QTimer *FCheckTimer;

	bool FControlling;
	bool FStarted;
	bool FConnected;

	QString FLocalUfrag;
	QString FLocalPwd;
	QString FRemoteUfrag;
	QString FRemotePwd;
	quint64 FTieBreaker;

	QList<IceCandidate> FLocalCandidates;
	QList<IceCandidate> FRemoteCandidates;
	QList<CheckPair> FCheckPairs;

	qint64 FLastActivityMs; //Consent freshness (RFC 7666)
	int FTrace;             //rate limit for the interop debugging trace
};

#endif // ICEAGENT_H
