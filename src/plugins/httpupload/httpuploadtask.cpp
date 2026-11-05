#include "httpuploadtask.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUuid>
#include <definitions/namespaces.h>
#include <definitions/internalerrors.h>
#include <utils/logger.h>

#define HTTPUPLOAD_SLOT_TIMEOUT   30000
#define HTTPUPLOAD_STALL_TIMEOUT  60000
#define HTTPUPLOAD_MAX_HEADER     8192
#define HTTPUPLOAD_MAX_HEADER_NAME 256

HttpUploadTask::HttpUploadTask(IStanzaProcessor *AStanzaProcessor, QNetworkAccessManager *ANetworkManager,
	const Jid &AStreamJid, const Jid &AServiceJid, const QString &AServiceNS,
	const QString &AFileName, qint64 AMaxFileSize, QObject *AParent) :
	QObject(AParent),
	FStanzaProcessor(AStanzaProcessor),
	FNetworkManager(ANetworkManager),
	FReply(NULL),
	FStallTimer(this),
	FUploadId(QUuid::createUuid().toString()),
	FStreamJid(AStreamJid),
	FServiceJid(AServiceJid),
	FServiceNS(AServiceNS),
	FFileName(AFileName),
	FFileSize(0),
	FMaxFileSize(AMaxFileSize),
	FState(Creating),
	FBytesSent(0),
	FFinishedEmitted(false)
{
	FStallTimer.setSingleShot(true);
	FStallTimer.setInterval(HTTPUPLOAD_STALL_TIMEOUT);
	connect(&FStallTimer,SIGNAL(timeout()),SLOT(onStallTimeout()));
}

HttpUploadTask::~HttpUploadTask()
{
	cleanupReply();
	FFile.close();
	emit uploadDestroyed();
}

QString HttpUploadTask::uploadId() const
{
	return FUploadId;
}

Jid HttpUploadTask::streamJid() const
{
	return FStreamJid;
}

Jid HttpUploadTask::serviceJid() const
{
	return FServiceJid;
}

QString HttpUploadTask::serviceNS() const
{
	return FServiceNS;
}

QString HttpUploadTask::fileName() const
{
	return FFileName;
}

QString HttpUploadTask::contentType() const
{
	return FContentType;
}

qint64 HttpUploadTask::fileSize() const
{
	return FFileSize;
}

IHttpUpload::State HttpUploadTask::state() const
{
	return FState;
}

qint64 HttpUploadTask::bytesSent() const
{
	return FBytesSent;
}

QUrl HttpUploadTask::getUrl() const
{
	return FGetUrl;
}

XmppError HttpUploadTask::error() const
{
	return FError;
}

void HttpUploadTask::setContentTypeOverride(const QString &AContentType)
{
	if (FState == Creating)
		FContentTypeOverride = AContentType;
}

bool HttpUploadTask::start()
{
	if (FState != Creating)
		return false;

	QFileInfo fileInfo(FFileName);
	if (!fileInfo.isFile())
		return fail(IERR_HTTPUPLOAD_FILE_IO_ERROR,tr("File not found: %1").arg(FFileName));
	if (fileInfo.size() <= 0)
		return fail(IERR_HTTPUPLOAD_FILE_IO_ERROR,tr("File is empty: %1").arg(FFileName));
	if (FMaxFileSize > 0 && fileInfo.size() > FMaxFileSize)
		return fail(IERR_HTTPUPLOAD_FILE_TOO_LARGE,tr("File is too large: maximum size is %1 bytes").arg(FMaxFileSize));

	FFileSize = fileInfo.size();
	FContentType = !FContentTypeOverride.isEmpty()
		? FContentTypeOverride : QMimeDatabase().mimeTypeForFile(fileInfo).name();
	if (FContentType.isEmpty())
		FContentType = QStringLiteral("application/octet-stream");

	FFile.setFileName(FFileName);
	if (!FFile.open(QIODevice::ReadOnly))
		return fail(IERR_HTTPUPLOAD_FILE_IO_ERROR,tr("Failed to open file: %1").arg(FFileName));

	Stanza stanza(STANZA_KIND_IQ,STANZA_NS_CLIENT);
	stanza.setType(STANZA_TYPE_GET).setTo(FServiceJid.full()).setUniqueId();
	QDomElement request = stanza.addElement("request",FServiceNS);
	if (FServiceNS == NS_HTTP_UPLOAD_LEGACY)
	{
		QDomElement nameElem = stanza.createElement("filename",FServiceNS);
		nameElem.appendChild(stanza.createTextNode(fileInfo.fileName()));
		request.appendChild(nameElem);

		QDomElement sizeElem = stanza.createElement("size",FServiceNS);
		sizeElem.appendChild(stanza.createTextNode(QString::number(FFileSize)));
		request.appendChild(sizeElem);

		QDomElement typeElem = stanza.createElement("content-type",FServiceNS);
		typeElem.appendChild(stanza.createTextNode(FContentType));
		request.appendChild(typeElem);
	}
	else
	{
		request.setAttribute("filename",fileInfo.fileName());
		request.setAttribute("size",QString::number(FFileSize));
		request.setAttribute("content-type",FContentType);
	}

	FRequestId = stanza.id();
	setState(RequestingSlot);
	if (!FStanzaProcessor->sendStanzaRequest(this,FStreamJid,stanza,HTTPUPLOAD_SLOT_TIMEOUT))
		return fail(IERR_HTTPUPLOAD_SLOT_REQUEST_FAILED,tr("Failed to send upload slot request"));

	LOG_STRM_INFO(FStreamJid,QString("HTTP upload slot requested, file=%1, size=%2, to=%3").arg(fileInfo.fileName()).arg(FFileSize).arg(FServiceJid.full()));
	return true;
}

