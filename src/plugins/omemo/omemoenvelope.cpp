#include "omemoenvelope.h"

#include "omemocrypto.h"

#include <QDomDocument>
#include <QDomNode>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace
{
	const int OmemoAesKeySize = 16;
	const int OmemoGcmIvSize = 12;
	const int OmemoGcmTagSize = 16;
	const int OmemoKeyTagSize = OmemoAesKeySize + OmemoGcmTagSize;
	const char *OmemoNamespace = "eu.siacs.conversations.axolotl";
}

const char *OmemoEnvelope::ns()
{
	return OmemoNamespace;
}

bool OmemoEnvelope::setError(const QString &AError, QString *AErrorOut)
{
	if (AErrorOut != nullptr)
		*AErrorOut = AError;
	return false;
}

bool OmemoEnvelope::aesGcmEncrypt(const QByteArray &AKey, const QByteArray &AIv,
	const QByteArray &APlain, QByteArray &ACipher, QByteArray &ATag, QString *AError)
{
	if (AKey.size() != OmemoAesKeySize || AIv.size() != OmemoGcmIvSize)
		return setError(QStringLiteral("invalid AES key or IV size"),AError);

	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	if (context == nullptr)
		return setError(QStringLiteral("cannot allocate AES context"),AError);

	bool ok = EVP_EncryptInit_ex(context,EVP_aes_128_gcm(),nullptr,nullptr,nullptr) == 1
		&& EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_SET_IVLEN,AIv.size(),nullptr) == 1
		&& EVP_EncryptInit_ex(context,nullptr,nullptr,
			(const unsigned char *)AKey.constData(),(const unsigned char *)AIv.constData()) == 1;

	QByteArray output(APlain.size() + OmemoGcmTagSize,0);
	int written = 0;
	int finalWritten = 0;
	if (ok)
		ok = EVP_EncryptUpdate(context,(unsigned char *)output.data(),&written,
			(const unsigned char *)APlain.constData(),APlain.size()) == 1;
	if (ok)
		ok = EVP_EncryptFinal_ex(context,(unsigned char *)output.data() + written,&finalWritten) == 1;

	QByteArray tag(OmemoGcmTagSize,0);
	if (ok)
		ok = EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_GET_TAG,tag.size(),tag.data()) == 1;

	EVP_CIPHER_CTX_free(context);

	if (!ok)
		return setError(QStringLiteral("cannot encrypt OMEMO payload"),AError);

	output.resize(written + finalWritten);
	ACipher = output;
	ATag = tag;
	return true;
}

bool OmemoEnvelope::aesGcmDecrypt(const QByteArray &AKey, const QByteArray &AIv,
	const QByteArray &ACipher, const QByteArray &ATag, QByteArray &APlain, QString *AError)
{
	if (AKey.size() != OmemoAesKeySize || AIv.size() != OmemoGcmIvSize
		|| ATag.size() != OmemoGcmTagSize)
		return setError(QStringLiteral("invalid AES key, IV or tag size"),AError);

	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	if (context == nullptr)
		return setError(QStringLiteral("cannot allocate AES context"),AError);

	bool ok = EVP_DecryptInit_ex(context,EVP_aes_128_gcm(),nullptr,nullptr,nullptr) == 1
		&& EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_SET_IVLEN,AIv.size(),nullptr) == 1
		&& EVP_DecryptInit_ex(context,nullptr,nullptr,
			(const unsigned char *)AKey.constData(),(const unsigned char *)AIv.constData()) == 1;

	QByteArray output(ACipher.size(),0);
	int written = 0;
	int finalWritten = 0;
	if (ok)
		ok = EVP_DecryptUpdate(context,(unsigned char *)output.data(),&written,
			(const unsigned char *)ACipher.constData(),ACipher.size()) == 1;
	if (ok)
		ok = EVP_CIPHER_CTX_ctrl(context,EVP_CTRL_GCM_SET_TAG,ATag.size(),
			const_cast<char *>(ATag.constData())) == 1;
	if (ok)
		ok = EVP_DecryptFinal_ex(context,(unsigned char *)output.data() + written,&finalWritten) == 1;

	EVP_CIPHER_CTX_free(context);

	if (!ok)
		return setError(QStringLiteral("cannot decrypt OMEMO payload"),AError);

	output.resize(written + finalWritten);
	APlain = output;
	return true;
}

