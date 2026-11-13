#include "iceagent.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>
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
static const quint16 StunAttrErrorCode        = 0x0009;
static const quint16 StunAttrPriority         = 0x0024;
static const quint16 StunAttrUseCandidate     = 0x0025;
static const quint16 StunAttrIceControlled    = 0x8029;
static const quint16 StunAttrIceControlling   = 0x802A;
static const quint16 StunAttrFingerprint      = 0x8028; //delineating marker: libnice REQUIRES it and rejects the whole message without it

//RFC 5389 section 6: the magic cookie is 0x2112A442. A message whose bytes 4..8 are not
//exactly this value is NOT a 5389 message - peers silently discard such requests, and we
//would misclassify their (correct) requests as legacy RFC 3489 ones.
static const quint32 StunMagicCookie          = 0x2112A442;
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
	//RTP/RTCP start with 0x80..0xBF and DTLS with 0x14..0x3F, so the leading octet alone
	//is a complete discriminator (RFC 7983). Neither the magic cookie nor the length field
	//may be trusted here: peers already seen in the wild send cookie-less (classic) STUN,
	//and padding/length quirks must not push their checks into the media path.
	return quint8(AData.at(0)) <= 0x01;
}

//RFC 5389 15.5: FINGERPRINT is optional but MUST be validated when present. libnice (Dino)
//and libwebrtc are strict about it, so a wrong CRC means the whole message is discarded.
static bool fingerprintValid(const QByteArray &AData)
{
	int offset = StunHeaderSize;
	const quint16 declaredLength = qFromBigEndian<quint16>(AData.constData() + 2);
	const int dataEnd = qMin(AData.size(), StunHeaderSize + int(declaredLength));
	while (offset + 4 <= dataEnd)
	{
		const quint16 attrType = qFromBigEndian<quint16>(AData.constData() + offset);
		const quint16 attrLen = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		if (attrType == StunAttrFingerprint && attrLen == 4)
		{
			QByteArray forCrc = AData.left(offset);
			const quint16 crcLength = qToBigEndian<quint16>(quint16(offset - StunHeaderSize + 8));
			forCrc.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&crcLength), 2));
			const quint32 expected = crc32(forCrc) ^ FingerprintXor;
			const quint32 actual = qFromBigEndian<quint32>(AData.constData() + offset + 4);
			return expected == actual;
		}
		offset += 4 + attrLen + (4 - attrLen % 4) % 4;
	}
	return true; //Absent attribute: allowed (classic RFC 3489 agents never send one)
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
		pair.attempts = 0;
		pair.rtoMs = CheckIntervalMs;
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
		{
			const qint64 written = socket->writeDatagram(AData, QHostAddress(pair.remote.address), pair.remote.port);
			LOG_DEBUG(QString("IceAgent: media out %1 bytes comp=%2 to %3:%4 written=%5").
				arg(AData.size()).arg(AComponent).arg(pair.remote.address).arg(pair.remote.port).arg(written));
			return written != -1;
		}
	}
	static int s_dropped = 0;
	if (++s_dropped <= 5)
		LOG_WARNING(QString("IceAgent: media out dropped, no nominated pair for comp=%1 (connected=%2)").arg(AComponent).arg(FConnected));
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

	//Outgoing checks are always RFC 5389 (magic cookie + 12-byte transaction id +
	//FINGERPRINT): every ICE implementation in use today (libnice in Dino, libwebrtc in
	//Snikket/Conversations, Swiften) speaks 5389. We still ANSWER legacy RFC 3489
	//requests in their own style - mirroring the request is required there.
	const QByteArray transactionId = randomBytes(12);

	QByteArray attributes;
	//RFC 8445 section 7.2.2: the username is "<peer-ufrag>:<sender-ufrag>", i.e. the ufrag
	//of the agent that RECEIVES the request comes first - in both roles. Sending it the
	//other way round makes strict implementations (libnice, libwebrtc inside Dino/Snikket)
	//drop the check as coming from an unknown session.
	const QString username = FRemoteUfrag + ":" + FLocalUfrag;
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
	quint32 magic = qToBigEndian<quint32>(StunMagicCookie);
	header.append(reinterpret_cast<const char*>(&magic), 4);
	header.append(transactionId); //12-byte transaction id after the cookie

	QByteArray forIntegrity = header + attributes;
	//RFC 5389 15.4: when signing, the header length must point just past MESSAGE-INTEGRITY,
	//i.e. attributes size + 24 (4-byte attribute header + 20-byte HMAC). Using +20 makes
	//every receiver reject the check (verified against the RFC 5769 test vector).
	quint16 integrityLength = qToBigEndian<quint16>(quint16(attributes.size() + 24));
	forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&integrityLength), 2));
	QByteArray hmac = QMessageAuthenticationCode::hash(forIntegrity, FRemotePwd.toUtf8(), QCryptographicHash::Sha1);
	QByteArray withIntegrity = attributes + stunAttribute(StunAttrMessageIntegrity, hmac);

	//FINGERPRINT over everything (with length including the fingerprint attribute itself)
	QByteArray forCrc = header + withIntegrity;
	quint16 crcLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + 8));
	forCrc.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&crcLength), 2));
	quint32 crc = qToBigEndian<quint32>(crc32(forCrc) ^ FingerprintXor);
	QByteArray fingerprint = stunAttribute(StunAttrFingerprint, QByteArray(reinterpret_cast<const char*>(&crc), 4));

	QByteArray packet = header + withIntegrity + fingerprint;
	quint16 finalLength = qToBigEndian<quint16>(quint16(withIntegrity.size() + fingerprint.size()));
	packet.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&finalLength), 2));

	if (socketForComponent(APair.local.componentId)->writeDatagram(packet, QHostAddress(APair.remote.address), APair.remote.port) < 0)
		return false;

	APair.transactionId = transactionId;
	APair.lastPacket = packet;
	APair.lastSent = QDateTime::currentMSecsSinceEpoch();
	if (FTrace < 80)
	{
	{
		LOG_DEBUG(QString("IceAgent: check -> %1:%2 comp=%3 nominate=%4 size=%5").
			arg(APair.remote.address).arg(APair.remote.port).arg(APair.local.componentId).arg(ANominate).arg(packet.size()));
		//Debug aid: the first check of a pair, so the wire format can be replayed
		//through an independent STUN implementation when interop breaks.
		if (APair.attempts == 0)
			LOG_DEBUG(QString("IceAgent: check bytes %1").arg(QString(packet.toHex())));
	}
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
			if (FTrace < 200)
				LOG_INFO(QString("IceAgent: STUN %1 type=0x%2 received from %3:%4 comp=%5").
					arg(type == StunBindingRequest ? "request" : "response").arg(type, 4, 16, QChar('0')).
					arg(peerAddress.toString()).arg(peerPort).arg(componentId));
			if (type == StunBindingRequest)
				handleStunRequest(data, peerAddress, peerPort, componentId);
			else if (type == StunBindingSuccess || type == StunBindingError)
				handleStunResponse(data, componentId);
		}
		else
		{
			if (FTrace < 80)
			{
				//Debug aid: what we classified as media (a DTLS record starts with 0x16).
				QByteArray head = data.left(24).toHex(' ');
				quint16 sLen = data.size() >= 4 ? qFromBigEndian<quint16>(data.constData() + 2) : 0;
				LOG_DEBUG(QString("IceAgent: media datagram %1 bytes from %2:%3 comp=%4 first=0x%5 hdr[%6] lenField=%7")
					.arg(data.size()).arg(peerAddress.toString()).arg(peerPort).arg(componentId)
					.arg(quint8(data.at(0)), 2, 16, QChar('0')).arg(head).arg(sLen));
			}
			emit mediaReceived(data, componentId);
		}
	}
}

