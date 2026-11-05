#include "omemoattachments.h"

#include "omemostore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>

#include <QPointer>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <utils/logger.h>
#include <utils/options.h>

// The plan keeps the received attachments simple: url -> key material in the
// encrypted store, bytes in the file cache.  The key fragment and the
// thumbnail must never survive into the plaintext message archive.
namespace
{
	// XEP-0454: aesgcm://<url>#<IV hex (24 chars)><key hex (64 chars)>
	const int AesgcmSchemeSize = 9;
	const int IvHexSize = 24;
	const int KeyHexSize = 64;

	// ingests the whole url token up to the whitespace boundaries
	const QRegularExpression AesgcmRegex(QStringLiteral(R"REGEX(aesgcm://[^\s#<>"]+#[0-9a-fA-F]{88})REGEX"));

	QString opensslError()
	{
		unsigned long code = ERR_get_error();
		if (code == 0)
			return QStringLiteral("unknown OpenSSL error");
		char buffer[256];
		ERR_error_string_n(code,buffer,sizeof(buffer));
		return QString::fromLatin1(buffer);
	}

	// XEP-0454 file format: AES-256-GCM(key, iv) with the 16 byte tag appended
	// to the ciphertext.  No additional authenticated data.
	bool decryptAttachment(const QByteArray &AKey, const QByteArray &AIv,
		const QByteArray &AData, QByteArray &APlain, QString &AError)
	{
		APlain.clear();
		AError.clear();
		if (AKey.size() != 32 || AIv.isEmpty() || AData.size() <= 16)
		{
			AError = QStringLiteral("invalid aesgcm key or data size");
			return false;
		}

		const QByteArray cipher = AData.left(AData.size() - 16);
		const QByteArray tag = AData.right(16);

		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		if (ctx == nullptr)
		{
			AError = opensslError();
			return false;
		}

		bool ok = EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr) > 0
			&& EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,AIv.size(),nullptr) > 0
			&& EVP_DecryptInit_ex(ctx,nullptr,nullptr,(const unsigned char *)AKey.constData(),
				(const unsigned char *)AIv.constData()) > 0;

		int written = 0;
		APlain.resize(cipher.size() + 16);
		if (ok && !cipher.isEmpty())
			ok = EVP_DecryptUpdate(ctx,(unsigned char *)APlain.data(),&written,
				(const unsigned char *)cipher.constData(),cipher.size()) > 0;

		if (ok)
			ok = EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,tag.size(),
				const_cast<char *>(tag.constData())) > 0;

		int finalWritten = 0;
		if (ok)
		{
			ok = EVP_DecryptFinal_ex(ctx,(unsigned char *)APlain.data() + written,&finalWritten) > 0;
			if (ok)
				APlain.resize(written + finalWritten);
		}

		if (!ok)
		{
			AError = QStringLiteral("aesgcm authentication failed: %1").arg(opensslError());
			APlain.clear();
		}

		EVP_CIPHER_CTX_free(ctx);
		return ok;
	}
} // namespace

OmemoAttachments::OmemoAttachments(QObject *AParent) :
	QObject(AParent),
	FNetwork(new OmemoNetworkManager(this))
{
}