void HttpUploadTask::abort(const XmppError &AError)
{
	if (FState==Finished || FState==Failed || FState==Aborted)
		return;

	FError = AError.isNull() ? XmppError(IERR_HTTPUPLOAD_ABORTED,tr("Upload aborted")) : AError;
	FRequestId.clear();
	cleanupReply();
	FFile.close();
	setState(Aborted);
	finishUpload();
	LOG_STRM_INFO(FStreamJid,QString("HTTP upload aborted, file=%1").arg(QFileInfo(FFileName).fileName()));
}

void HttpUploadTask::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	Q_UNUSED(AStreamJid);
	if (FState!=RequestingSlot || AStanza.id()!=FRequestId)
		return;

	FRequestId.clear();
	if (!AStanza.isResult())
	{
		XmppStanzaError error(AStanza);
		LOG_STRM_WARNING(FStreamJid,QString("Failed to request HTTP upload slot, file=%1, to=%2: %3")
			.arg(QFileInfo(FFileName).fileName(),FServiceJid.full(),error.condition()));
		fail(error);
		return;
	}

	QDomElement slot = AStanza.firstElement("slot",FServiceNS);
	if (slot.isNull() || !parseSlot(slot))
	{
		LOG_STRM_WARNING(FStreamJid,QString("Failed to request HTTP upload slot, file=%1, to=%2: invalid slot")
			.arg(QFileInfo(FFileName).fileName(),FServiceJid.full()));
		fail(IERR_HTTPUPLOAD_INVALID_RESPONSE,tr("Upload slot contains invalid URLs"));
		return;
	}

	startHttpUpload();
}

bool HttpUploadTask::fail(const XmppError &AError)
{
	if (FState==Finished || FState==Failed || FState==Aborted)
		return false;

	FError = AError;
	FRequestId.clear();
	cleanupReply();
	FFile.close();
	setState(Failed);
	finishUpload();
	return false;
}

bool HttpUploadTask::fail(const QString &ACondition, const QString &AMessage)
{
	return fail(XmppError(ACondition,AMessage));
}

void HttpUploadTask::setState(State AState)
{
	if (FState != AState)
	{
		FState = AState;
		emit stateChanged();
	}
}

QString HttpUploadTask::readSlotUrl(const QDomElement &ASlotElement)
{
	QString url = ASlotElement.attribute("url").trimmed();
	if (url.isEmpty())
	{
		QDomElement urlElem = ASlotElement.firstChildElement("url");
		if (!urlElem.isNull())
			url = urlElem.text().trimmed();
	}
	if (url.isEmpty())
	{
		// Legacy servers may put the URL directly as the element text, even
		// when the element also contains <header> children.
		for (QDomNode node = ASlotElement.firstChild(); !node.isNull(); node = node.nextSibling())
		{
			if (node.isText())
			{
				url = node.nodeValue().trimmed();
				if (!url.isEmpty())
					break;
			}
		}
	}
	return url;
}

bool HttpUploadTask::isHttpUrl(const QUrl &AUrl)
{
	if (!AUrl.isValid() || AUrl.isEmpty())
		return false;
	QString scheme = AUrl.scheme().toLower();
	return scheme==QStringLiteral("http") || scheme==QStringLiteral("https");
}

bool HttpUploadTask::parseSlot(const QDomElement &ASlot)
{
	QDomElement put = ASlot.firstChildElement("put");
	QDomElement get = ASlot.firstChildElement("get");
	if (put.isNull() || get.isNull())
		return false;

	QUrl putUrl(readSlotUrl(put));
	QUrl getUrl(readSlotUrl(get));
	if (!isHttpUrl(putUrl) || !isHttpUrl(getUrl))
		return false;

	FPutUrl = putUrl;
	FGetUrl = getUrl;

	FHeaders.clear();
	static const QRegularExpression headerNameRegex(QStringLiteral("^[A-Za-z0-9!#$%&'*+.^_`|~-]+$"));
	for (QDomElement header = put.firstChildElement("header"); !header.isNull(); header = header.nextSiblingElement("header"))
	{
		QString name = header.attribute("name").trimmed();
		if (name.isEmpty() || name.length()>HTTPUPLOAD_MAX_HEADER_NAME || !headerNameRegex.match(name).hasMatch())
			continue;

		QString value = header.text();
		value.remove(QLatin1Char('\r'));
		value.remove(QLatin1Char('\n'));
		if (value.toUtf8().size() > HTTPUPLOAD_MAX_HEADER)
			continue;

		FHeaders.append(qMakePair(name.toLatin1(),value.toUtf8()));
	}

	if (putUrl.scheme().compare(QStringLiteral("http"),Qt::CaseInsensitive)==0 ||
		getUrl.scheme().compare(QStringLiteral("http"),Qt::CaseInsensitive)==0)
	{
		emit insecureUpload();
	}

	return true;
}

