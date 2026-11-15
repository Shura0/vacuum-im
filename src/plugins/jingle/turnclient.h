#ifndef TURNCLIENT_H
#define TURNCLIENT_H

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "stunmessage.h"

class QHostInfo;
class QTimer;
class QUdpSocket;

/*
 * TURN client (RFC 5766/8656) used for the relayed ICE candidate.
 *
 * A TURN allocation lives on its own UDP socket and 5-tuple, so the client owns
 * the socket instead of sharing the media one. The allocation is requested with
 * long-term credentials (the 401 challenge supplies the realm and the nonce, the
 * key is MD5(username:realm:password)), refreshed before it expires, and used in
 * the indication style: Send indications carry outgoing media, Data indications
 * deliver the peer's datagrams. ChannelBind would save 36 bytes per packet but
 * is optional in the protocol and not needed for a 20 ms Opus stream.
 *
 * Only the UDP transport is supported (the Jingle ICE-UDP transport), only IPv4
 * addresses are handled.
 */
class TurnClient :
	public QObject
{
	Q_OBJECT;
public:
	explicit TurnClient(QObject *AParent = nullptr);
	virtual ~TurnClient();

	void setServer(const QString &AHost, quint16 APort);
	void setCredentials(const QString &AUsername, const QString &APassword);

	bool start();
	void stop();

	bool isAllocated() const;
	QHostAddress relayAddress() const;
	quint16 relayPort() const;
	quint16 localPort() const;

	//Sends a datagram to the peer through the relay (Send indication). The
	//server drops it until a permission for the peer's IP exists, so callers
	//should addPermission() for every candidate as soon as it is known.
	bool sendTo(const QByteArray &AData, const QHostAddress &APeerAddress, quint16 APeerPort);
	//Requests (or refreshes) the permission for a peer IP
	void addPermission(const QHostAddress &APeerAddress);

signals:
	void allocated(const QHostAddress &ARelayAddress, quint16 ARelayPort);
	void failed(const QString &AReason);
	void dataReceived(const QByteArray &AData, const QHostAddress &APeerAddress, quint16 APeerPort);

private slots:
	void onReadyRead();
	void onTimer();

private:
	struct PendingRequest
	{
		QByteArray packet;
		QByteArray transactionId;
		quint16 type;
		QStringList addresses; //XOR-PEER-ADDRESS entries of a permission request
		int attempts;
		qint64 rtoMs;
		qint64 lastSentMs;
	};

	void onServerResolved(const QHostInfo &AInfo);
	void appendAuth(QList<Stun::Attribute> &AAttributes) const;
	void resendRequest(quint16 AType, const QStringList &AAddresses);
	void sendAllocate();
	void sendRefresh(quint32 ALifetime);
	void sendPermissions();
	void postRequest(const QByteArray &APacket, const QByteArray &ATransactionId, quint16 AType, const QStringList &AAddresses = QStringList());
	void handleMessage(const QByteArray &AData);
	void handleResponse(const Stun::Message &AMessage, int AIndex);
	void handleAllocateResponse(const Stun::Message &AMessage);
	void handleRefreshResponse(const Stun::Message &AMessage);
	void handlePermissionResponse(const Stun::Message &AMessage, const QStringList &AAddresses);
	void handleIndication(const Stun::Message &AMessage);
	void fail(const QString &AReason);
	void retransmit();

private:
	QUdpSocket *FSocket;
	QTimer *FTimer;

	QString FServerHost;
	quint16 FServerPort;
	QHostAddress FServerAddress;
	QString FUsername;
	QString FPassword;

	QString FRealm;
	QByteArray FNonce;
	QByteArray FKey;

	QHostAddress FRelayAddress;
	quint16 FRelayPort;
	int FLifetimeSec;
	qint64 FRefreshAtMs;

	QList<PendingRequest> FPending;
	QHash<QString, qint64> FPermissions; //Peer IP -> when the permission must be refreshed

	bool FStarted;
	bool FAllocated;
	bool FFailing;
};

#endif // TURNCLIENT_H
