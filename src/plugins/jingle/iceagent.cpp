#include "iceagent.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSet>
#include <QUdpSocket>
#include <QTimer>
#include <QtEndian>

#include <definitions/namespaces.h>
#include <utils/logger.h>

//STUN message types and attributes (RFC 5389/8489, RFC 8445 subset)
static const quint16 StunBindingRequest       = 0x0001;
static const quint16 StunBindingSuccess       = 0x0101;
static const quint16 StunBindingError         = 0x0111;

static const quint16 StunAttrUsername         = 0x0006;
static const quint16 StunAttrMessageIntegrity = 0x0008;
static const quint16 StunAttrXorMappedAddress = 0x0020;
static const quint16 StunAttrErrorCode        = 0x0300;
static const quint16 StunAttrPriority         = 0x0024;
static const quint16 StunAttrUseCandidate     = 0x0025;
static const quint16 StunAttrIceControlled    = 0x8029;
static const quint16 StunAttrIceControlling   = 0x802A;
static const quint16 StunAttrFingerprint      = 0x802C;

static const quint32 StunMagicCookie          = 0x21960464;
static const quint32 FingerprintXor           = 0x5354554e; // RFC 5389: CRC32 XOR 0x5354554e ("STUN")
static const int StunHeaderSize               = 20;

//RFC 7666 consent expiration
static const qint64 CheckIntervalMs           = 500;
static const qint64 KeepAliveMs               = 10000;
static const qint64 ConsentTimeoutMs          = 45000;

static QByteArray randomBytes(int ACount)
{
	QByteArray result;
	result.reserve(ACount);
	for (int i = 0; i < ACount; ++i)
		result.append(char(quint8(QRandomGenerator::global()->bounded(256))));
	return result;
}

static QByteArray stunAttribute(quint16 AType, const QByteArray &AValue)
{
	QByteArray result;
	quint16 len = qToBigEndian<quint16>(AValue.size());
	quint16 type = qToBigEndian<quint16>(AType);
	result.append(reinterpret_cast<const char*>(&type), 2);
	result.append(reinterpret_cast<const char*>(&len), 2);
	result.append(AValue);
	while (result.size() % 4 != 0) //attributes are padded to 32 bit
		result.append('\0');
	return result;
}

static QByteArray stunAttribute(quint16 AType, quint32 AValue)
{
	quint32 be = qToBigEndian<quint32>(AValue);
	return stunAttribute(AType, QByteArray(reinterpret_cast<const char*>(&be), 4));
}

static QByteArray stunAttribute(quint16 AType, quint64 AValue)
{
	quint64 be = qToBigEndian<quint64>(AValue);
	return stunAttribute(AType, QByteArray(reinterpret_cast<const char*>(&be), 8));
}