bool OmemoEnvelope::encrypt(OmemoCrypto *ACrypto, const QList<Target> &ATargets,
	const QByteArray &APlain, Data &AData, QString *AError)
{
	if (ACrypto == nullptr)
		return setError(QStringLiteral("OMEMO crypto is not available"),AError);
	if (ATargets.isEmpty())
		return setError(QStringLiteral("no OMEMO devices to encrypt to"),AError);
	if (ACrypto->deviceId() == 0)
		return setError(QStringLiteral("OMEMO device id is not initialized"),AError);

	QByteArray key(OmemoAesKeySize,0);
	QByteArray iv(OmemoGcmIvSize,0);
	if (RAND_bytes((unsigned char *)key.data(),key.size()) != 1
		|| RAND_bytes((unsigned char *)iv.data(),iv.size()) != 1)
		return setError(QStringLiteral("cannot generate OMEMO payload key"),AError);

	QByteArray cipher;
	QByteArray tag;
	if (!aesGcmEncrypt(key,iv,APlain,cipher,tag,AError))
		return false;

	const QByteArray keyTag = key + tag;
	QList<EncryptedKey> keys;
	for (const Target &target : ATargets)
	{
		QByteArray signalCipher;
		bool preKey = false;
		QString error;
		if (!ACrypto->encrypt(target.jid,target.deviceId,keyTag,signalCipher,preKey,&error))
			return setError(QStringLiteral("cannot encrypt OMEMO key for %1/%2: %3")
				.arg(target.jid).arg(target.deviceId).arg(error),AError);

		EncryptedKey keyEntry;
		keyEntry.deviceId = target.deviceId;
		keyEntry.data = signalCipher;
		keyEntry.preKey = preKey;
		keys.append(keyEntry);
	}

	AData.sid = ACrypto->deviceId();
	AData.iv = iv;
	AData.payload = cipher;
	AData.keys = keys;
	return true;
}

bool OmemoEnvelope::decrypt(OmemoCrypto *ACrypto, const Data &AData,
	const QString &ASenderJid, QByteArray &APlain, QString *AError)
{
	APlain.clear();
	if (ACrypto == nullptr)
		return setError(QStringLiteral("OMEMO crypto is not available"),AError);
	if (ACrypto->deviceId() == 0)
		return setError(QStringLiteral("OMEMO device id is not initialized"),AError);
	if (AData.iv.size() != OmemoGcmIvSize)
		return setError(QStringLiteral("invalid OMEMO IV"),AError);

	QByteArray keyTag;
	bool decryptedKey = false;
	QString lastError;
	for (const EncryptedKey &key : AData.keys)
	{
		if (key.deviceId != ACrypto->deviceId())
			continue;

		QString error;
		bool ok = false;
		if (key.preKey)
		{
			ok = ACrypto->decryptPreKey(ASenderJid,AData.sid,key.data,keyTag,&error);
			if (!ok)
			{
				lastError = error;
				ok = ACrypto->decryptSignal(ASenderJid,AData.sid,key.data,keyTag,&error);
			}
		}
		else
		{
			ok = ACrypto->decryptSignal(ASenderJid,AData.sid,key.data,keyTag,&error);
			if (!ok)
			{
				lastError = error;
				ok = ACrypto->decryptPreKey(ASenderJid,AData.sid,key.data,keyTag,&error);
			}
		}

		if (ok)
		{
			decryptedKey = true;
			break;
		}
		lastError = error;
	}

	if (!decryptedKey)
	{
		if (lastError.isEmpty())
			return setError(QStringLiteral("OMEMO message is not encrypted to this device"),AError);
		return setError(QStringLiteral("cannot decrypt OMEMO key: %1").arg(lastError),AError);
	}

	if (keyTag.size() < OmemoKeyTagSize)
		return setError(QStringLiteral("decrypted OMEMO key is too short"),AError);

	const QByteArray key = keyTag.left(OmemoAesKeySize);
	const QByteArray tag = keyTag.mid(OmemoAesKeySize,OmemoGcmTagSize);

	// Some ratchet-forwarded messages contain no payload; the key update is
	// still important for the next message.
	if (AData.payload.isEmpty())
		return true;

	return aesGcmDecrypt(key,AData.iv,AData.payload,tag,APlain,AError);
}

