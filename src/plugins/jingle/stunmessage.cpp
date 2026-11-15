#include "stunmessage.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QVector>
#include <QtEndian>

namespace Stun
{

static const quint32 MagicCookie = 0x2112A442;
static const quint32 FingerprintXor = 0x5354554e; //"STUN"
static const int HeaderSize = 20;

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

Message::Message()
{
	type = 0;
}

bool Message::parse(const QByteArray &AData)
{
	attributes.clear();
	transactionId.clear();
	type = 0;

	if (AData.size() < HeaderSize)
		return false;
	if ((quint8(AData.at(0)) & 0xC0) != 0)
		return false;
	if (qFromBigEndian<quint32>(AData.constData() + 4) != MagicCookie)
		return false;

	type = qFromBigEndian<quint16>(AData.constData());
	transactionId = AData.mid(8, 12);

	//Honour the declared length: peers may pad the datagram (libwebrtc does) and
	//the trailing bytes must not be walked as attributes.
	const quint16 declaredLength = qFromBigEndian<quint16>(AData.constData() + 2);
	const int end = qMin(AData.size(), HeaderSize + int(declaredLength));
	int offset = HeaderSize;
	while (offset + 4 <= end)
	{
		const quint16 attrType = qFromBigEndian<quint16>(AData.constData() + offset);
		const quint16 attrLength = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		if (offset + 4 + int(attrLength) > end)
			break;
		attributes.append(Attribute(attrType, AData.mid(offset + 4, attrLength)));
		offset += 4 + attrLength + (4 - attrLength % 4) % 4;
	}
	return true;
}

QByteArray Message::value(quint16 AType) const
{
	for (const Attribute &attr : attributes)
		if (attr.first == AType)
			return attr.second;
	return QByteArray();
}

QList<QByteArray> Message::values(quint16 AType) const
{
	QList<QByteArray> result;
	for (const Attribute &attr : attributes)
		if (attr.first == AType)
			result.append(attr.second);
	return result;
}

bool Message::has(quint16 AType) const
{
	for (const Attribute &attr : attributes)
		if (attr.first == AType)
			return true;
	return false;
}

int Message::errorCode() const
{
	const QByteArray code = value(AttributeErrorCode);
	if (code.size() < 4)
		return 0;
	return (quint8(code.at(2)) & 0x07) * 100 + quint8(code.at(3));
}

QString Message::errorReason() const
{
	const QByteArray code = value(AttributeErrorCode);
	if (code.size() <= 4)
		return QString();
	return QString::fromUtf8(code.mid(4));
}

QHostAddress Message::xorAddress(quint16 AType) const
{
	const QByteArray encoded = value(AType);
	if (encoded.size() < 8)
		return QHostAddress();
	const quint8 family = quint8(encoded.at(1));
	if (family != 0x01) //IPv4 only: the plugin works on IPv4 sockets
		return QHostAddress();
	const quint32 cookie = qFromBigEndian<quint32>(encoded.constData() + 4);
	return QHostAddress(cookie ^ MagicCookie);
}

quint16 Message::xorPort(quint16 AType) const
{
	const QByteArray encoded = value(AType);
	if (encoded.size() < 8)
		return 0;
	return qFromBigEndian<quint16>(encoded.constData() + 2) ^ quint16(MagicCookie >> 16);
}

QByteArray attribute(quint16 AType, const QByteArray &AValue)
{
	QByteArray result;
	const quint16 type = qToBigEndian<quint16>(AType);
	const quint16 length = qToBigEndian<quint16>(quint16(AValue.size()));
	result.append(reinterpret_cast<const char*>(&type), 2);
	result.append(reinterpret_cast<const char*>(&length), 2);
	result.append(AValue);
	while (result.size() % 4 != 0)
		result.append('\0');
	return result;
}

QByteArray attribute32(quint16 AType, quint32 AValue)
{
	const quint32 be = qToBigEndian<quint32>(AValue);
	return attribute(AType, QByteArray(reinterpret_cast<const char*>(&be), 4));
}

QByteArray xorAddressAttribute(quint16 AType, const QHostAddress &AAddress, quint16 APort)
{
	if (AAddress.protocol() != QAbstractSocket::IPv4Protocol)
		return QByteArray();
	QByteArray value;
	value.append(char(0x00)); //reserved
	value.append(char(0x01)); //family IPv4
	const quint16 port = qToBigEndian<quint16>(APort ^ quint16(MagicCookie >> 16));
	value.append(reinterpret_cast<const char*>(&port), 2);
	const quint32 address = qToBigEndian<quint32>(AAddress.toIPv4Address() ^ MagicCookie);
	value.append(reinterpret_cast<const char*>(&address), 4);
	return attribute(AType, value);
}

QByteArray buildMessage(quint16 AType, const QByteArray &ATransactionId,
	const QList<Attribute> &AAttributes, const QByteArray &AIntegrityKey)
{
	QByteArray header;
	const quint16 type = qToBigEndian<quint16>(AType);
	header.append(reinterpret_cast<const char*>(&type), 2);
	header.append(2, '\0'); //length, fixed up below
	const quint32 cookie = qToBigEndian<quint32>(MagicCookie);
	header.append(reinterpret_cast<const char*>(&cookie), 4);
	header.append(ATransactionId.leftJustified(12, '\0', true));

	QByteArray body;
	for (const Attribute &attr : AAttributes)
		body.append(attribute(attr.first, attr.second));

	if (!AIntegrityKey.isEmpty())
	{
		//RFC 5389 15.4: the length field must point just past MESSAGE-INTEGRITY
		QByteArray forIntegrity = header + body;
		const quint16 length = qToBigEndian<quint16>(quint16(body.size() + 24));
		forIntegrity.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&length), 2));
		const QByteArray hmac = QMessageAuthenticationCode::hash(forIntegrity, AIntegrityKey, QCryptographicHash::Sha1);
		body.append(attribute(AttributeMessageIntegrity, hmac));
	}

	//RFC 5389 15.5: FINGERPRINT is a CRC32 of the message with the length field
	//covering the fingerprint attribute itself
	QByteArray forCrc = header + body;
	const quint16 crcLength = qToBigEndian<quint16>(quint16(body.size() + 8));
	forCrc.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&crcLength), 2));
	const quint32 crc = qToBigEndian<quint32>(crc32(forCrc) ^ FingerprintXor);
	body.append(attribute(AttributeFingerprint, QByteArray(reinterpret_cast<const char*>(&crc), 4)));

	QByteArray packet = header + body;
	const quint16 finalLength = qToBigEndian<quint16>(quint16(body.size()));
	packet.replace(2, 2, QByteArray(reinterpret_cast<const char*>(&finalLength), 2));
	return packet;
}

QByteArray transactionId()
{
	QByteArray result;
	result.reserve(12);
	for (int i = 0; i < 12; ++i)
		result.append(char(quint8(QRandomGenerator::global()->bounded(256))));
	return result;
}

QByteArray longTermKey(const QString &AUsername, const QString &ARealm, const QString &APassword)
{
	return QCryptographicHash::hash((AUsername + ':' + ARealm + ':' + APassword).toUtf8(),
		QCryptographicHash::Md5);
}

bool looksLikeStun(const QByteArray &AData)
{
	//RFC 7983: the leading two bits 00 cover STUN; RTP/RTCP start with 10 and
	//DTLS with 00xxxxxx at 20..63, hence the cookie check as a second gate.
	return AData.size() >= HeaderSize && (quint8(AData.at(0)) & 0xC0) == 0 &&
		qFromBigEndian<quint32>(AData.constData() + 4) == MagicCookie;
}

}
