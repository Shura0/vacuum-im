#ifndef ICEAGENT_H
#define ICEAGENT_H

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

#include "natservice.h"

class QUdpSocket;
class QTimer;
class TurnClient;

struct IceCandidate
{
	IceCandidate() {
		componentId = 0;
		port = 0;
		priority = 0;
		relatedPort = 0;
		type = QStringLiteral("host");
	}

	int componentId; //1 - RTP, 2 - RTCP
	QString address; //IPv4 (XEP-0176 transport)
	quint16 port;
	quint32 priority;
	QString ufrag;   //The owner's local ufrag for remote candidates
	QString type;    //"host", "srflx" or "relay" (RFC 8445)
	QString relatedAddress; //srflx: mapped local base, relay: the base candidate address
	quint16 relatedPort;

	inline bool isNull() const {
		return address.isEmpty() || port == 0;
	}
	inline bool isRelay() const {
		return type == QStringLiteral("relay");
	}
};

/*
 * Minimal ICE agent for the XEP-0176 ICE-UDP transport (RFC 5245/8445-lite,
 * like most XMPP clients implement it): gathers host candidates over two UDP
 * sockets (RTP/RTCP components), can add a server-reflexive candidate from a
 * STUN server and a relayed candidate from a TURN server, performs STUN binding
 * checks with MESSAGE-INTEGRITY keyed by the ICE password, and nominates the
 * best pair on both roles (the controlling side nominates actively, the
 * controlled side reacts on the USE-CANDIDATE flag).
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

	//NAT traversal servers and the relay-only test mode. Must be set before start().
	void setNatConfig(const NatConfig &AConfig);

	QString localUfrag() const;
	QString localPwd() const;
	QString remoteUfrag() const;

	bool start(); //Binds sockets and gathers candidates
	void stop();
	//Starts a new ICE generation: fresh local ufrag/pwd and sockets, keeping
	//the same object so the media engine's signal connections stay intact.
	bool restart();
	int generation() const;

	bool isGatheringFinished() const;
	QList<IceCandidate> localCandidates() const; //Valid after start()

	void setRemoteCredentials(const QString &AUfrag, const QString &APwd);
	void addRemoteCandidate(const IceCandidate &ACandidate);
	void clearRemoteCandidates();

	bool writeMedia(const QByteArray &AData, int AComponent);

signals:
	void connected();
	void disconnected(const QString &AReason);
	void mediaReceived(const QByteArray &AData, int AComponent);
	//A candidate discovered after start() (srflx/relay); late candidates are
	//sent to the peer with a transport-info action.
	void candidateGathered(const IceCandidate &ACandidate);
	//Gathering is over (immediately when no STUN/TURN servers are configured).
	//Emitted once per generation when checks have been going out for 10 s
	//without any response and the peer only offered private host candidates.
	void restartNeeded();
	void gatheringFinished();

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

	quint32 candidatePriority(const QString &AType, int AComponent, int AIndex) const;
	static int typePreference(const QString &AType);
	QUdpSocket *socketForComponent(int AComponent) const;
	//Routes a datagram over the path of the pair: the UDP sockets for
	//host/srflx, the TURN allocation for relay pairs.
	bool sendPairPacket(const CheckPair &APair, const QByteArray &AData);
	bool writePathPacket(const QByteArray &AData, const QHostAddress &ATo, quint16 APort, int AComponent, bool AViaRelay);
	bool sendCheck(CheckPair &APair, bool ANominate);
	void handleStunRequest(const QByteArray &AData, const QHostAddress &AFrom, quint16 APort, int AComponent, bool AViaRelay);
	void logErrorCode(const QByteArray &AData);
	void sendStunError(const QByteArray &ARequest, const QHostAddress &ATo, quint16 APort, int AComponent, int ACode, const QString &AReason, bool AViaRelay);
	void addPeerReflexiveCandidate(const QHostAddress &AAddress, quint16 APort, int AComponent, bool AViaRelay, quint32 APriority);
	void handleStunResponse(const QByteArray &AData, int AComponent);
	void updateNominated(const IceCandidate &ARemote, int AComponent, bool AViaRelay);
	void tryNominate();
	int bestPairIndex(int AComponent) const;
	void resetConsent();

	//NAT traversal gathering
	void startNatGathering();
	void onStunServerResolved(const QHostAddress &AAddress);
	void onTurnAllocated(const QHostAddress &ARelayAddress, quint16 ARelayPort);
	void onTurnFailed(const QString &AReason);
	void onTurnData(const QByteArray &AData, const QHostAddress &APeerAddress, quint16 APeerPort);
	void finishGathering();

private:
	QUdpSocket *FRtpSocket;
	QUdpSocket *FRtcpSocket;
	QTimer *FCheckTimer;
	QTimer *FGatherTimer;
	TurnClient *FTurn;

	NatConfig FNatConfig;
	bool FForceRelay;

	bool FControlling;
	bool FStarted;
	bool FConnected;
	bool FGathering;
	bool FGatheringFinished;

	QByteArray FStunPacket;        //Serialized srflx request, retransmitted as-is
	QByteArray FStunTransactionId; //Non-empty while the srflx request is pending
	QHostAddress FStunServer;
	quint16 FStunPort;
	int FStunAttempts;
	qint64 FStunLastSent;
	qint64 FStunRtoMs;
	bool FStunPending;
	bool FTurnPending;

	QString FLocalUfrag;
	QString FLocalPwd;
	QString FRemoteUfrag;
	QString FRemotePwd;
	quint64 FTieBreaker;

	QList<IceCandidate> FLocalCandidates;
	QList<IceCandidate> FRemoteCandidates;
	QList<CheckPair> FCheckPairs;

	qint64 FLastActivityMs; //Consent freshness (RFC 7666)
	qint64 FCheckStartMs;   //When the first check pair appeared, for the stall report
	qint64 FCheckSentCount;
	qint64 FStunInboundCount;
	int FStunRequestCount;  //Inbound binding requests; zero means an ICE-lite peer
	bool FNoConnectivityReported;
	bool FRestartSuggested;
	int FGeneration;
	int FTrace;             //rate limit for the interop debugging trace
};

#endif // ICEAGENT_H