static quint32 crc32(const QByteArray &AData)
{
	static QVector<quint32> table;
	if (table.isEmpty())
	{
		table.resize(256);
		for (quint32 i = 0; i < 256; ++i)
		{
			quint32 c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
	}
	quint32 c = 0xFFFFFFFF;
	for (char ch : AData)
		c = table[quint32((quint8(c) ^ quint8(ch)))] ^ (c >> 8);
	return c ^ 0xFFFFFFFF;
}

static bool isStunData(const QByteArray &AData)
{
	if (AData.size() < StunHeaderSize)
		return false;
	//RFC 7983 leading octet 0b00 covers both STUN flavours:
	// - RFC 5389 "delineating": magic cookie at bytes 4..8, 12-byte transaction id;
	// - RFC 3489 "classic" (XEP-0176 era, still used by libnice-based Dino builds):
	//   no cookie, 16-byte transaction id at bytes 4..20.
	//RTP starts 0x80..0xBF, DTLS 0x14..0x3F, so a leading byte <= 0x01 with a plausible
	//length field is a strong enough discriminator; the cookie must NOT be required.
	if (quint8(AData.at(0)) > 0x01)
		return false;
	const quint16 length = qFromBigEndian<quint16>(AData.constData() + 2);
	return StunHeaderSize + int(length) <= AData.size();
}

static bool isDelineatingStun(const QByteArray &AData)
{
	return AData.size() >= 8 &&
		qFromBigEndian<quint32>(AData.constData() + 4) == StunMagicCookie;
}

IceAgent::IceAgent(QObject *AParent)
	: QObject(AParent)
{
	FRtpSocket = nullptr;
	FRtcpSocket = nullptr;
	FCheckTimer = nullptr;
	FControlling = false;
	FStarted = false;
	FConnected = false;
	FTieBreaker = QRandomGenerator::global()->generate64();
	FLastActivityMs = 0;
	FTrace = 0;
}

IceAgent::~IceAgent()
{
	stop();
}

void IceAgent::setControlling(bool AControlling)
{
	FControlling = AControlling;
}

bool IceAgent::isControlling() const
{
	return FControlling;
}

QString IceAgent::localUfrag() const
{
	return FLocalUfrag;
}

QString IceAgent::localPwd() const
{
	return FLocalPwd;
}

bool IceAgent::start()
{
	if (FStarted)
		return true;

	//RFC 8445: ufrag at least 24 bits, password at least 128 bits
	FLocalUfrag = randomBytes(4).toHex();
	FLocalPwd = randomBytes(32).toHex();

	FRtpSocket = new QUdpSocket(this);
	FRtcpSocket = new QUdpSocket(this);
	if (!FRtpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress) ||
		!FRtcpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress))
	{
		stop();
		return false;
	}

	connect(FRtpSocket, &QUdpSocket::readyRead, this, &IceAgent::onUdpReadyRead);
	connect(FRtcpSocket, &QUdpSocket::readyRead, this, &IceAgent::onUdpReadyRead);

	//Gather host candidates over the usable IPv4 interfaces (RFC 8445, type preference 126)
	int interfaceIndex = 0;
	const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
	for (const QHostAddress &address : addresses)
	{
		if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
			continue;
		for (int component : {1, 2})
		{
			IceCandidate candidate;
			candidate.componentId = component;
			candidate.address = address.toString();
			candidate.port = (component == 1) ? FRtpSocket->localPort() : FRtcpSocket->localPort();
			candidate.priority = hostPriority(component, interfaceIndex);
			FLocalCandidates.append(candidate);
		}
		++interfaceIndex;
	}
	if (FLocalCandidates.isEmpty())
	{
		stop();
		return false;
	}

	FCheckTimer = new QTimer(this);
	FCheckTimer->setInterval(CheckIntervalMs);
	connect(FCheckTimer, &QTimer::timeout, this, &IceAgent::onCheckTimer);
	FCheckTimer->start();

	FStarted = true;
	return true;
}

void IceAgent::stop()
{
	if (FCheckTimer)
	{
		FCheckTimer->stop();
		delete FCheckTimer;
		FCheckTimer = nullptr;
	}
	delete FRtpSocket;
	delete FRtcpSocket;
	FRtpSocket = nullptr;
	FRtcpSocket = nullptr;
	FStarted = false;
	FConnected = false;
	FCheckPairs.clear();
}

QList<IceCandidate> IceAgent::localCandidates() const
{
	return FLocalCandidates;
}

void IceAgent::setRemoteCredentials(const QString &AUfrag, const QString &APwd)
{
	FRemoteUfrag = AUfrag;
	FRemotePwd = APwd;
}

void IceAgent::addRemoteCandidate(const IceCandidate &ACandidate)
{
	FRemoteCandidates.append(ACandidate);
	if (FTrace < 80)
		LOG_INFO(QString("IceAgent: remote candidate %1:%2 comp=%3 added (ufrag %4)").
			arg(ACandidate.address).arg(ACandidate.port).arg(ACandidate.componentId).arg(ACandidate.ufrag));

	if (!FStarted || FConnected)
		return;

	//Pair every remote candidate with the local candidate of the same component
	for (const IceCandidate &local : FLocalCandidates)
	{
		if (local.componentId != ACandidate.componentId)
			continue;
		CheckPair pair;
		pair.local = local;
		pair.remote = ACandidate;
		pair.verified = false;
		pair.nominated = false;
		pair.lastSent = 0;
		pair.sendStyle = 0;
		FCheckPairs.append(pair);
	}
}

void IceAgent::clearRemoteCandidates()
{
	FRemoteCandidates.clear();
	FCheckPairs.clear();
}

bool IceAgent::writeMedia(const QByteArray &AData, int AComponent)
{
	if (!FConnected)
		return false;

	QUdpSocket *socket = socketForComponent(AComponent);
	for (const CheckPair &pair : FCheckPairs)
	{
		if (pair.local.componentId == AComponent && pair.nominated)
			return socket->writeDatagram(AData, QHostAddress(pair.remote.address), pair.remote.port) != -1;
	}
	return false;
}

