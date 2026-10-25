#ifndef OMEMOENVELOPE_H
#define OMEMOENVELOPE_H

#include <QByteArray>
#include <QDomElement>
#include <QList>
#include <QString>

class OmemoCrypto;

// Legacy OMEMO 0.3 (eu.siacs.conversations.axolotl) message envelope.
//
// The message body is encrypted with a random 16-byte AES-128-GCM key and a
// 12-byte IV.  The 16-byte key and the 16-byte GCM tag are concatenated and
// sent through the Signal session of every recipient device.  The XML looks
// like:
//
//   <encrypted xmlns="eu.siacs.conversations.axolotl">
//     <header sid="own-device-id">
//       <iv>base64</iv>
//       <key rid="device-id" prekey="true">base64(signal-ciphertext)</key>
//     </header>
//     <payload>base64(aes-gcm-ciphertext-without-tag)</payload>
//   </encrypted>
class OmemoEnvelope
{
public:
	struct EncryptedKey
	{
		quint32 deviceId = 0;
		QByteArray data;
		bool preKey = false;
	};

	struct Data
	{
		quint32 sid = 0;
		QByteArray iv;
		QByteArray payload;
		QList<EncryptedKey> keys;
	};

	struct Target
	{
		QString jid;
		quint32 deviceId = 0;
	};

	static const char *ns();

	// Encrypt APlain for every target device, using ACrypto to transport the
	// AES key+tag.  Every target must already have a Signal session.
	static bool encrypt(OmemoCrypto *ACrypto, const QList<Target> &ATargets,
		const QByteArray &APlain, Data &AData, QString *AError = nullptr);

	// Find the key for this device, decrypt it through Signal and then
	// decrypt the payload.
	static bool decrypt(OmemoCrypto *ACrypto, const Data &AData,
		const QString &ASenderJid, QByteArray &APlain, QString *AError = nullptr);

	static QDomElement toXml(QDomDocument &ADocument, const Data &AData);
	static bool fromXml(const QDomElement &AElement, Data &AData,
		QString *AError = nullptr);

	static QByteArray toXmlString(const Data &AData, QString *AError = nullptr);
	static bool fromXmlString(const QByteArray &AXml, Data &AData,
		QString *AError = nullptr);

private:
	static bool setError(const QString &AError, QString *AErrorOut);
	static bool aesGcmEncrypt(const QByteArray &AKey, const QByteArray &AIv,
		const QByteArray &APlain, QByteArray &ACipher, QByteArray &ATag,
		QString *AError);
	static bool aesGcmDecrypt(const QByteArray &AKey, const QByteArray &AIv,
		const QByteArray &ACipher, const QByteArray &ATag, QByteArray &APlain,
		QString *AError);
};

#endif // OMEMOENVELOPE_H
