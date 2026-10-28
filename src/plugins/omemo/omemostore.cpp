#include "omemostore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstring>

namespace
{
	const char StoreMagic[] = "VOMEMO1";
	const int StoreMagicSize = 7;
	const int StoreSaltSize = 16;
	const int StoreNonceSize = 12;
	const int StoreTagSize = 16;
	const int StoreHeaderSize = StoreMagicSize + StoreSaltSize + StoreNonceSize + StoreTagSize;

	const char StoreInfo[] = "vacuum-im/omemo/store/v1";

	QString opensslError()
	{
		unsigned long code = ERR_get_error();
		if (code == 0)
			return QStringLiteral("unknown OpenSSL error");
		char buffer[256];
		ERR_error_string_n(code,buffer,sizeof(buffer));
		return QString::fromLatin1(buffer);
	}

	bool deriveKey(const QByteArray &AIkm, const QByteArray &ASalt, QByteArray &AKey, QString &AError)
	{
		AKey.clear();
		AError.clear();

		EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr);
		if (ctx == nullptr)
		{
			AError = opensslError();
			return false;
		}

		bool ok = EVP_PKEY_derive_init(ctx) > 0
			&& EVP_PKEY_CTX_set_hkdf_md(ctx,EVP_sha256()) > 0
			&& EVP_PKEY_CTX_set1_hkdf_salt(ctx,(const unsigned char *)ASalt.constData(),ASalt.size()) > 0
			&& EVP_PKEY_CTX_set1_hkdf_key(ctx,(const unsigned char *)AIkm.constData(),AIkm.size()) > 0
			&& EVP_PKEY_CTX_add1_hkdf_info(ctx,(const unsigned char *)StoreInfo,strlen(StoreInfo)) > 0;

		if (ok)
		{
			AKey.resize(32);
			size_t length = AKey.size();
			ok = EVP_PKEY_derive(ctx,(unsigned char *)AKey.data(),&length) > 0 && length == 32;
			if (!ok)
				AKey.clear();
		}

		if (!ok)
			AError = opensslError();

		EVP_PKEY_CTX_free(ctx);
		return ok;
	}

	bool encryptPayload(const QByteArray &AKey, const QByteArray &ANonce, const QByteArray &APlain,
		QByteArray &ACipher, QByteArray &ATag, QString &AError)
	{
		ACipher.clear();
		ATag.clear();
		AError.clear();

		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		if (ctx == nullptr)
		{
			AError = opensslError();
			return false;
		}

		bool ok = EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr) > 0
			&& EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,ANonce.size(),nullptr) > 0
			&& EVP_EncryptInit_ex(ctx,nullptr,nullptr,(const unsigned char *)AKey.constData(),
				(const unsigned char *)ANonce.constData()) > 0;

		int written = 0;
		if (ok)
		{
			ACipher.resize(APlain.size() + EVP_MAX_BLOCK_LENGTH);
			ok = EVP_EncryptUpdate(ctx,(unsigned char *)ACipher.data(),&written,
				(const unsigned char *)APlain.constData(),APlain.size()) > 0;
		}

		int finalWritten = 0;
		if (ok)
		{
			ok = EVP_EncryptFinal_ex(ctx,(unsigned char *)ACipher.data() + written,&finalWritten) > 0;
			if (ok)
				ACipher.resize(written + finalWritten);
		}

		if (ok)
		{
			ATag.resize(StoreTagSize);
			ok = EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,ATag.size(),ATag.data()) > 0;
		}

		if (!ok)
		{
			AError = opensslError();
			ACipher.clear();
			ATag.clear();
		}

		EVP_CIPHER_CTX_free(ctx);
		return ok;
	}

	bool decryptPayload(const QByteArray &AKey, const QByteArray &ANonce, const QByteArray &ACipher,
		const QByteArray &ATag, QByteArray &APlain, QString &AError)
	{
		APlain.clear();
		AError.clear();

		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		if (ctx == nullptr)
		{
			AError = opensslError();
			return false;
		}

		bool ok = EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr) > 0
			&& EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,ANonce.size(),nullptr) > 0
			&& EVP_DecryptInit_ex(ctx,nullptr,nullptr,(const unsigned char *)AKey.constData(),
				(const unsigned char *)ANonce.constData()) > 0;

		int written = 0;
		if (ok && !ACipher.isEmpty())
		{
			APlain.resize(ACipher.size() + EVP_MAX_BLOCK_LENGTH);
			ok = EVP_DecryptUpdate(ctx,(unsigned char *)APlain.data(),&written,
				(const unsigned char *)ACipher.constData(),ACipher.size()) > 0;
		}

		if (ok)
		{
			ok = EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,ATag.size(),
				const_cast<char *>(ATag.constData())) > 0;
		}

		int finalWritten = 0;
		if (ok)
		{
			ok = EVP_DecryptFinal_ex(ctx,(unsigned char *)APlain.data() + written,&finalWritten) > 0;
			if (ok)
				APlain.resize(written + finalWritten);
		}

		if (!ok)
		{
			AError = QStringLiteral("authentication failed");
			APlain.clear();
		}

		EVP_CIPHER_CTX_free(ctx);
		return ok;
	}

	QString encode(const QByteArray &AData)
	{
		return QString::fromLatin1(AData.toBase64());
	}

	QByteArray decode(const QString &AData)
	{
		return QByteArray::fromBase64(AData.toLatin1());
	}
}

