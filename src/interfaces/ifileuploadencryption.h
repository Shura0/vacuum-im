#ifndef IFILEUPLOADENCRYPTION_H
#define IFILEUPLOADENCRYPTION_H

#include <QObject>
#include <QString>
#include <utils/jid.h>

#define FILEUPLOADENCRYPTION_UUID "{22B8EE4B-0128-4144-8F14-8E71151F7F3A}"

// Optional end-to-end file encryption hook for HTTP upload.
//
// When enabled for a chat, the file is encrypted before it is uploaded and the
// message body uses the XEP-0454 aesgcm:// scheme.  The encryption key and IV
// must never be written to the plaintext message archive.
class IFileUploadEncryption
{
public:
	virtual QObject *instance() =0;
	virtual bool isFileEncryptionEnabled(const Jid &AStreamJid, const Jid &AContactJid) =0;
	// Encrypts AFileName into AOutputFileName.  AFragment is the XEP-0454
	// "#<iv hex><key hex>" suffix to append to the https upload URL.
	virtual bool encryptFile(const Jid &AStreamJid, const Jid &AContactJid,
		const QString &AFileName, const QString &AOutputFileName,
		QString &AFragment, QString *AError = nullptr) =0;
	// Stores the attachment key material for the final upload URL and caches
	// the plaintext for the local outgoing message display.
	virtual void registerUploadedFile(const Jid &AStreamJid, const QString &ACleanUrl,
		const QString &ASourceFileName, const QString &AFragment) =0;
};

Q_DECLARE_INTERFACE(IFileUploadEncryption,"Vacuum.Plugin.IFileUploadEncryption/1.0")

#endif // IFILEUPLOADENCRYPTION_H
