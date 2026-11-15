#include "turnclient.h"

#include <QDateTime>
#include <QHostInfo>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>

#include <utils/logger.h>

//RFC 5766 defaults and retransmission parameters. The RTO schedule is the one
//from RFC 5389 section 7.2.1 (500 ms doubling, seven transmissions overall).
static const quint16 TurnDefaultPort = 3478;
static const quint32 TurnDefaultLifetimeSec = 600;
static const int TurnPermissionRefreshSec = 240;    //permissions live 300 s
static const int TurnPermissionRetrySec = 60;       //pause after a failed permission
static const qint64 TurnInitialRtoMs = 500;
static const qint64 TurnMaxRtoMs = 1600;
static const int TurnMaxAttempts = 7;
static const int TurnTimerIntervalMs = 250;

//REQUESTED-TRANSPORT value: the protocol number of UDP in the first byte, the
//rest of the 32-bit word is zero (RFC 5766 section 6.1).
static QByteArray requestedTransportUdp()
{
	QByteArray value;
	value.append(char(17));
	value.append(3, '\0');
	return value;
}

TurnClient::TurnClient(QObject *AParent) : QObject(AParent)
{
	FSocket = nullptr;
	FTimer = nullptr;
	FServerPort = TurnDefaultPort;
	FRelayPort = 0;
	FLifetimeSec = 0;
	FRefreshAtMs = 0;
	FStarted = false;
	FAllocated = false;
	FFailing = false;
}

TurnClient::~TurnClient()
{
	stop();
}

void TurnClient::setServer(const QString &AHost, quint16 APort)
{
	FServerHost = AHost.trimmed();
	FServerPort = APort != 0 ? APort : TurnDefaultPort;
}

void TurnClient::setCredentials(const QString &AUsername, const QString &APassword)
{
	FUsername = AUsername;
	FPassword = APassword;
}

bool TurnClient::isAllocated() const
{
	return FAllocated;
}

QHostAddress TurnClient::relayAddress() const
{
	return FRelayAddress;
}

quint16 TurnClient::relayPort() const
{
	return FRelayPort;
}

quint16 TurnClient::localPort() const
{
	return FSocket != nullptr ? FSocket->localPort() : 0;
}

bool TurnClient::start()
{
	if (FStarted)
		return true;

	if (FServerHost.isEmpty() || FServerPort == 0)
	{
		fail(QString("no TURN server configured"));
		return false;
	}
	if (FUsername.isEmpty() && !FPassword.isEmpty())
	{
		//Harmless: an anonymous TURN server may not need credentials, keep going
		LOG_DEBUG(QString("TurnClient: password set without a username"));
	}

	FSocket = new QUdpSocket(this);
	if (!FSocket->bind(QHostAddress::AnyIPv4, 0))
	{
		const QString error = FSocket->errorString();
		FSocket->deleteLater();
		FSocket = nullptr;
		fail(QString("failed to bind the relay socket: %1").arg(error));
		return false;
	}
	connect(FSocket, &QUdpSocket::readyRead, this, &TurnClient::onReadyRead);

	FTimer = new QTimer(this);
	FTimer->setInterval(TurnTimerIntervalMs);
	connect(FTimer, &QTimer::timeout, this, &TurnClient::onTimer);
	FTimer->start();

	FStarted = true;
	FAllocated = false;
	FFailing = false;
	FPending.clear();
	FPermissions.clear();
	FKey.clear();
	FNonce.clear();
	FRealm.clear();
	FRelayAddress = QHostAddress();
	FRelayPort = 0;
	FLifetimeSec = 0;
	FRefreshAtMs = 0;

	LOG_INFO(QString("TurnClient: allocating on %1:%2 (local port %3)").arg(FServerHost).arg(FServerPort).arg(FSocket->localPort()));

	QHostInfo::lookupHost(FServerHost, this, [this](const QHostInfo &AInfo) {
		onServerResolved(AInfo);
	});
	return true;
}

void TurnClient::stop()
{
	FStarted = false;
	FAllocated = false;
	FRefreshAtMs = 0;
	FPending.clear();
	FPermissions.clear();

	if (FTimer != nullptr)
	{
		FTimer->stop();
		FTimer->deleteLater();
		FTimer = nullptr;
	}
	if (FSocket != nullptr)
	{
		FSocket->disconnect(this);
		FSocket->close();
		FSocket->deleteLater();
		FSocket = nullptr;
	}
}

