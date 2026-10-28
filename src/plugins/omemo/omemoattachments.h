#ifndef OMEMOATTACHMENTS_H
#define OMEMOATTACHMENTS_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>

#include <interfaces/imessagewidgets.h>

#include "omemoaccount.h"
#include "omemonetwork.h"

// Receiving OMEMO encrypted files and images (XEP-0454).
//
// An OMEMO message body of the form
//     aesgcm://<https-url>#<iv-hex><key-hex>
// optionally followed by a single new line and a data: URI thumbnail carries
// an AES-256-GCM encrypted file.  The message key material must never reach
// the plaintext archive, so the body is sanitized: the aesgcm:// scheme is
// rewritten to https:// and the #iv+key fragment is removed before the
// message is stored or shown.  The key material is kept only inside the
// encrypted OmemoStore.  The https url becomes the anchor href of the
// message, so the inserted message looks like a normal link until the
// decrypted local file replaces it with an inline image.
//
// The decrypted files are cached under <filesPath>/omemo/files/<account>/.
class OmemoAttachments : public QObject
{
	Q_OBJECT
public:
	// Upper bounds: the attachment key table is capped to protect the store
	// and the transfers are buffered in memory before being written to disk.
	static const int MaxAttachments = 500;
	static const qint64 MaxDownloadBytes = 32 * 1024 * 1024;

	explicit OmemoAttachments(QObject *AParent = nullptr);

	// Rewrites every aesgcm url of the body: registers the key material and
	// the thumbnail in the encrypted store, returns the sanitized body
	// (https urls without the key fragment, without the thumbnail line).
	// When AAutoDownload is set the file transfer starts immediately if the
	// file is not cached yet.
	QString sanitizeBody(OmemoAccount *AAccount, const QString &ABody, bool AAutoDownload);

	// True when the url is a registered attachment of this account.
	bool isAttachment(OmemoAccount *AAccount, const QString &ACleanUrl) const;
	// The thumbnail data: URI of the attachment, or an empty string.
	QString attachmentThumbnail(OmemoAccount *AAccount, const QString &ACleanUrl) const;

	// Local path of the decrypted file.  The suffix of the url is preserved
	// when Qt can display it, otherwise "bin" is used.
	static QString attachmentFilePath(OmemoAccount *AAccount, const QString &ACleanUrl);
	static bool isFileCached(OmemoAccount *AAccount, const QString &ACleanUrl);
	// The human readable name of the attachment, as the sender uploaded it.
	static QString attachmentFileName(const QString &ACleanUrl);
	// True when Qt supports the suffix of the (local) file as an image type.
	static bool isImageFilePath(const QString &ALocalFile);
	// Human readable size, e.g. "3.5 MB".
	static QString formatFileSize(qint64 ABytes);

	// Starts (or joins) the download of the encrypted file.  On success the
	// decrypted file is written to the cache and attachmentReady() is emitted.
	void downloadAttachment(OmemoAccount *AAccount, const QString &ACleanUrl);

signals:
	// The decrypted file has appeared in the cache; the view can insert it.
	void attachmentReady(OmemoAccount *AAccount, const QString &ACleanUrl, bool ASuccess);

private:
	void pruneAttachments(OmemoAccount *AAccount);

private:
	OmemoNetworkManager *FNetwork;
	// account uuid -> clean url (in-flight downloads)
	QHash<QString, QSet<QString> > FDownloads;
};

#endif // OMEMOATTACHMENTS_H