static int errorCodeOf(const QByteArray &AData)
{
	int offset = StunHeaderSize;
	const quint16 declaredLength = qFromBigEndian<quint16>(AData.constData() + 2);
	const int dataEnd = qMin(AData.size(), StunHeaderSize + int(declaredLength));
	while (offset + 4 <= dataEnd)
	{
		const quint16 attrType = qFromBigEndian<quint16>(AData.constData() + offset);
		const quint16 attrLen = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		if (attrType == StunAttrErrorCode && attrLen >= 4)
			return (quint8(AData.at(offset + 6)) & 0x07) * 100 + quint8(AData.at(offset + 7));
		offset += 4 + attrLen + (4 - attrLen % 4) % 4;
	}
	return 0;
}

void IceAgent::logErrorCode(const QByteArray &AData)
{
	int offset = StunHeaderSize;
	const quint16 declaredLength = qFromBigEndian<quint16>(AData.constData() + 2);
	const int dataEnd = qMin(AData.size(), StunHeaderSize + int(declaredLength));
	while (offset + 4 <= dataEnd)
	{
		const quint16 attrType = qFromBigEndian<quint16>(AData.constData() + offset);
		const quint16 attrLen = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		if (attrType == StunAttrErrorCode && attrLen >= 4)
		{
			LOG_WARNING(QString("IceAgent: peer STUN error %1 '%2'").
				arg(errorCodeOf(AData)).arg(QString::fromUtf8(AData.mid(offset + 8, attrLen - 4))));
			return;
		}
		offset += 4 + attrLen + (4 - attrLen % 4) % 4;
	}
}