void TurnClient::onServerResolved(const QHostInfo &AInfo)
{
	if (!FStarted || FFailing)
		return;

	QHostAddress address;
	for (const QHostAddress &candidate : AInfo.addresses())
	{
		if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
		{
			address = candidate;
			break;
		}
	}
	if (address.isNull())
	{
		fail(QString("failed to resolve TURN server '%1': %2").arg(FServerHost, AInfo.errorString()));
		return;
	}

	FServerAddress = address;
	LOG_DEBUG(QString("TurnClient: TURN server %1 resolved to %2").arg(FServerHost).arg(address.toString()));
	sendAllocate();
}

void TurnClient::appendAuth(QList<Stun::Attribute> &AAttributes) const
{
	if (FKey.isEmpty())
		return;
	AAttributes.append(Stun::Attribute(Stun::AttributeUsername, FUsername.toUtf8()));
	AAttributes.append(Stun::Attribute(Stun::AttributeRealm, FRealm.toUtf8()));
	AAttributes.append(Stun::Attribute(Stun::AttributeNonce, FNonce));
}

void TurnClient::resendRequest(quint16 AType, const QStringList &AAddresses)
{
	if (AType == Stun::AllocateRequest)
		sendAllocate();
	else if (AType == Stun::RefreshRequest)
		sendRefresh(quint32(FLifetimeSec > 0 ? FLifetimeSec : TurnDefaultLifetimeSec));
	else if (AType == Stun::CreatePermissionRequest)
		sendPermissions();
}

void TurnClient::sendAllocate()
{
	QList<Stun::Attribute> attributes;
	attributes.append(Stun::Attribute(Stun::AttributeRequestedTransport, requestedTransportUdp()));
	appendAuth(attributes);

	const QByteArray transactionId = Stun::transactionId();
	const QByteArray packet = Stun::buildMessage(Stun::AllocateRequest, transactionId, attributes, FKey);

	LOG_DEBUG(QString("TurnClient: Allocate request txn=%1 authenticated=%2").arg(QString(transactionId.toHex())).arg(!FKey.isEmpty()));
	postRequest(packet, transactionId, Stun::AllocateRequest);
}

void TurnClient::sendRefresh(quint32 ALifetime)
{
	if (!FStarted || FSocket == nullptr)
		return;

	QList<Stun::Attribute> attributes;
	const quint32 lifetime = qToBigEndian<quint32>(ALifetime);
	attributes.append(Stun::Attribute(Stun::AttributeLifetime, QByteArray(reinterpret_cast<const char*>(&lifetime), 4)));
	appendAuth(attributes);

	const QByteArray transactionId = Stun::transactionId();
	const QByteArray packet = Stun::buildMessage(Stun::RefreshRequest, transactionId, attributes, FKey);

	//Do not resend on every timer tick while the refresh is in flight
	FRefreshAtMs = QDateTime::currentMSecsSinceEpoch() + 30000;

	LOG_DEBUG(QString("TurnClient: Refresh request lifetime=%1").arg(ALifetime));
	postRequest(packet, transactionId, Stun::RefreshRequest);
}

void TurnClient::sendPermissions()
{
	if (!FStarted || !FAllocated || FSocket == nullptr)
		return;

	//One batched permission request at a time
	for (const PendingRequest &pending : FPending)
	{
		if (pending.type == Stun::CreatePermissionRequest)
			return;
	}

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	QStringList addresses;
	for (auto it = FPermissions.constBegin(); it != FPermissions.constEnd(); ++it)
	{
		if (it.value() <= now)
			addresses.append(it.key());
	}
	if (addresses.isEmpty())
		return;

	QList<Stun::Attribute> attributes;
	QStringList requested;
	for (const QString &address : addresses)
	{
		const QByteArray encoded = Stun::xorAddressAttribute(Stun::AttributeXorPeerAddress, QHostAddress(address), 0);
		if (encoded.isEmpty())
			continue;
		attributes.append(Stun::Attribute(Stun::AttributeXorPeerAddress, encoded.mid(4)));
		requested.append(address);
	}
	if (requested.isEmpty())
		return;
	appendAuth(attributes);

	const QByteArray transactionId = Stun::transactionId();
	const QByteArray packet = Stun::buildMessage(Stun::CreatePermissionRequest, transactionId, attributes, FKey);

	LOG_DEBUG(QString("TurnClient: CreatePermission for %1").arg(requested.join(',')));
	postRequest(packet, transactionId, Stun::CreatePermissionRequest, requested);
}