quint32 IceAgent::hostPriority(int AComponent, int AInterfaceIndex) const
{
	//RFC 8445: (type-preference << 24) | (local-preference << 8) | (256 - component)
	const quint32 typePreference = 126;
	const quint32 localPreference = 65535 - AInterfaceIndex;
	return (typePreference << 24) + (localPreference << 8) + (256 - AComponent);
}

QUdpSocket *IceAgent::socketForComponent(int AComponent) const
{
	return AComponent == 2 ? FRtcpSocket : FRtpSocket;
}

bool IceAgent::sendCheck(CheckPair &APair, bool ANominate)
{
	if (FRemoteUfrag.isEmpty() || FRemotePwd.isEmpty())
		return false;

	//Style per pair: some classic builds (libnice without RFC5389 support, e.g. the
	//Dino 0.2/0.3 packages) never answer delineating requests. We alternate styles on
	//every retransmit until the pair is verified; each style keeps its own pending
	//transaction id. FINGERPRINT/XOR-MAPPED are RFC5389 extensions and stay out of
	//classic packets; both styles authenticate with MESSAGE-INTEGRITY.
	const bool classicStyle = (APair.sendStyle != 0);

	//Delineating style: cookie at bytes 4..8 + 12-byte id at 8..20.
	//Classic style: 16-byte id at bytes 4..20, no cookie. Responses echo bytes 8..20 in
	//both cases, so we match responses on a 12-byte token either way (for classic that is
	//the last 12 bytes of the id).
	const QByteArray transactionId = classicStyle ? randomBytes(16) : randomBytes(12);

	QByteArray attributes;
	//RFC 5245: the username is always "controlling-ufrag:controlled-ufrag", not "remote:local"
	const QString username = FControlling ? FLocalUfrag + ":" + FRemoteUfrag : FRemoteUfrag + ":" + FLocalUfrag;
	attributes += stunAttribute(StunAttrUsername, username.toUtf8());
	attributes += stunAttribute(StunAttrPriority, APair.local.priority);
	attributes += stunAttribute(FControlling ? StunAttrIceControlling : StunAttrIceControlled, FTieBreaker);
	if (ANominate)
		attributes += stunAttribute(StunAttrUseCandidate, QByteArray());

	//MESSAGE-INTEGRITY: HMAC-SHA1 over the header (with adjusted length) + attributes before it
	quint16 type = qToBigEndian<quint16>(StunBindingRequest);
	QByteArray header;
	header.append(reinterpret_cast<const char*>(&type), 2);
	header.append(2, '\0'); //length placeholder, fixed up below
	if (classicStyle) {
		header.append(transactionId); //16-byte transaction id, no cookie
	} else {
		quint32 magic = qToBigEndian<quint32>(StunMagicCookie);
		header.append(reinterpret_cast<const char*>(&magic), 4);
		header.append(transactionId); //12-byte transaction id after the cookie
	}

	QByteArray forIntegrity = header + attributes;
	quint16 integrityLength = qToBigEndian<quint16>(quint16(attributes.size() + 20));
	forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&integrityLength), 2));
	QByteArray hmac = QMessageAuthenticationCode::hash(forIntegrity, FRemotePwd.toUtf8(), QCryptographicHash::Sha1);
	QByteArray withIntegrity = attributes + stunAttribute(StunAttrMessageIntegrity, hmac);

	QByteArray fingerprint;
	if (!classicStyle)
	{
		//fingerprint over everything (with length including the fingerprint attribute itself)
		QByteArray forCrc = header + withIntegrity;
		quint16 crcLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + 8));
		forCrc.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&crcLength), 2));
		quint32 crc = qToBigEndian<quint32>(crc32(forCrc) ^ FingerprintXor);
		fingerprint = stunAttribute(StunAttrFingerprint, QByteArray(reinterpret_cast<const char*>(&crc), 4));
	}

	QByteArray packet = header + withIntegrity + fingerprint;
	quint16 finalLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + fingerprint.size()));
	packet.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&finalLength), 2));

	if (socketForComponent(APair.local.componentId)->writeDatagram(packet, QHostAddress(APair.remote.address), APair.remote.port) < 0)
		return false;

	if (classicStyle)
		APair.classicTransactionId = transactionId.right(12); //bytes 8..20 of the classic id are echoed by the peer
	else
		APair.transactionId = transactionId;
	APair.sendStyle = classicStyle ? 0 : 1; //alternate on the next retransmit
	APair.lastSent = QDateTime::currentMSecsSinceEpoch();
	if (FTrace < 80)
	{
		LOG_INFO(QString("IceAgent: check -> %1:%2 comp=%3 nominate=%4 size=%5 style=%6").
			arg(APair.remote.address).arg(APair.remote.port).arg(APair.local.componentId).arg(ANominate).arg(packet.size()).arg(classicStyle ? "classic" : "delineating"));
	}
	return true;
}