void IceAgent::sendStunError(const QByteArray &ARequest, const QHostAddress &ATo, quint16 APort, int AComponent, int ACode, const QString &AReason)
{
	const bool delineating = isDelineatingStun(ARequest);
	QByteArray header;
	const quint16 type = qToBigEndian<quint16>(StunBindingError);
	header.append(reinterpret_cast<const char*>(&type), 2);
	header.append(2, '\0');
	if (delineating)
	{
		const quint32 magic = qToBigEndian<quint32>(StunMagicCookie);
		header.append(reinterpret_cast<const char*>(&magic), 4);
		header.append(ARequest.mid(8, 12));
	}
	else
	{
		header.append(ARequest.mid(4, 16));
	}
	QByteArray errorValue;
	errorValue.append(2, '\0');
	errorValue.append(char((ACode / 100) & 0x07));
	errorValue.append(char(ACode % 100));
	errorValue.append(AReason.toUtf8());
	QByteArray attributes = stunAttribute(StunAttrErrorCode, errorValue);
	QByteArray forIntegrity = header + attributes;
	const quint16 integrityLength = qToBigEndian<quint16>(quint16(attributes.size() + 24));
	forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&integrityLength), 2));
	const QByteArray hmac = QMessageAuthenticationCode::hash(forIntegrity, FLocalPwd.toUtf8(), QCryptographicHash::Sha1);
	QByteArray packet = header + attributes + stunAttribute(StunAttrMessageIntegrity, hmac);
	const quint16 finalLength = qToBigEndian<quint16>(quint16(packet.size() - StunHeaderSize));
	packet.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&finalLength), 2));
	socketForComponent(AComponent)->writeDatagram(packet, ATo, APort);
}