QString OmemoAttachments::sanitizeBody(OmemoAccount *AAccount, const QString &ABody, bool AAutoDownload)
{
	if (AAccount == nullptr || !ABody.contains(QStringLiteral("aesgcm://")))
		return ABody;

	QString body = ABody;
	QString thumbnail;
	bool thumbnailRemoved = false;

	// The thumbnail is the data: URI line that follows the url line separated
	// by a single new line (XEP-0454).  Everything else in the body stays
	// untouched.  The thumbnail line is lifted out and stored in the encrypted
	// store; it is not rendered (yet) and never reaches the plaintext archive.
	QStringList lines = ABody.split(QChar::LineFeed);
	for (int i=0; i<lines.count(); ++i)
	{
		if (lines.at(i).startsWith(QStringLiteral("data:"),Qt::CaseInsensitive)
			&& i > 0 && AesgcmRegex.match(lines.at(i-1)).hasMatch())
		{
			if (thumbnail.isEmpty())
			{
				thumbnail = lines.at(i);
				lines.removeAt(i);
				--i;
				thumbnailRemoved = true;
			}
		}
	}
	if (thumbnailRemoved)
		body = lines.join(QChar::LineFeed);

	bool changed = false;
	QRegularExpressionMatchIterator it = AesgcmRegex.globalMatch(body);
	while (it.hasNext())
	{
		const QRegularExpressionMatch match = it.next();
		const QString token = match.captured();
		const int hashPos = token.indexOf(QLatin1Char('#'));
		const QString urlPart = token.mid(AesgcmSchemeSize,hashPos - AesgcmSchemeSize);
		const QString fragment = token.mid(hashPos + 1);
		const QByteArray iv = QByteArray::fromHex(fragment.left(IvHexSize).toLatin1());
		const QByteArray key = QByteArray::fromHex(fragment.mid(IvHexSize).toLatin1());

		// The anchor href of the sanitized message is QUrl::toEncoded of the
		// same https url, so the store keys are normalized exactly that way
		// and both sides of the display comparisons always agree.
		const QString cleanUrl = QString::fromUtf8(QUrl(QStringLiteral("https://") + urlPart).toEncoded());
		if (QUrl(cleanUrl).host().isEmpty())
		{
			// Not a usable attachment: still strip the keys from the body.
			body.replace(token,QStringLiteral("https://") + urlPart);
			changed = true;
			continue;
		}

		if (!isAttachment(AAccount,cleanUrl))
		{
			OmemoStore::AttachmentKeys keys;
			keys.iv = iv;
			keys.key = key;
			keys.added = QDateTime::currentMSecsSinceEpoch();
			AAccount->store()->data().attachments.insert(cleanUrl,keys);
			pruneAttachments(AAccount);
			AAccount->store()->save();
		}
		if (!thumbnail.isEmpty() && !AAccount->store()->data().attachmentThumbs.contains(cleanUrl))
		{
			AAccount->store()->data().attachmentThumbs.insert(cleanUrl,thumbnail);
			AAccount->store()->save();
		}
		if (AAutoDownload)
			downloadAttachment(AAccount,cleanUrl);

		body.replace(token,QStringLiteral("https://") + urlPart);
		changed = true;
	}

	if (!changed)
		return ABody;

	LOG_DEBUG(QString("OMEMO attachment body sanitized, size=%1").arg(body.size()));
	return body;
}

bool OmemoAttachments::isAttachment(OmemoAccount *AAccount, const QString &ACleanUrl) const
{
	return AAccount != nullptr && AAccount->store() != nullptr
		&& AAccount->store()->data().attachments.contains(ACleanUrl);
}

QString OmemoAttachments::attachmentThumbnail(OmemoAccount *AAccount, const QString &ACleanUrl) const
{
	return AAccount != nullptr && AAccount->store() != nullptr
		? AAccount->store()->data().attachmentThumbs.value(ACleanUrl) : QString();
}

QString OmemoAttachments::attachmentFilePath(OmemoAccount *AAccount, const QString &ACleanUrl)
{
	const QString hash = QCryptographicHash::hash(ACleanUrl.toUtf8(),QCryptographicHash::Sha256).toHex();
	QString ext = QFileInfo(QUrl(ACleanUrl).path()).suffix().toLower();
	if (ext.isEmpty() || !QImageReader::supportedImageFormats().contains(ext.toUtf8()))
		ext = QStringLiteral("bin");
	return Options::filesPath() + QStringLiteral("/omemo/files/") + AAccount->accountId().toString(QUuid::WithoutBraces)
		+ QLatin1Char('/') + hash + QLatin1Char('.') + ext;
}

bool OmemoAttachments::isFileCached(OmemoAccount *AAccount, const QString &ACleanUrl)
{
	return QFile::exists(attachmentFilePath(AAccount,ACleanUrl));
}

QString OmemoAttachments::attachmentFileName(const QString &ACleanUrl)
{
	// QUrl::fileName returns the decoded file name.
	QString name = QUrl(ACleanUrl).fileName();
	if (name.isEmpty())
		name = QObject::tr("attachment");
	return name;
}

bool OmemoAttachments::isImageFilePath(const QString &ALocalFile)
{
	const QString ext = QFileInfo(ALocalFile).suffix().toLower();
	return !ext.isEmpty() && ext != QStringLiteral("bin")
		&& QImageReader::supportedImageFormats().contains(ext.toUtf8());
}

