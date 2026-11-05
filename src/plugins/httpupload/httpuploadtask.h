#ifndef HTTPUPLOADTASK_H
#define HTTPUPLOADTASK_H

#include <QFile>
#include <QList>
#include <QNetworkReply>
#include <QPair>
#include <QTimer>
#include <QUrl>
#include <interfaces/ihttpupload.h>
#include <interfaces/istanzaprocessor.h>

class QNetworkAccessManager;

class HttpUploadTask :
	public QObject,
	public IHttpUpload,
	public IStanzaRequestOwner
{
	Q_OBJECT;
	Q_INTERFACES(IHttpUpload IStanzaRequestOwner);
public:
	HttpUploadTask(IStanzaProcessor *AStanzaProcessor, QNetworkAccessManager *ANetworkManager,
		const Jid &AStreamJid, const Jid &AServiceJid, const QString &AServiceNS,
		const QString &AFileName, qint64 AMaxFileSize, QObject *AParent = NULL);
	~HttpUploadTask();
	//IHttpUpload
	virtual QObject *instance() { return this; }
	virtual QString uploadId() const;
	virtual Jid streamJid() const;
	virtual Jid serviceJid() const;
	virtual QString serviceNS() const;
	virtual QString fileName() const;
	virtual QString contentType() const;
	virtual qint64 fileSize() const;
	virtual State state() const;
	virtual qint64 bytesSent() const;
	virtual QUrl getUrl() const;
	virtual XmppError error() const;
	virtual bool start();
	virtual void abort(const XmppError &AError = XmppError());
	//IStanzaRequestOwner
	virtual void stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza);
	void setContentTypeOverride(const QString &AContentType);
signals:
	void stateChanged();
	void progressChanged();
	void uploadFinished();
	void uploadDestroyed();
	void insecureUpload();
protected:
	bool fail(const XmppError &AError);
	bool fail(const QString &ACondition, const QString &AMessage);
	void setState(State AState);
	bool parseSlot(const QDomElement &ASlot);
	void startHttpUpload();
	void cleanupReply();
	void finishUpload();
	void startStallTimer();
	void stopStallTimer();
	static QString readSlotUrl(const QDomElement &ASlotElement);
	static bool isHttpUrl(const QUrl &AUrl);
protected slots:
	void onReplyUploadProgress(qint64 ABytesSent, qint64 ABytesTotal);
	void onReplyFinished();
	void onStallTimeout();
private:
	IStanzaProcessor *FStanzaProcessor;
	QNetworkAccessManager *FNetworkManager;
private:
	QFile FFile;
	QNetworkReply *FReply;
	QTimer FStallTimer;
	QString FUploadId;
	Jid FStreamJid;
	Jid FServiceJid;
	QString FServiceNS;
	QString FFileName;
	QString FContentType;
	QString FContentTypeOverride;
	qint64 FFileSize;
	qint64 FMaxFileSize;
	State FState;
	qint64 FBytesSent;
	QUrl FPutUrl;
	QUrl FGetUrl;
	QList<QPair<QByteArray,QByteArray> > FHeaders;
	XmppError FError;
	QString FRequestId;
	bool FFinishedEmitted;
};

#endif // HTTPUPLOADTASK_H