void TurnClient::postRequest(const QByteArray &APacket, const QByteArray &ATransactionId, quint16 AType, const QStringList &AAddresses)
{
	if (FSocket == nullptr || FServerAddress.isNull())
		return;

	PendingRequest request;
	request.packet = APacket;
	request.transactionId = ATransactionId;
	request.type = AType;
	request.addresses = AAddresses;
	request.attempts = 1;
	request.rtoMs = TurnInitialRtoMs;
	request.lastSentMs = QDateTime::currentMSecsSinceEpoch();
	FPending.append(request);

	FSocket->writeDatagram(APacket, FServerAddress, FServerPort);
}

bool TurnClient::sendTo(const QByteArray &AData, const QHostAddress &APeerAddress, quint16 APeerPort)
{
	if (!FAllocated || FSocket == nullptr || FServerAddress.isNull())
		return false;
	if (APeerAddress.protocol() != QAbstractSocket::IPv4Protocol)
		return false;

	const QByteArray encoded = Stun::xorAddressAttribute(Stun::AttributeXorPeerAddress, APeerAddress, APeerPort);
	if (encoded.isEmpty())
		return false;

	QList<Stun::Attribute> attributes;
	attributes.append(Stun::Attribute(Stun::AttributeXorPeerAddress, encoded.mid(4)));
	attributes.append(Stun::Attribute(Stun::AttributeData, AData));
	const QByteArray packet = Stun::buildMessage(Stun::SendIndication, Stun::transactionId(), attributes);

	return FSocket->writeDatagram(packet, FServerAddress, FServerPort) == qint64(packet.size());
}

void TurnClient::addPermission(const QHostAddress &APeerAddress)
{
	if (APeerAddress.protocol() != QAbstractSocket::IPv4Protocol || APeerAddress.isNull())
		return;

	const QString address = APeerAddress.toString();
	if (!FPermissions.contains(address))
		FPermissions.insert(address, 0);

	if (FAllocated)
		sendPermissions();
}

void TurnClient::onReadyRead()
{
	QPointer<TurnClient> self(this);

	while (self != nullptr && FSocket != nullptr && FSocket->hasPendingDatagrams())
	{
		QByteArray data;
		data.resize(int(FSocket->pendingDatagramSize()));
		QHostAddress from;
		quint16 port = 0;
		if (FSocket->readDatagram(data.data(), data.size(), &from, &port) < 0)
			continue;

		if (!Stun::looksLikeStun(data))
		{
			//Only STUN travels on this 5-tuple; anything else would be a stray
			//datagram from the server address, safe to ignore.
			LOG_DEBUG(QString("TurnClient: ignoring %1 bytes of non-STUN data from %2:%3").arg(data.size()).arg(from.toString()).arg(port));
			continue;
		}
		if (!FServerAddress.isNull() && from != FServerAddress)
		{
			LOG_WARNING(QString("TurnClient: ignoring STUN from unexpected %1:%2 (server is %3)").arg(from.toString()).arg(port).arg(FServerAddress.toString()));
			continue;
		}

		handleMessage(data);
	}
}

void TurnClient::handleMessage(const QByteArray &AData)
{
	Stun::Message message;
	if (!message.parse(AData) || message.transactionId.size() != 12)
	{
		LOG_DEBUG(QString("TurnClient: failed to parse a STUN message of %1 bytes").arg(AData.size()));
		return;
	}

	if (message.type == Stun::DataIndication)
	{
		handleIndication(message);
		return;
	}

	int index = -1;
	for (int i = 0; i < FPending.count(); ++i)
	{
		if (FPending.at(i).transactionId == message.transactionId)
		{
			index = i;
			break;
		}
	}
	if (index < 0)
	{
		LOG_DEBUG(QString("TurnClient: response type=0x%1 for an unknown transaction %2").
			arg(message.type, 4, 16, QChar('0')).arg(QString(message.transactionId.toHex())));
		return;
	}

	handleResponse(message, index);
}