OmemoStore::OmemoStore(const QString &AFilePath, const QByteArray &AKey) : FFilePath(AFilePath), FKey(AKey), FLoaded(false), FDirty(false)
{
}

bool OmemoStore::exists() const
{
	return QFile::exists(FFilePath);
}

void OmemoStore::reset()
{
	FData = Data();
	FLoaded = true;
	FDirty = false;
	FLastError.clear();
}

bool OmemoStore::load()
{
	FLastError.clear();

	if (!exists())
	{
		FData = Data();
		FLoaded = true;
		FDirty = false;
		return true;
	}

	if (FKey.isEmpty())
	{
		FLastError = QStringLiteral("profile key is empty");
		FLoaded = false;
		return false;
	}

	QFile file(FFilePath);
	if (!file.open(QFile::ReadOnly))
	{
		FLastError = QStringLiteral("cannot open %1: %2").arg(FFilePath,file.errorString());
		FLoaded = false;
		return false;
	}

	QByteArray blob = file.readAll();
	file.close();

	if (blob.size() < StoreHeaderSize || memcmp(blob.constData(),StoreMagic,StoreMagicSize) != 0)
	{
		FLastError = QStringLiteral("invalid OMEMO store header");
		FLoaded = false;
		return false;
	}

	const QByteArray salt = blob.mid(StoreMagicSize,StoreSaltSize);
	const QByteArray nonce = blob.mid(StoreMagicSize + StoreSaltSize,StoreNonceSize);
	const QByteArray tag = blob.mid(StoreMagicSize + StoreSaltSize + StoreNonceSize,StoreTagSize);
	const QByteArray cipher = blob.mid(StoreHeaderSize);

	QByteArray key;
	QString error;
	if (!deriveKey(FKey,salt,key,error))
	{
		FLastError = QStringLiteral("cannot derive storage key: %1").arg(error);
		FLoaded = false;
		return false;
	}

	QByteArray payload;
	if (!decryptPayload(key,nonce,cipher,tag,payload,error))
	{
		FLastError = QStringLiteral("cannot decrypt OMEMO store: %1").arg(error);
		FLoaded = false;
		return false;
	}

	if (!deserialize(payload))
	{
		FLoaded = false;
		return false;
	}

	FLoaded = true;
	FDirty = false;
	return true;
}

