#ifndef IHTTPUPLOAD_H
#define IHTTPUPLOAD_H

#include <QObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <utils/jid.h>
#include <utils/xmpperror.h>

#define HTTPUPLOAD_UUID "{19582966-15b0-4969-85e0-ff15c262e17d}"

class IHttpUpload
{
public:
	enum State {
		Creating,
		RequestingSlot,
		Uploading,
		Finished,
		Failed,
		Aborted
	};
public:
	virtual QObject *instance() =0;
	virtual QString uploadId() const =0;
	virtual Jid streamJid() const =0;
	virtual Jid serviceJid() const =0;
	virtual QString serviceNS() const =0;
	virtual QString fileName() const =0;
	virtual QString contentType() const =0;
	virtual qint64 fileSize() const =0;
	virtual State state() const =0;
	virtual qint64 bytesSent() const =0;
	virtual QUrl getUrl() const =0;
	virtual XmppError error() const =0;
	virtual bool start() =0;
	virtual void abort(const XmppError &AError = XmppError()) =0;
protected:
	virtual void stateChanged() =0;
	virtual void progressChanged() =0;
	virtual void uploadFinished() =0;
	virtual void uploadDestroyed() =0;
};

class IHttpUploadsManager
{
public:
	virtual QObject *instance() =0;
	virtual bool isSupported(const Jid &AStreamJid) const =0;
	virtual qint64 maxFileSize(const Jid &AStreamJid) const =0;
	virtual QList<IHttpUpload *> uploads() const =0;
	virtual IHttpUpload *findUpload(const QString &AUploadId) const =0;
	virtual IHttpUpload *createUpload(const Jid &AStreamJid, const QString &AFileName, QObject *AParent = NULL) =0;
	// Uploads a file and sends the resulting link to the contact or room.
	// Returns false when no HTTP upload service is available or the task
	// cannot be created, so the caller can fall back to another transport.
	virtual bool uploadFile(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType, const QString &AFileName) =0;
protected:
	virtual void uploadCreated(IHttpUpload *AUpload) =0;
	virtual void uploadDestroyed(IHttpUpload *AUpload) =0;
	virtual void servicesChanged(const Jid &AStreamJid) =0;
};

Q_DECLARE_INTERFACE(IHttpUpload,"Vacuum.Plugin.IHttpUpload/1.0")
Q_DECLARE_INTERFACE(IHttpUploadsManager,"Vacuum.Plugin.IHttpUploadsManager/1.0")

#endif // IHTTPUPLOAD_H