void TurnClient::handleResponse(const Stun::Message &AMessage, int AIndex)
{
	const PendingRequest request = FPending.at(AIndex);

	quint16 successType = 0;
	quint16 errorType = 0;
	switch (request.type)
	{
	case Stun::AllocateRequest:
		successType = Stun::AllocateSuccess;
		errorType = Stun::AllocateError;
		break;
	case Stun::RefreshRequest:
		successType = Stun::RefreshSuccess;
		errorType = Stun::RefreshError;
		break;
	case Stun::CreatePermissionRequest:
		successType = Stun::CreatePermissionSuccess;
		errorType = Stun::CreatePermissionError;
		break;
	default:
		break;
	}

	if (AMessage.type == successType)
	{
		FPending.removeAt(AIndex);
		switch (request.type)
		{
		case Stun::AllocateRequest:
			handleAllocateResponse(AMessage);
			break;
		case Stun::RefreshRequest:
			handleRefreshResponse(AMessage);
			break;
		case Stun::CreatePermissionRequest:
			handlePermissionResponse(AMessage, request.addresses);
			break;
		default:
			break;
		}
		return;
	}

	if (AMessage.type != errorType)
	{
		FPending.removeAt(AIndex);
		LOG_WARNING(QString("TurnClient: unexpected response type=0x%1 to request type=0x%2").
			arg(AMessage.type, 4, 16, QChar('0')).arg(request.type, 4, 16, QChar('0')));
		return;
	}

	const int code = AMessage.errorCode();
	const QByteArray nonce = AMessage.value(Stun::AttributeNonce);
	const QString reason = AMessage.errorReason();

	if (code == 401 && FKey.isEmpty())
	{
		FRealm = QString::fromUtf8(AMessage.value(Stun::AttributeRealm));
		if (!nonce.isEmpty())
			FNonce = nonce;
		if (FRealm.isEmpty() || FNonce.isEmpty())
		{
			fail(QString("TURN server did not send a realm/nonce in the authentication challenge"));
			return;
		}
		FKey = Stun::longTermKey(FUsername, FRealm, FPassword);
		FPending.removeAt(AIndex);
		LOG_INFO(QString("TurnClient: authentication challenge realm='%1', retrying with credentials").arg(FRealm));
		resendRequest(request.type, request.addresses);
		return;
	}

	if (code == 438 && !nonce.isEmpty())
	{
		FNonce = nonce;
		if (FKey.isEmpty())
			FKey = Stun::longTermKey(FUsername, FRealm, FPassword);
		FPending.removeAt(AIndex);
		LOG_INFO(QString("TurnClient: stale nonce, retrying request type=0x%1").arg(request.type, 4, 16, QChar('0')));
		resendRequest(request.type, request.addresses);
		return;
	}

	FPending.removeAt(AIndex);

	const QString message = QString("TURN request type=0x%1 failed: %2 '%3'").
		arg(request.type, 4, 16, QChar('0')).arg(code).arg(reason);
	LOG_WARNING(message);

	if (request.type == Stun::AllocateRequest || request.type == Stun::RefreshRequest || code == 401)
	{
		fail(message);
	}
	else
	{
		//A failed permission is not fatal: the peer simply gets no traffic until
		//the next attempt, so back off instead of hammering the server.
		const qint64 retryAt = QDateTime::currentMSecsSinceEpoch() + TurnPermissionRetrySec * 1000;
		for (const QString &address : request.addresses)
			FPermissions.insert(address, retryAt);
	}
}

void TurnClient::handleAllocateResponse(const Stun::Message &AMessage)
{
	const QHostAddress relayAddress = AMessage.xorAddress(Stun::AttributeXorRelayedAddress);
	const quint16 relayPort = AMessage.xorPort(Stun::AttributeXorRelayedAddress);
	if (relayAddress.isNull() || relayPort == 0)
	{
		fail(QString("TURN allocate success without a usable XOR-RELAYED-ADDRESS"));
		return;
	}

	const QByteArray lifetime = AMessage.value(Stun::AttributeLifetime);
	FLifetimeSec = lifetime.size() >= 4 ? int(qFromBigEndian<quint32>(lifetime.constData())) : int(TurnDefaultLifetimeSec);
	if (FLifetimeSec <= 0)
		FLifetimeSec = int(TurnDefaultLifetimeSec);

	FRelayAddress = relayAddress;
	FRelayPort = relayPort;
	FAllocated = true;
	FFailing = false;

	int refreshSec = qMax(10, FLifetimeSec / 2);
	if (refreshSec > FLifetimeSec - 5)
		refreshSec = qMax(5, FLifetimeSec - 5);
	FRefreshAtMs = QDateTime::currentMSecsSinceEpoch() + qint64(refreshSec) * 1000;

	LOG_INFO(QString("TurnClient: allocation ready, relayed %1:%2 lifetime=%3s, refresh in %4s").
		arg(FRelayAddress.toString()).arg(FRelayPort).arg(FLifetimeSec).arg(refreshSec));

	emit allocated(FRelayAddress, FRelayPort);

	//Permissions requested before the allocation completed can be sent now
	sendPermissions();
}