void HttpUploadTask::startHttpUpload()
{
	if (FNetworkManager == NULL)
	{
		fail(IERR_HTTPUPLOAD_NETWORK_ERROR,tr("HTTP network manager is not available"));
		return;
	}

	QNetworkRequest request(FPutUrl);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute,true);

	for (int i=0; i<FHeaders.count(); ++i)
		request.setRawHeader(FHeaders.at(i).first,FHeaders.at(i).second);

	request.setHeader(QNetworkRequest::ContentTypeHeader,FContentType);
	request.setHeader(QNetworkRequest::ContentLengthHeader,FFileSize);

	FReply = FNetworkManager->put(request,&FFile);
	if (FReply == NULL)
	{
		fail(IERR_HTTPUPLOAD_NETWORK_ERROR,tr("Failed to start HTTP upload"));
		return;
	}

	connect(FReply,&QNetworkReply::uploadProgress,this,&HttpUploadTask::onReplyUploadProgress);
	connect(FReply,&QNetworkReply::finished,this,&HttpUploadTask::onReplyFinished);
	setState(Uploading);
	startStallTimer();
	LOG_STRM_INFO(FStreamJid,QString("HTTP upload started, file=%1, size=%2, host=%3")
		.arg(QFileInfo(FFileName).fileName()).arg(FFileSize).arg(FPutUrl.host()));
}

void HttpUploadTask::cleanupReply()
{
	stopStallTimer();
	if (FReply != NULL)
	{
		FReply->disconnect(this);
		FReply->abort();
		FReply->deleteLater();
		FReply = NULL;
	}
}

void HttpUploadTask::finishUpload()
{
	if (!FFinishedEmitted)
	{
		FFinishedEmitted = true;
		emit uploadFinished();
	}
}

void HttpUploadTask::startStallTimer()
{
	FStallTimer.start();
}

void HttpUploadTask::stopStallTimer()
{
	FStallTimer.stop();
}

void HttpUploadTask::onReplyUploadProgress(qint64 ABytesSent, qint64 ABytesTotal)
{
	Q_UNUSED(ABytesTotal);
	if (FState == Uploading)
	{
		FBytesSent = ABytesSent;
		startStallTimer();
		emit progressChanged();
	}
}

void HttpUploadTask::onReplyFinished()
{
	QNetworkReply *reply = FReply;
	if (reply == NULL)
		return;

	FReply = NULL;
	stopStallTimer();
	int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	QNetworkReply::NetworkError networkError = reply->error();
	QString errorString = reply->errorString();
	reply->readAll();
	reply->deleteLater();
	FFile.close();

	if (FState != Uploading)
		return;

	if (status >= 400)
	{
		LOG_STRM_WARNING(FStreamJid,QString("HTTP upload failed, file=%1, host=%2: HTTP status %3")
			.arg(QFileInfo(FFileName).fileName(),FPutUrl.host()).arg(status));
		fail(IERR_HTTPUPLOAD_HTTP_ERROR,tr("Server returned HTTP status %1").arg(status));
	}
	else if (networkError != QNetworkReply::NoError)
	{
		LOG_STRM_WARNING(FStreamJid,QString("HTTP upload failed, file=%1, host=%2: %3")
			.arg(QFileInfo(FFileName).fileName(),FPutUrl.host(),errorString));
		fail(IERR_HTTPUPLOAD_NETWORK_ERROR,errorString);
	}
	else if (status<200 || status>=300)
	{
		LOG_STRM_WARNING(FStreamJid,QString("HTTP upload failed, file=%1, host=%2: HTTP status %3")
			.arg(QFileInfo(FFileName).fileName(),FPutUrl.host()).arg(status));
		fail(IERR_HTTPUPLOAD_HTTP_ERROR,tr("Server returned HTTP status %1").arg(status));
	}
	else
	{
		FBytesSent = FFileSize;
		setState(Finished);
		emit progressChanged();
		finishUpload();
		LOG_STRM_INFO(FStreamJid,QString("HTTP upload finished, file=%1, host=%2")
			.arg(QFileInfo(FFileName).fileName(),FGetUrl.host()));
	}
}

void HttpUploadTask::onStallTimeout()
{
	if (FState == Uploading)
	{
		LOG_STRM_WARNING(FStreamJid,QString("HTTP upload stalled, file=%1, host=%2")
			.arg(QFileInfo(FFileName).fileName(),FPutUrl.host()));
		fail(IERR_HTTPUPLOAD_NETWORK_ERROR,tr("HTTP upload stalled"));
	}
}