void IceAgent::onUdpReadyRead()
{
	QUdpSocket *socket = qobject_cast<QUdpSocket *>(sender());
	const int componentId = socket == FRtcpSocket ? 2 : 1;

	while (socket->hasPendingDatagrams())
	{
		QByteArray data;
		data.resize(int(socket->pendingDatagramSize()));
		QHostAddress peerAddress;
		quint16 peerPort = 0;
		if (socket->readDatagram(data.data(), data.size(), &peerAddress, &peerPort) < 0)
			continue;

		FLastActivityMs = QDateTime::currentMSecsSinceEpoch();

		if (isStunData(data))
		{
			const quint16 type = qFromBigEndian<quint16>(data.constData());
			if (FTrace < 80)
				LOG_INFO(QString("IceAgent: STUN %1 received from %2:%3 comp=%4").
					arg(type == StunBindingRequest ? "request" : "response", peerAddress.toString()).arg(peerPort).arg(componentId));
			if (type == StunBindingRequest)
				handleStunRequest(data, peerAddress, peerPort, componentId);
			else if (type == StunBindingSuccess || type == StunBindingError)
				handleStunResponse(data, componentId);
		}
		else
		{
			if (FTrace < 80)
			{
				//Temp interop diagnostic: dump the first bytes of anything we treat as media.
				QByteArray head = data.left(24).toHex(' ');
				quint16 sLen = data.size() >= 4 ? qFromBigEndian<quint16>(data.constData() + 2) : 0;
				LOG_INFO(QString("IceAgent: media datagram %1 bytes from %2:%3 comp=%4 first=0x%5 hdr[%6] lenField=%7")
					.arg(data.size()).arg(peerAddress.toString()).arg(peerPort).arg(componentId)
					.arg(quint8(data.at(0)), 2, 16, QChar('0')).arg(head).arg(sLen));
			}
			emit mediaReceived(data, componentId);
		}
	}
}