void IceAgent::handleStunRequest(const QByteArray &AData, const QHostAddress &AFrom, quint16 APort, int AComponent)
{
	if (!fingerprintValid(AData))
	{
		static int s_warned = 0;
		if (++s_warned <= 5)
			LOG_WARNING(QString("IceAgent: dropping STUN request from %1:%2 with a bad FINGERPRINT").arg(AFrom.toString()).arg(APort));
		return;
	}

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
	int peerRole = 0; //1 = peer claims ICE-CONTROLLING, -1 = ICE-CONTROLLED, 0 = absent
	quint64 peerTieBreaker = 0;

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
			//Same rule as when signing: the coverage length ends just past this attribute.
			quint16 adjustedLength = qToBigEndian<quint16>(quint16(forIntegrity.size() - StunHeaderSize + 24));
			forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&adjustedLength), 2));
			QByteArray expected = QMessageAuthenticationCode::hash(forIntegrity, FLocalPwd.toUtf8(), QCryptographicHash::Sha1);
			integrityOk = (expected == value);
		}
		else if (attrType == StunAttrUseCandidate)
			useCandidate = true;
		else if (attrType == StunAttrIceControlling || attrType == StunAttrIceControlled)
		{
			peerRole = attrType == StunAttrIceControlling ? 1 : -1;
			peerTieBreaker = 0;
			for (int i = 0; i < value.size() && i < 8; ++i)
				peerTieBreaker = (peerTieBreaker << 8) | quint8(value.at(i));
		}

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

	//RFC 8445 7.3.1.1: both agents must not keep the controlling role. The agent with the
	//higher tie-breaker keeps it; the other switches. Otherwise the loser answers 487.
	if ((peerRole == 1 && FControlling) || (peerRole == -1 && !FControlling))
	{
		const bool weKeepRole = (peerRole == 1) ? (FTieBreaker >= peerTieBreaker) : (FTieBreaker < peerTieBreaker);
		if (weKeepRole)
		{
			if (FTrace < 200)
				LOG_WARNING(QString("IceAgent: role conflict with %1:%2, answering 487").arg(AFrom.toString()).arg(APort));
			sendStunError(AData, AFrom, APort, AComponent, 487, "Role Conflict");
			return;
		}
		FControlling = !FControlling;
		if (FTrace < 200)
			LOG_WARNING(QString("IceAgent: role conflict with %1:%2, switching to %3").
				arg(AFrom.toString()).arg(APort).arg(FControlling ? "controlling" : "controlled"));
		//Re-check every pair under the new role with a fresh transaction
		for (CheckPair &pair : FCheckPairs)
		{
			pair.transactionId.clear();
			pair.attempts = 0;
			pair.rtoMs = CheckIntervalMs;
		}
	}

	{
		static int s_requestTrace = 0;
		if (s_requestTrace < 40)
		{
			++s_requestTrace;
			LOG_DEBUG(QString("IceAgent: peer check from %1:%2 comp=%3 role=%4 use-candidate=%5 txn=%6 username=%7").
				arg(AFrom.toString()).arg(APort).arg(AComponent).
				arg(peerRole == 1 ? "controlling" : (peerRole == -1 ? "controlled" : "none")).
				arg(useCandidate ? "yes" : "no").arg(QString(AData.mid(8, 12).toHex())).
				arg(QString::fromUtf8(username)));
		}
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

		//MESSAGE-INTEGRITY over header(length = attrs + 24, i.e. incl. the MI attribute) + attrs
		QByteArray forIntegrity = header + attributes;
		quint16 integrityLength = qToBigEndian<quint16>(quint16(attributes.size() + 24));
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
		//Debug aid: the answer itself, decodable offline with libnice.
		static int s_answerTrace = 0;
		if (s_answerTrace < 3)
		{
			++s_answerTrace;
			LOG_DEBUG(QString("IceAgent: answer sent type=0x%1 size=%2 %3").
				arg(StunBindingSuccess, 4, 16, QChar('0')).arg(packet.size()).arg(QString(packet.toHex())));
		}
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
	if (!fingerprintValid(AData))
	{
		static int s_warned = 0;
		if (++s_warned <= 5)
			LOG_WARNING(QString("IceAgent: dropping STUN response with a bad FINGERPRINT (%1 bytes)").arg(AData.size()));
		return;
	}

	const QByteArray transactionId = AData.mid(8, 12);
	const quint16 type = qFromBigEndian<quint16>(AData.constData());
	//Debug aid: the first responses, decodable offline (type, ERROR-CODE, attributes).
	static int s_dumped = 0;
	if (s_dumped < 4)
	{
		++s_dumped;
		LOG_DEBUG(QString("IceAgent: response hex type=0x%1 size=%2 %3").
			arg(type, 4, 16, QChar('0')).arg(AData.size()).arg(QString(AData.toHex())));
	}
	if (type != StunBindingSuccess)
	{
		if (FTrace < 200)
		{
			LOG_WARNING(QString("IceAgent: STUN response type=0x%1 comp=%2 txn=%3 rejected").
				arg(type, 4, 16, QChar('0')).arg(AComponent).arg(QString(transactionId.toHex())));
			logErrorCode(AData);
			if (type == StunBindingError && errorCodeOf(AData) == 487)
			{
				//RFC 8445 7.2.5.1: 487 means the peer insists on the other role
				FControlling = !FControlling;
				LOG_WARNING(QString("IceAgent: role conflict reported by peer, switching to %1").
					arg(FControlling ? "controlling" : "controlled"));
				for (CheckPair &pair : FCheckPairs)
				{
					pair.transactionId.clear();
					pair.attempts = 0;
					pair.rtoMs = CheckIntervalMs;
				}
			}
		}
		return;
	}

	bool matched = false;
	for (CheckPair &pair : FCheckPairs)
	{
		if (pair.local.componentId != AComponent)
			continue;
		if (pair.transactionId != transactionId)
			continue;
		matched = true;
		if (transactionId.isEmpty())
			continue;
		pair.transactionId.clear();
		pair.attempts = 0;
		pair.rtoMs = CheckIntervalMs;
		if (!pair.verified)
		{
			pair.verified = true;
			if (FTrace < 80)
				LOG_INFO(QString("IceAgent: pair %1:%2 comp=%3 verified (nominated=%4)").
					arg(pair.remote.address).arg(pair.remote.port).arg(AComponent).arg(pair.nominated));
			if (FControlling)
				tryNominate();
			else if (pair.nominated && !FConnected)
			{
				//The controller nominated this pair before our own check finished
				FConnected = true;
				resetConsent();
				emit connected();
			}
		}
		return;
	}
	if (!matched)
	{
		QStringList pending;
		for (const CheckPair &pair : FCheckPairs)
			if (pair.local.componentId == AComponent && !pair.transactionId.isEmpty())
				pending << QString(pair.transactionId.toHex());
		LOG_WARNING(QString("IceAgent: success response comp=%1 txn=%2 matches no pending transaction (pending: %3)").
			arg(AComponent).arg(QString(transactionId.toHex()), pending.join(',')));
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
	//The controller decides which pairs carry the session. It routinely nominates only
	//component 1 when rtcp-mux is in play, even though it advertised component-2
	//candidates, so waiting for "every advertised component" deadlocks the call.
	if (!FConnected)
	{
		for (const CheckPair &pair : FCheckPairs)
		{
			if (pair.nominated && pair.verified)
			{
				FConnected = true;
				resetConsent();
				emit connected();
				break;
			}
		}
	}
}

void IceAgent::tryNominate()
{
	if (FConnected || !FControlling)
		return;

	//Nominate every pair that already passed a check. Requiring a verified pair for each
	//component the peer advertised is wrong: with rtcp-mux the peer lists component-2
	//candidates but never makes component 2 usable, and the call would never start.
	bool nominated = false;
	for (CheckPair &pair : FCheckPairs)
	{
		if (!pair.verified)
			continue;
		if (FTrace < 80)
			LOG_INFO(QString("IceAgent: nominating %1:%2 comp=%3").arg(pair.remote.address).arg(pair.remote.port).arg(pair.local.componentId));
		sendCheck(pair, true);
		pair.nominated = true;
		nominated = true;
	}
	if (!nominated)
		return;

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
		if (!pair.transactionId.isEmpty())
		{
			//RFC 5389 7.2.1: retransmit the SAME request with an exponentially growing
			//RTO. Without this a single dropped check stalls ICE for the whole session.
			if (now - pair.lastSent <= pair.rtoMs)
				continue;
			if (pair.attempts >= 7)
			{
				pair.transactionId.clear();
				pair.attempts = 0;
				pair.rtoMs = CheckIntervalMs;
				continue;
			}
			QUdpSocket *socket = socketForComponent(pair.local.componentId);
			if (socket && !pair.lastPacket.isEmpty())
			{
				socket->writeDatagram(pair.lastPacket, QHostAddress(pair.remote.address), pair.remote.port);
				++pair.attempts;
				pair.rtoMs = qMin<qint64>(pair.rtoMs * 2, 1600);
				pair.lastSent = now;
				if (FTrace < 80)
					LOG_DEBUG(QString("IceAgent: retransmit check -> %1:%2 comp=%3 attempt=%4").
						arg(pair.remote.address).arg(pair.remote.port).arg(pair.local.componentId).arg(pair.attempts));
			}
			continue;
		}
		if (now - pair.lastSent > CheckIntervalMs * 2)
			sendCheck(pair, false);
	}
}
