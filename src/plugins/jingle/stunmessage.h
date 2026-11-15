#ifndef STUNMESSAGE_H
#define STUNMESSAGE_H

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QString>

/*
 * STUN codec shared by the ICE agent (server-reflexive gathering) and the TURN
 * client (RFC 5389 messages with long-term credentials, RFC 5766 allocations).
 *
 * The ICE checks themselves are built in iceagent.cpp because they have to
 * reproduce the quirks of the peers in the wild (RFC 3489 style answers,
 * MESSAGE-INTEGRITY covered length, role attributes). Everything that only
 * speaks RFC 5389/5766 lives here: the cookie is mandatory, the transaction id
 * is 12 bytes, MESSAGE-INTEGRITY is HMAC-SHA1 and FINGERPRINT is the CRC32
 * defined in RFC 5389 section 15.5.
 */
namespace Stun
{
enum MessageType
{
	BindingRequest          = 0x0001,
	BindingSuccess          = 0x0101,
	BindingError            = 0x0111,
	AllocateRequest         = 0x0003,
	AllocateSuccess         = 0x0103,
	AllocateError           = 0x0113,
	RefreshRequest          = 0x0004,
	RefreshSuccess          = 0x0104,
	RefreshError            = 0x0114,
	CreatePermissionRequest = 0x0008,
	CreatePermissionSuccess = 0x0108,
	CreatePermissionError   = 0x0118,
	SendIndication          = 0x0016,
	DataIndication          = 0x0017
};

enum AttributeType
{
	AttributeMappedAddress      = 0x0001,
	AttributeUsername           = 0x0006,
	AttributeMessageIntegrity   = 0x0008,
	AttributeErrorCode          = 0x0009,
	AttributeChannelNumber      = 0x000C,
	AttributeLifetime           = 0x000D,
	AttributeXorPeerAddress     = 0x0012,
	AttributeData               = 0x0013,
	AttributeRealm              = 0x0014,
	AttributeNonce              = 0x0015,
	AttributeXorRelayedAddress  = 0x0016,
	AttributeRequestedTransport = 0x0019,
	AttributeXorMappedAddress   = 0x0020,
	AttributeSoftware           = 0x8022,
	AttributeFingerprint        = 0x8028
};

typedef QPair<quint16, QByteArray> Attribute;

struct Message
{
	Message();

	quint16 type;
	QByteArray transactionId;
	QList<Attribute> attributes;

	QByteArray value(quint16 AType) const;      //First occurrence
	QList<QByteArray> values(quint16 AType) const;
	bool has(quint16 AType) const;
	int errorCode() const;                      //0 when the attribute is absent
	QString errorReason() const;

	bool parse(const QByteArray &AData);
	//XOR-MAPPED-ADDRESS, XOR-PEER-ADDRESS, XOR-RELAYED-ADDRESS
	QHostAddress xorAddress(quint16 AType) const;
	quint16 xorPort(quint16 AType) const;
};

//True when the datagram carries a STUN message (RFC 7983 leading bits 00).
bool looksLikeStun(const QByteArray &AData);

QByteArray buildMessage(quint16 AType, const QByteArray &ATransactionId,
	const QList<Attribute> &AAttributes, const QByteArray &AIntegrityKey = QByteArray());

QByteArray attribute(quint16 AType, const QByteArray &AValue);
QByteArray attribute32(quint16 AType, quint32 AValue);
QByteArray xorAddressAttribute(quint16 AType, const QHostAddress &AAddress, quint16 APort);
QByteArray transactionId();

//RFC 5389 15.4: the long-term credential key is MD5(username ":" realm ":" password)
QByteArray longTermKey(const QString &AUsername, const QString &ARealm, const QString &APassword);
}

#endif // STUNMESSAGE_H