void IceAgent::handleStunRequest(const QByteArray &AData, const QHostAddress &AFrom, quint16 APort, int AComponent)
{
	//Parse username, role and USE-CANDIDATE; the MESSAGE-INTEGRITY key is our local password
	int offset = StunHeaderSize;
	//Honor the declared message length: agents may pad the datagram (libwebrtc does), and
	//the trailing pad must not be walked as attributes.
	const quint16 declaredLength = qFromBigEndian<quint16>(AData.constData() + 2);
	const int dataEnd = qMin(AData.size(), StunHeaderSize + int(declaredLength));
	QByteArray transactionId = AData.mid(8, 12);
	QByteArray username;
	bool useCandidate = false;
	bool integrityOk = false;
	int integrityStart = -1;

	while (offset + 4 <= dataEnd)
	{
		quint16 attrType = qFromBigEndian<quint16>(AData.constData() + offset);
		quint16 attrLen = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		QByteArray value = AData.mid(offset + 4, attrLen);

		if (attrType == StunAttrUsername)
			username = value;
		else if (attrType == StunAttrMessageIntegrity)
		{
			integrityStart = offset;
			QByteArray forIntegrity = AData.left(offset);
			quint16 adjustedLength = qToBigEndian<quint16>(quint16(forIntegrity.size() - StunHeaderSize + 20));
			forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&adjustedLength), 2));
			QByteArray expected = QMessageAuthenticationCode::hash(forIntegrity, FLocalPwd.toUtf8(), QCryptographicHash::Sha1);
			integrityOk = (expected == value);
		}
		else if (attrType == StunAttrUseCandidate)
			useCandidate = true;

		int pad = (4 - attrLen % 4) % 4;
		offset += 4 + attrLen + pad;
	}

	if (!integrityOk || integrityStart < 0)
	{
		if (FTrace < 80)
			LOG_WARNING(QString("IceAgent: dropping STUN request from %1:%2 — MESSAGE-INTEGRITY check failed").arg(AFrom.toString()).arg(APort));
		return;
	}
	//The username is "controlling-ufrag:controlled-ufrag" (RFC 5245). Role conflicts
	//(both sides believing they control, which XEP-0166 initiator vs WebRTC answerer
	//disagreement produces) must not brick ICE: the MESSAGE-INTEGRITY above already
	//authenticated the sender by the shared pwd, so accept either orientation.
	const QString firstUfrag = QString::fromUtf8(username).section(':', 0, 0);
	const QString secondUfrag = QString::fromUtf8(username).section(':', 1, 1);
	if (username.isEmpty() || (firstUfrag != FLocalUfrag && secondUfrag != FLocalUfrag))
	{
		if (FTrace < 80)
			LOG_WARNING(QString("IceAgent: dropping STUN request from %1:%2 — username '%3' does not contain our ufrag '%4'").
				arg(AFrom.toString()).arg(APort).arg(QString::fromUtf8(username), FLocalUfrag));
		return;
	}

	//Answer the check in the SAME style the request arrived (classic agents match the
	//16-byte id region; delineating peers expect the cookie back), with MESSAGE-INTEGRITY
	//keyed by our pwd; XOR-MAPPED-ADDRESS/FINGERPRINT only for RFC 5389 style.
	const bool delineating = isDelineatingStun(AData);
	{
		QByteArray header;
		quint16 type = qToBigEndian<quint16>(StunBindingSuccess);
		header.append(reinterpret_cast<const char*>(&type), 2);
		header.append(2, '\0'); //length placeholder
		if (delineating)
		{
			quint32 magic = qToBigEndian<quint32>(StunMagicCookie);
			header.append(reinterpret_cast<const char*>(&magic), 4);
			header.append(AData.mid(8, 12));
		}
		else
		{
			header.append(AData.mid(4, 16)); //echo the classic transaction id region verbatim
		}

		QByteArray attributes;
		if (delineating && AFrom.protocol() == QAbstractSocket::IPv4Protocol)
		{
			QByteArray value;
			value.append(char(0x00));                                            //reserved
			value.append(char(0x01));                                            //family IPv4
			quint16 port = qToBigEndian<quint16>(quint16(APort) ^ quint16(StunMagicCookie >> 16));
			value.append(reinterpret_cast<const char*>(&port), 2);
			quint32 addr = qToBigEndian<quint32>(AFrom.toIPv4Address() ^ StunMagicCookie);
			value.append(reinterpret_cast<const char*>(&addr), 4);
			attributes += stunAttribute(StunAttrXorMappedAddress, value);
		}

		//MESSAGE-INTEGRITY over header(length = attrs + 20) + attrs
		QByteArray forIntegrity = header + attributes;
		quint16 integrityLength = qToBigEndian<quint16>(quint16(attributes.size() + 20));
		forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&integrityLength), 2));
		QByteArray hmac = QMessageAuthenticationCode::hash(forIntegrity, FLocalPwd.toUtf8(), QCryptographicHash::Sha1);
		QByteArray withIntegrity = attributes + stunAttribute(StunAttrMessageIntegrity, hmac);

		QByteArray fingerprint;
		if (delineating)
		{
			//FINGERPRINT over everything, length field covering the fingerprint attribute too
			QByteArray forCrc = header + withIntegrity;
			quint16 crcLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + 8));
			forCrc.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&crcLength), 2));
			quint32 crc = qToBigEndian<quint32>(crc32(forCrc) ^ FingerprintXor);
			fingerprint = stunAttribute(StunAttrFingerprint, QByteArray(reinterpret_cast<const char*>(&crc), 4));
		}

		QByteArray packet = header + withIntegrity + fingerprint;
		quint16 finalLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + fingerprint.size()));
		packet.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&finalLength), 2));

		socketForComponent(AComponent)->writeDatagram(packet, AFrom, APort);
	}

	if (useCandidate)
	{
		IceCandidate remote;
		remote.componentId = AComponent;
		remote.address = AFrom.toString();
		remote.port = APort;
		updateNominated(remote, AComponent);
	}
}