QString OmemoAttachments::formatFileSize(qint64 ABytes)
{
	if (ABytes < 1024)
		return QObject::tr("%n byte(s)",nullptr,(int)ABytes);
	if (ABytes < 1024*1024)
		return QObject::tr("%1 KB").arg(QString::number(ABytes/1024.0,'f',1));
	if (ABytes < 1024*qint64(1024)*1024)
		return QObject::tr("%1 MB").arg(QString::number(ABytes/(1024.0*1024),'f',1));
	return QObject::tr("%1 GB").arg(QString::number(ABytes/(1024.0*1024*1024),'f',2));
}

bool OmemoAttachments::encryptFile(const QString &AFileName, const QString &AOutputFileName,
	QString &AFragment, QString *AError)
{
	AFragment.clear();
	if (AError != nullptr)
		AError->clear();

	QFile input(AFileName);
	if (!input.open(QIODevice::ReadOnly))
	{
		if (AError != nullptr)
			*AError = QStringLiteral("cannot open source file: %1").arg(input.errorString());
		return false;
	}

	QSaveFile output(AOutputFileName);
	if (!output.open(QIODevice::WriteOnly))
	{
		if (AError != nullptr)
			*AError = QStringLiteral("cannot create encrypted file: %1").arg(output.errorString());
		return false;
	}

	unsigned char key[32] = {};
	unsigned char iv[12] = {};
	if (RAND_bytes(key,sizeof(key)) != 1 || RAND_bytes(iv,sizeof(iv)) != 1)
	{
		if (AError != nullptr)
			*AError = QStringLiteral("cannot generate attachment key: %1").arg(opensslError());
		output.cancelWriting();
		return false;
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	bool ok = ctx != nullptr
		&& EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr) > 0
		&& EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,sizeof(iv),nullptr) > 0
		&& EVP_EncryptInit_ex(ctx,nullptr,nullptr,key,iv) > 0;

	QByteArray inputBuffer(64*1024,'\0');
	QByteArray outputBuffer(inputBuffer.size()+16,'\0');
	while (ok && !input.atEnd())
	{
		qint64 read = input.read(inputBuffer.data(),inputBuffer.size());
		if (read < 0)
		{
			if (AError != nullptr)
				*AError = QStringLiteral("cannot read source file: %1").arg(input.errorString());
			ok = false;
			break;
		}
		if (read == 0)
			continue;

		int written = 0;
		ok = EVP_EncryptUpdate(ctx,reinterpret_cast<unsigned char *>(outputBuffer.data()),&written,
			reinterpret_cast<const unsigned char *>(inputBuffer.constData()),int(read)) > 0;
		if (ok)
			ok = output.write(outputBuffer.constData(),written) == written;
	}

	if (ok)
	{
		int finalWritten = 0;
		ok = EVP_EncryptFinal_ex(ctx,reinterpret_cast<unsigned char *>(outputBuffer.data()),&finalWritten) > 0;
		if (ok)
			ok = output.write(outputBuffer.constData(),finalWritten) == finalWritten;
	}

	unsigned char tag[16] = {};
	if (ok)
		ok = EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,sizeof(tag),tag) > 0;
	if (ok)
		ok = output.write(reinterpret_cast<const char *>(tag),sizeof(tag)) == sizeof(tag);

	if (ok)
	{
		ok = output.commit();
		if (!ok && AError != nullptr)
			*AError = QStringLiteral("cannot write encrypted file: %1").arg(output.errorString());
	}
	else
	{
		if (AError != nullptr && AError->isEmpty())
			*AError = QStringLiteral("file encryption failed: %1").arg(opensslError());
		output.cancelWriting();
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok)
		return false;

	AFragment = QByteArray(reinterpret_cast<const char *>(iv),sizeof(iv)).toHex()
		+ QByteArray(reinterpret_cast<const char *>(key),sizeof(key)).toHex();
	return true;
}