void TurnClient::handleRefreshResponse(const Stun::Message &AMessage)
{
	const QByteArray lifetime = AMessage.value(Stun::AttributeLifetime);
	if (lifetime.size() >= 4)
		FLifetimeSec = int(qFromBigEndian<quint32>(lifetime.constData()));
	if (FLifetimeSec <= 0)
		FLifetimeSec = int(TurnDefaultLifetimeSec);

	int refreshSec = qMax(10, FLifetimeSec / 2);
	if (refreshSec > FLifetimeSec - 5)
		refreshSec = qMax(5, FLifetimeSec - 5);
	FRefreshAtMs = QDateTime::currentMSecsSinceEpoch() + qint64(refreshSec) * 1000;

	LOG_DEBUG(QString("TurnClient: allocation refreshed, lifetime=%1s").arg(FLifetimeSec));
}

void TurnClient::handlePermissionResponse(const Stun::Message &AMessage, const QStringList &AAddresses)
{
	Q_UNUSED(AMessage);
	const qint64 refreshAt = QDateTime::currentMSecsSinceEpoch() + TurnPermissionRefreshSec * 1000;
	for (const QString &address : AAddresses)
		FPermissions.insert(address, refreshAt);
	LOG_DEBUG(QString("TurnClient: permissions installed for %1").arg(AAddresses.join(',')));
}

void TurnClient::handleIndication(const Stun::Message &AMessage)
{
	const QHostAddress peerAddress = AMessage.xorAddress(Stun::AttributeXorPeerAddress);
	const quint16 peerPort = AMessage.xorPort(Stun::AttributeXorPeerAddress);
	const QByteArray data = AMessage.value(Stun::AttributeData);
	if (peerAddress.isNull() || peerPort == 0)
	{
		LOG_DEBUG(QString("TurnClient: Data indication without a usable peer address"));
		return;
	}

	emit dataReceived(data, peerAddress, peerPort);
}

void TurnClient::onTimer()
{
	if (!FStarted)
		return;

	retransmit();

	if (FAllocated)
	{
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (FRefreshAtMs > 0 && now >= FRefreshAtMs)
			sendRefresh(quint32(FLifetimeSec > 0 ? FLifetimeSec : int(TurnDefaultLifetimeSec)));
		sendPermissions();
	}
}

void TurnClient::retransmit()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (int i = FPending.count() - 1; i >= 0; --i)
	{
		PendingRequest &request = FPending[i];
		if (now - request.lastSentMs < request.rtoMs)
			continue;

		if (request.attempts >= TurnMaxAttempts)
		{
			const quint16 type = request.type;
			FPending.removeAt(i);
			const QString message = QString("TURN request type=0x%1 got no response after %2 attempts").
				arg(type, 4, 16, QChar('0')).arg(TurnMaxAttempts);
			if (type == Stun::AllocateRequest || type == Stun::RefreshRequest)
			{
				fail(message);
				return;
			}
			LOG_WARNING(message);
			continue;
		}

		request.attempts++;
		request.rtoMs = qMin(request.rtoMs * 2, TurnMaxRtoMs);
		request.lastSentMs = now;
		if (FSocket != nullptr && !FServerAddress.isNull())
		{
			FSocket->writeDatagram(request.packet, FServerAddress, FServerPort);
			LOG_DEBUG(QString("TurnClient: retransmit type=0x%1 attempt=%2 rto=%3ms").
				arg(request.type, 4, 16, QChar('0')).arg(request.attempts).arg(request.rtoMs));
		}
	}
}

void TurnClient::fail(const QString &AReason)
{
	if (FFailing)
		return;
	FFailing = true;
	LOG_WARNING(QString("TurnClient: %1").arg(AReason));
	stop();
	//Never report a failure from inside datagram/handshake processing: the
	//receiver is allowed to delete this object in its slot.
	QMetaObject::invokeMethod(this, [this, AReason]() { emit failed(AReason); }, Qt::QueuedConnection);
}