bool OmemoStore::save()
{
	FLastError.clear();

	if (FKey.isEmpty())
	{
		FLastError = QStringLiteral("profile key is empty");
		return false;
	}

	QByteArray salt(StoreSaltSize,0);
	QByteArray nonce(StoreNonceSize,0);
	if (RAND_bytes((unsigned char *)salt.data(),salt.size()) != 1
		|| RAND_bytes((unsigned char *)nonce.data(),nonce.size()) != 1)
	{
		FLastError = QStringLiteral("cannot generate random data: %1").arg(opensslError());
		return false;
	}

	QByteArray key;
	QString error;
	if (!deriveKey(FKey,salt,key,error))
	{
		FLastError = QStringLiteral("cannot derive storage key: %1").arg(error);
		return false;
	}

	QByteArray cipher;
	QByteArray tag;
	if (!encryptPayload(key,nonce,serialize(),cipher,tag,error))
	{
		FLastError = QStringLiteral("cannot encrypt OMEMO store: %1").arg(error);
		return false;
	}

	QDir dir = QFileInfo(FFilePath).dir();
	if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
	{
		FLastError = QStringLiteral("cannot create directory %1").arg(dir.absolutePath());
		return false;
	}

	QSaveFile file(FFilePath);
	file.setDirectWriteFallback(false);
	if (!file.open(QFile::WriteOnly))
	{
		FLastError = QStringLiteral("cannot open %1: %2").arg(FFilePath,file.errorString());
		return false;
	}

	file.write(StoreMagic,StoreMagicSize);
	file.write(salt);
	file.write(nonce);
	file.write(tag);
	file.write(cipher);

	if (!file.commit())
	{
		FLastError = QStringLiteral("cannot save %1: %2").arg(FFilePath,file.errorString());
		return false;
	}

	QFile::setPermissions(FFilePath,QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	FDirty = false;
	return true;
}

QByteArray OmemoStore::serialize() const
{
	QJsonObject root;
	root.insert(QStringLiteral("version"),2);

	QJsonObject device;
	device.insert(QStringLiteral("id"),(double)FData.deviceId);
	device.insert(QStringLiteral("label"),FData.deviceLabel);
	device.insert(QStringLiteral("registrationId"),(double)FData.registrationId);
	device.insert(QStringLiteral("identityKeyPair"),encode(FData.identityKeyPair));
	device.insert(QStringLiteral("identityPublic"),encode(FData.identityPublic));
	device.insert(QStringLiteral("latestSignedPreKeyId"),(double)FData.latestSignedPreKeyId);
	device.insert(QStringLiteral("latestPreKeyId"),(double)FData.latestPreKeyId);
	root.insert(QStringLiteral("device"),device);

	QJsonArray signedPreKeys;
	for (QHash<quint32, SignedPreKey>::const_iterator it=FData.signedPreKeys.constBegin();
		it!=FData.signedPreKeys.constEnd(); ++it)
	{
		QJsonObject object;
		object.insert(QStringLiteral("id"),(double)it.key());
		object.insert(QStringLiteral("created"),(double)it->created);
		object.insert(QStringLiteral("data"),encode(it->data));
		signedPreKeys.append(object);
	}
	root.insert(QStringLiteral("signedPreKeys"),signedPreKeys);

	QJsonArray preKeys;
	for (QHash<quint32, QByteArray>::const_iterator it=FData.preKeys.constBegin();
		it!=FData.preKeys.constEnd(); ++it)
	{
		QJsonObject object;
		object.insert(QStringLiteral("id"),(double)it.key());
		object.insert(QStringLiteral("data"),encode(it.value()));
		preKeys.append(object);
	}
	root.insert(QStringLiteral("preKeys"),preKeys);

	QJsonObject devices;
	for (QHash<QString, QHash<quint32, Device> >::const_iterator jit=FData.devices.constBegin();
		jit!=FData.devices.constEnd(); ++jit)
	{
		QJsonArray list;
		for (QHash<quint32, Device>::const_iterator dit=jit->constBegin(); dit!=jit->constEnd(); ++dit)
		{
			QJsonObject object;
			object.insert(QStringLiteral("id"),(double)dit.key());
			object.insert(QStringLiteral("keyId"),encode(dit->keyId));
			object.insert(QStringLiteral("session"),encode(dit->session));
			object.insert(QStringLiteral("trust"),dit->trustLevel);
			object.insert(QStringLiteral("unrespondedSent"),dit->unrespondedSent);
			object.insert(QStringLiteral("unrespondedReceived"),dit->unrespondedReceived);
			object.insert(QStringLiteral("removedAt"),(double)dit->removedAt);
			list.append(object);
		}
		devices.insert(jit.key(),list);
	}
	root.insert(QStringLiteral("devices"),devices);

	QJsonArray disabledChats;
	for (const QString &jid : FData.disabledChats)
		disabledChats.append(jid);
	root.insert(QStringLiteral("disabledChats"),disabledChats);

	QJsonObject messageCache;
	for (QHash<QString, QByteArray>::const_iterator it=FData.messageCache.constBegin();
		it!=FData.messageCache.constEnd(); ++it)
		messageCache.insert(it.key(),encode(it.value()));
	root.insert(QStringLiteral("messageCache"),messageCache);

	// Introduced in version 2. Attachment keys and thumbnails are as secret as
	// the message plaintext, so they live inside the same encrypted blob.
	QJsonObject attachments;
	for (QHash<QString, AttachmentKeys>::const_iterator it=FData.attachments.constBegin();
		it!=FData.attachments.constEnd(); ++it)
	{
		QJsonObject object;
		object.insert(QStringLiteral("iv"),encode(it.value().iv));
		object.insert(QStringLiteral("key"),encode(it.value().key));
		object.insert(QStringLiteral("added"),(double)it.value().added);
		attachments.insert(it.key(),object);
	}
	root.insert(QStringLiteral("attachments"),attachments);

	QJsonObject attachmentThumbs;
	for (QHash<QString, QString>::const_iterator it=FData.attachmentThumbs.constBegin();
		it!=FData.attachmentThumbs.constEnd(); ++it)
		attachmentThumbs.insert(it.key(),it.value());
	root.insert(QStringLiteral("attachmentThumbs"),attachmentThumbs);

	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool OmemoStore::deserialize(const QByteArray &APayload)
{
	QJsonParseError parseError;
	QJsonDocument document = QJsonDocument::fromJson(APayload,&parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
	{
		FLastError = QStringLiteral("cannot parse OMEMO store: %1").arg(parseError.errorString());
		return false;
	}

	QJsonObject root = document.object();
	// Version 1: the original format.  Version 2 adds the XEP-0454 attachment
	// key material; older files simply load without it.
	const int version = root.value(QStringLiteral("version")).toInt();
	if (version != 1 && version != 2)
	{
		FLastError = QStringLiteral("unsupported OMEMO store version");
		return false;
	}

	Data data;
	QJsonObject device = root.value(QStringLiteral("device")).toObject();
	data.deviceId = (quint32)device.value(QStringLiteral("id")).toDouble();
	data.deviceLabel = device.value(QStringLiteral("label")).toString();
	data.registrationId = (quint32)device.value(QStringLiteral("registrationId")).toDouble();
	data.identityKeyPair = decode(device.value(QStringLiteral("identityKeyPair")).toString());
	data.identityPublic = decode(device.value(QStringLiteral("identityPublic")).toString());
	data.latestSignedPreKeyId = (quint32)device.value(QStringLiteral("latestSignedPreKeyId")).toDouble();
	data.latestPreKeyId = (quint32)device.value(QStringLiteral("latestPreKeyId")).toDouble();

	const QJsonArray signedArray = root.value(QStringLiteral("signedPreKeys")).toArray();
	for (const QJsonValue &value : signedArray)
	{
		QJsonObject object = value.toObject();
		quint32 id = (quint32)object.value(QStringLiteral("id")).toDouble();
		if (id == 0)
			continue;
		SignedPreKey key;
		key.created = (qint64)object.value(QStringLiteral("created")).toDouble();
		key.data = decode(object.value(QStringLiteral("data")).toString());
		data.signedPreKeys.insert(id,key);
	}

	const QJsonArray preKeyArray = root.value(QStringLiteral("preKeys")).toArray();
	for (const QJsonValue &value : preKeyArray)
	{
		QJsonObject object = value.toObject();
		quint32 id = (quint32)object.value(QStringLiteral("id")).toDouble();
		if (id != 0)
			data.preKeys.insert(id,decode(object.value(QStringLiteral("data")).toString()));
	}

	QJsonObject devices = root.value(QStringLiteral("devices")).toObject();
	for (QJsonObject::const_iterator jit=devices.constBegin(); jit!=devices.constEnd(); ++jit)
	{
		QHash<quint32, Device> list;
		const QJsonArray deviceArray = jit.value().toArray();
		for (const QJsonValue &value : deviceArray)
		{
			QJsonObject object = value.toObject();
			quint32 id = (quint32)object.value(QStringLiteral("id")).toDouble();
			if (id == 0)
				continue;
			Device deviceData;
			deviceData.keyId = decode(object.value(QStringLiteral("keyId")).toString());
			deviceData.session = decode(object.value(QStringLiteral("session")).toString());
			deviceData.trustLevel = object.value(QStringLiteral("trust")).toInt();
			deviceData.unrespondedSent = object.value(QStringLiteral("unrespondedSent")).toInt();
			deviceData.unrespondedReceived = object.value(QStringLiteral("unrespondedReceived")).toInt();
			deviceData.removedAt = (qint64)object.value(QStringLiteral("removedAt")).toDouble();
			list.insert(id,deviceData);
		}
		data.devices.insert(jit.key(),list);
	}

	const QJsonArray disabledArray = root.value(QStringLiteral("disabledChats")).toArray();
	for (const QJsonValue &value : disabledArray)
	{
		const QString jid = value.toString();
		if (!jid.isEmpty())
			data.disabledChats.insert(jid);
	}

	const QJsonObject messageCache = root.value(QStringLiteral("messageCache")).toObject();
	for (QJsonObject::const_iterator it=messageCache.constBegin(); it!=messageCache.constEnd(); ++it)
	{
		const QByteArray plain = decode(it.value().toString());
		if (!it.key().isEmpty() && !plain.isEmpty())
			data.messageCache.insert(it.key(),plain);
	}

	if (version >= 2)
	{
		const QJsonObject attachments = root.value(QStringLiteral("attachments")).toObject();
		for (QJsonObject::const_iterator it=attachments.constBegin(); it!=attachments.constEnd(); ++it)
		{
			const QJsonObject object = it.value().toObject();
			AttachmentKeys keys;
			keys.iv = decode(object.value(QStringLiteral("iv")).toString());
			keys.key = decode(object.value(QStringLiteral("key")).toString());
			keys.added = (qint64)object.value(QStringLiteral("added")).toDouble();
			if (!it.key().isEmpty() && !keys.key.isEmpty())
				data.attachments.insert(it.key(),keys);
		}

		const QJsonObject attachmentThumbs = root.value(QStringLiteral("attachmentThumbs")).toObject();
		for (QJsonObject::const_iterator it=attachmentThumbs.constBegin(); it!=attachmentThumbs.constEnd(); ++it)
		{
			if (!it.key().isEmpty())
				data.attachmentThumbs.insert(it.key(),it.value().toString());
		}
	}

	FData = data;
	return true;
}