void IceAgent::handleStunResponse(const QByteArray &AData, int AComponent)
{
	const QByteArray transactionId = AData.mid(8, 12);
	const quint16 type = qFromBigEndian<quint16>(AData.constData());
	if (type != StunBindingSuccess)
		return;

	for (CheckPair &pair : FCheckPairs)
	{
		if (pair.local.componentId != AComponent)
			continue;
		if (pair.transactionId != transactionId && pair.classicTransactionId != transactionId)
			continue;
		if (transactionId.isEmpty())
			continue;
		pair.transactionId.clear();
		pair.classicTransactionId.clear();
		if (!pair.verified)
		{
			pair.verified = true;
			if (FTrace < 80)
				LOG_INFO(QString("IceAgent: pair %1:%2 comp=%3 verified").arg(pair.remote.address).arg(pair.remote.port).arg(AComponent));
			if (FControlling)
				tryNominate();
		}
		return;
	}
}

void IceAgent::updateNominated(const IceCandidate &ARemote, int AComponent)
{
	//Controlled role: the controlling peer tells us which pair to use
	for (CheckPair &pair : FCheckPairs)
	{
		if (pair.local.componentId == AComponent &&
			pair.remote.address == ARemote.address && pair.remote.port == ARemote.port)
			pair.nominated = true;
	}
	//Also accept a nominated source address that was not in the candidate list (post-nomination traffic)
	if (!FConnected)
	{
		//Mirror the controller's nomination set: with rtcp-mux only component 1 is nominated
		QSet<int> requiredComponents;
		for (const IceCandidate &remote : FRemoteCandidates)
		{
			if (remote.componentId == 1 || remote.componentId == 2)
				requiredComponents.insert(remote.componentId);
		}
		if (requiredComponents.isEmpty())
			requiredComponents << 1 << 2; //remote list unknown yet, wait for both

		bool allComponents = true;
		for (int component : requiredComponents)
		{
			bool found = false;
			for (const CheckPair &pair : FCheckPairs)
				found = found || (pair.local.componentId == component && pair.nominated);
			allComponents = allComponents && found;
		}
		if (allComponents)
		{
			FConnected = true;
			resetConsent();
			emit connected();
		}
	}
}

void IceAgent::tryNominate()
{
	if (FConnected || !FControlling)
		return;

	//Nominate the first verified pair per component the remote actually offered. With
	//rtcp-mux the remote may only send component-1 candidates, so requiring component 2
	//would stall the call forever.
	QSet<int> requiredComponents;
	for (const IceCandidate &remote : FRemoteCandidates)
	{
		if (remote.componentId == 1 || remote.componentId == 2)
			requiredComponents.insert(remote.componentId);
	}
	if (requiredComponents.isEmpty())
		return;

	IceCandidate selected[3];
	for (int component : requiredComponents)
	{
		for (CheckPair &pair : FCheckPairs)
		{
			if (pair.local.componentId == component && pair.verified)
			{
				selected[component] = pair.remote;
				break;
			}
		}
		if (selected[component].isNull())
			return; //This required component has no verified pair yet
	}

	for (CheckPair &pair : FCheckPairs)
	{
		if (!pair.verified)
			continue;
		if (pair.remote.address == selected[pair.local.componentId].address &&
			pair.remote.port == selected[pair.local.componentId].port)
		{
			//Send the nomination twice so it arrives in both STUN styles; whichever the
			//peer parses, it sees USE-CANDIDATE on a verified pair.
			sendCheck(pair, true);
			sendCheck(pair, true);
			pair.nominated = true;
		}
	}
	FConnected = true;
	resetConsent();
	emit connected();
}

void IceAgent::resetConsent()
{
	FLastActivityMs = QDateTime::currentMSecsSinceEpoch();
}

void IceAgent::onCheckTimer()
{
	if (!FStarted)
		return;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();

	if (FConnected)
	{
		//Consent freshness: keep checks flowing on the nominated pair and expire without traffic
		if (now - FLastActivityMs > ConsentTimeoutMs)
		{
			FConnected = false;
			emit disconnected(tr("Consent to send data expired"));
			return;
		}
		for (CheckPair &pair : FCheckPairs)
		{
			if (pair.nominated && now - pair.lastSent > KeepAliveMs)
				sendCheck(pair, false);
		}
		return;
	}

	if (FRemoteCandidates.isEmpty())
		return;

	for (CheckPair &pair : FCheckPairs)
	{
		if (pair.transactionId.isEmpty() && now - pair.lastSent > CheckIntervalMs * 2)
			sendCheck(pair, false);
	}
}