QDomElement OmemoEnvelope::toXml(QDomDocument &ADocument, const Data &AData)
{
	QDomElement encrypted = ADocument.createElement(QStringLiteral("encrypted"));
	encrypted.setAttribute(QStringLiteral("xmlns"),QString::fromLatin1(OmemoNamespace));

	QDomElement header = ADocument.createElement(QStringLiteral("header"));
	header.setAttribute(QStringLiteral("sid"),QString::number(AData.sid));

	QDomElement iv = ADocument.createElement(QStringLiteral("iv"));
	iv.appendChild(ADocument.createTextNode(QString::fromLatin1(AData.iv.toBase64())));
	header.appendChild(iv);

	for (const EncryptedKey &key : AData.keys)
	{
		QDomElement keyElement = ADocument.createElement(QStringLiteral("key"));
		keyElement.setAttribute(QStringLiteral("rid"),QString::number(key.deviceId));
		if (key.preKey)
			keyElement.setAttribute(QStringLiteral("prekey"),QStringLiteral("true"));
		keyElement.appendChild(ADocument.createTextNode(QString::fromLatin1(key.data.toBase64())));
		header.appendChild(keyElement);
	}

	encrypted.appendChild(header);

	if (!AData.payload.isEmpty())
	{
		QDomElement payload = ADocument.createElement(QStringLiteral("payload"));
		payload.appendChild(ADocument.createTextNode(QString::fromLatin1(AData.payload.toBase64())));
		encrypted.appendChild(payload);
	}

	return encrypted;
}

bool OmemoEnvelope::fromXml(const QDomElement &AElement, Data &AData, QString *AError)
{
	if (AElement.tagName() != QStringLiteral("encrypted")
		&& AElement.localName() != QStringLiteral("encrypted"))
		return setError(QStringLiteral("not an OMEMO encrypted element"),AError);

	QString namespaceUri = AElement.attribute(QStringLiteral("xmlns"));
	if (namespaceUri.isEmpty())
		namespaceUri = AElement.namespaceURI();
	if (namespaceUri != QString::fromLatin1(OmemoNamespace))
		return setError(QStringLiteral("unsupported OMEMO namespace"),AError);

	QDomElement header;
	for (QDomNode node=AElement.firstChild(); !node.isNull(); node=node.nextSibling())
	{
		if (node.isElement() && node.toElement().tagName() == QStringLiteral("header"))
		{
			header = node.toElement();
			break;
		}
	}
	if (header.isNull())
		return setError(QStringLiteral("OMEMO header is missing"),AError);

	bool ok = false;
	Data data;
	data.sid = header.attribute(QStringLiteral("sid")).toUInt(&ok);
	if (!ok || data.sid == 0)
		return setError(QStringLiteral("invalid OMEMO sender device id"),AError);

	for (QDomNode node=header.firstChild(); !node.isNull(); node=node.nextSibling())
	{
		if (!node.isElement())
			continue;
		QDomElement element = node.toElement();

		if (element.tagName() == QStringLiteral("iv"))
		{
			data.iv = QByteArray::fromBase64(element.text().trimmed().toLatin1());
			continue;
		}

		if (element.tagName() == QStringLiteral("key"))
		{
			bool idOk = false;
			quint32 deviceId = element.attribute(QStringLiteral("rid")).toUInt(&idOk);
			if (!idOk || deviceId == 0)
				return setError(QStringLiteral("invalid OMEMO key device id"),AError);

			QByteArray keyData = QByteArray::fromBase64(element.text().trimmed().toLatin1());
			if (keyData.isEmpty())
				return setError(QStringLiteral("empty OMEMO key"),AError);

			EncryptedKey key;
			key.deviceId = deviceId;
			key.data = keyData;
			QString preKey = element.attribute(QStringLiteral("prekey"));
			key.preKey = preKey == QStringLiteral("true") || preKey == QStringLiteral("1");
			data.keys.append(key);
		}
	}

	if (data.iv.size() != OmemoGcmIvSize)
		return setError(QStringLiteral("OMEMO IV is missing or invalid"),AError);
	if (data.keys.isEmpty())
		return setError(QStringLiteral("OMEMO message contains no keys"),AError);

	for (QDomNode node=AElement.firstChild(); !node.isNull(); node=node.nextSibling())
	{
		if (node.isElement() && node.toElement().tagName() == QStringLiteral("payload"))
		{
			data.payload = QByteArray::fromBase64(node.toElement().text().trimmed().toLatin1());
			break;
		}
	}

	AData = data;
	return true;
}

QByteArray OmemoEnvelope::toXmlString(const Data &AData, QString *AError)
{
	QDomDocument document;
	document.appendChild(toXml(document,AData));
	return document.toByteArray(0);
}

bool OmemoEnvelope::fromXmlString(const QByteArray &AXml, Data &AData, QString *AError)
{
	QDomDocument document;
	const QDomDocument::ParseResult result = document.setContent(AXml);
	if (!result)
		return setError(QStringLiteral("cannot parse OMEMO XML at %1:%2: %3")
			.arg(result.errorLine).arg(result.errorColumn).arg(result.errorMessage),AError);
	return fromXml(document.documentElement(),AData,AError);
}