void OmemoAttachments::storeUploadedFile(OmemoAccount *AAccount, const QString &ACleanUrl,
	const QString &ASourceFileName, const QString &AFragment)
{
	if (AAccount == nullptr || AAccount->store() == nullptr || AFragment.size() != IvHexSize + KeyHexSize)
		return;

	QByteArray iv = QByteArray::fromHex(AFragment.left(IvHexSize).toLatin1());
	QByteArray key = QByteArray::fromHex(AFragment.mid(IvHexSize).toLatin1());
	if (iv.size() != 12 || key.size() != 32)
		return;

	QUrl url(ACleanUrl);
	url.setFragment(QString());
	const QString cleanUrl = QString::fromUtf8(url.toEncoded());

	OmemoStore::AttachmentKeys keys;
	keys.iv = iv;
	keys.key = key;
	keys.added = QDateTime::currentMSecsSinceEpoch();
	AAccount->store()->data().attachments.insert(cleanUrl,keys);
	pruneAttachments(AAccount);
	AAccount->store()->save();

	const QString path = attachmentFilePath(AAccount,cleanUrl);
	QDir dir(QFileInfo(path).absolutePath());
	if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
	{
		LOG_ERROR(QString("OMEMO attachment directory create failed, path=%1").arg(dir.absolutePath()));
		return;
	}

	QFile::remove(path);
	if (!QFile::copy(ASourceFileName,path))
		LOG_ERROR(QString("OMEMO attachment cache copy failed, from=%1, to=%2").arg(ASourceFileName,path));
}

void OmemoAttachments::downloadAttachment(OmemoAccount *AAccount, const QString &ACleanUrl)
{
	if (AAccount == nullptr || ACleanUrl.isEmpty())
		return;
	if (isFileCached(AAccount,ACleanUrl))
	{
		emit attachmentReady(AAccount,ACleanUrl,true);
		return;
	}

	const QString accountId = AAccount->accountId().toString(QUuid::WithoutBraces);
	if (FDownloads.value(accountId).contains(ACleanUrl))
		return;

	const QUrl url = QUrl(ACleanUrl);
	if (!url.isValid() || url.scheme() != QStringLiteral("https") || FNetwork->isHostBlocked(url))
	{
		LOG_WARNING(QString("OMEMO attachment download refused, url=%1").arg(ACleanUrl));
		return;
	}

	FDownloads[accountId].insert(ACleanUrl);

	OmemoAccount *account = AAccount;
	QPointer<OmemoAccount> guard(AAccount);
	FNetwork->request(ACleanUrl,url,MaxDownloadBytes,[this,account,guard,ACleanUrl](const QByteArray &AData)
	{
		const QString key = account->accountId().toString(QUuid::WithoutBraces);
		if (FDownloads.contains(key))
		{
			FDownloads[key].remove(ACleanUrl);
			if (FDownloads.value(key).isEmpty())
				FDownloads.remove(key);
		}

		bool success = false;
		if (guard && !AData.isEmpty())
		{
			const OmemoStore::AttachmentKeys *keys = nullptr;
			OmemoStore *store = account->store();
			if (store != nullptr)
			{
				const QHash<QString, OmemoStore::AttachmentKeys>::const_iterator it
					= store->data().attachments.constFind(ACleanUrl);
				if (it != store->data().attachments.constEnd())
					keys = &it.value();
			}
			QByteArray plain;
			QString error;
			if (keys != nullptr
				&& decryptAttachment(keys->key,keys->iv,AData,plain,error))
			{
				const QString path = attachmentFilePath(account,ACleanUrl);
				QDir dir(QFileInfo(path).absolutePath());
				if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
					LOG_ERROR(QString("OMEMO attachment directory create failed, path=%1").arg(dir.absolutePath()));

				QSaveFile file(path);
				if (file.open(QFile::WriteOnly))
				{
					file.write(plain);
					if (file.commit())
						success = true;
					else
						LOG_ERROR(QString("OMEMO attachment write failed, path=%1, error=%2").arg(path,file.errorString()));
				}
				else
					LOG_ERROR(QString("OMEMO attachment open failed, path=%1, error=%2").arg(path,file.errorString()));
			}
			else
				LOG_WARNING(QString("OMEMO attachment decrypt failed, url=%1, error=%2").arg(ACleanUrl,error));
		}

		if (guard)
			emit attachmentReady(account,ACleanUrl,success);
	});
}

void OmemoAttachments::pruneAttachments(OmemoAccount *AAccount)
{
	QHash<QString, OmemoStore::AttachmentKeys> &attachments = AAccount->store()->data().attachments;
	while (attachments.count() > MaxAttachments)
	{
		qint64 oldest = -1;
		QString oldestUrl;
		for (QHash<QString, OmemoStore::AttachmentKeys>::const_iterator it=attachments.constBegin();
			it!=attachments.constEnd(); ++it)
		{
			if (oldest < 0 || it.value().added < oldest)
			{
				oldest = it.value().added;
				oldestUrl = it.key();
			}
		}
		if (oldestUrl.isEmpty())
			break;
		attachments.remove(oldestUrl);
	}
}
