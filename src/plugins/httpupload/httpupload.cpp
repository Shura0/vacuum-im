#include "httpupload.h"
#include "httpuploadtask.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <definitions/namespaces.h>
#include <definitions/resources.h>
#include <definitions/menuicons.h>
#include <definitions/toolbargroups.h>
#include <definitions/shortcuts.h>
#include <definitions/internalerrors.h>
#include <definitions/notificationtypes.h>
#include <definitions/notificationtypeorders.h>
#include <definitions/notificationdataroles.h>
#include <definitions/actiongroups.h>
#include <definitions/rosterindexkinds.h>
#include <definitions/rosterindexroles.h>
#include <interfaces/imultiuserchat.h>
#include <interfaces/ipresencemanager.h>
#include <interfaces/ifiletransfer.h>
#include <utils/action.h>
#include <utils/iconstorage.h>
#include <utils/shortcuts.h>
#include <utils/toolbarchanger.h>
#include <utils/message.h>
#include <utils/logger.h>

#define ADR_STREAM_JID                Action::DR_StreamJid
#define ADR_CONTACT_JID               Action::DR_Parametr1
#define ADR_FILE_NAME                 Action::DR_Parametr2
#define ADR_MESSAGE_TYPE              Action::DR_Parametr3

HttpUploadsManager::HttpUploadsManager()
{
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FUrlProcessor = NULL;
	FFileEncryption = NULL;
	FMessageWidgets = NULL;
	FMessageProcessor = NULL;
	FNotifications = NULL;
	FRostersViewPlugin = NULL;
	FNetworkManager = NULL;
	FFileTransferPresent = false;
}

HttpUploadsManager::~HttpUploadsManager()
{
	foreach(HttpUploadTask *task, FUploads)
	{
		task->disconnect(this);
		if (task->parent() != this)
			task->abort();
	}
	FUploads.clear();
	FUploadContexts.clear();
}

void HttpUploadsManager::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("HTTP Upload");
	APluginInfo->description = tr("Uploads files to an HTTP server using XEP-0363 and sends the link");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-im Team";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
}

bool HttpUploadsManager::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	IPlugin *plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0,NULL);
	if (plugin)
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0,NULL);
	if (plugin)
	{
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());
		if (FDiscovery)
		{
			connect(FDiscovery->instance(),SIGNAL(discoOpened(const Jid &)),SLOT(onDiscoOpened(const Jid &)));
			connect(FDiscovery->instance(),SIGNAL(discoClosed(const Jid &)),SLOT(onDiscoClosed(const Jid &)));
			connect(FDiscovery->instance(),SIGNAL(discoItemsReceived(const IDiscoItems &)),SLOT(onDiscoItemsReceived(const IDiscoItems &)));
			connect(FDiscovery->instance(),SIGNAL(discoInfoReceived(const IDiscoInfo &)),SLOT(onDiscoInfoReceived(const IDiscoInfo &)));
			connect(FDiscovery->instance(),SIGNAL(discoInfoRemoved(const IDiscoInfo &)),SLOT(onDiscoInfoRemoved(const IDiscoInfo &)));
		}
	}

	plugin = APluginManager->pluginInterface("IUrlProcessor").value(0,NULL);
	if (plugin)
	{
		FUrlProcessor = qobject_cast<IUrlProcessor *>(plugin->instance());
		if (FUrlProcessor)
			FNetworkManager = FUrlProcessor->networkAccessManager();
	}
	if (FNetworkManager == NULL)
	{
		FNetworkManager = new QNetworkAccessManager(this);
	}

	plugin = APluginManager->pluginInterface("IFileUploadEncryption").value(0,NULL);
	if (plugin)
		FFileEncryption = qobject_cast<IFileUploadEncryption *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IMessageWidgets").value(0,NULL);
	if (plugin)
	{
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());
		if (FMessageWidgets)
			connect(FMessageWidgets->instance(),SIGNAL(toolBarWidgetCreated(IMessageToolBarWidget *)),SLOT(onToolBarWidgetCreated(IMessageToolBarWidget *)));
	}

	plugin = APluginManager->pluginInterface("IMessageProcessor").value(0,NULL);
	if (plugin)
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface("INotifications").value(0,NULL);
	if (plugin)
		FNotifications = qobject_cast<INotifications *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IRostersViewPlugin").value(0,NULL);
	if (plugin)
		FRostersViewPlugin = qobject_cast<IRostersViewPlugin *>(plugin->instance());

	FFileTransferPresent = !APluginManager->pluginInterface("IFileTransfer").isEmpty();

	return FStanzaProcessor!=NULL && FDiscovery!=NULL && FNetworkManager!=NULL;
}

bool HttpUploadsManager::initObjects()
{
	Shortcuts::declareShortcut(SCT_MESSAGEWINDOWS_UPLOADFILE,tr("Upload file"),QKeySequence());

	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_SERVICE_NOT_FOUND,tr("HTTP upload service not found"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_FILE_IO_ERROR,tr("File input/output error"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_FILE_TOO_LARGE,tr("File is too large"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_SLOT_REQUEST_FAILED,tr("Failed to request upload slot"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_INVALID_RESPONSE,tr("Invalid upload slot response"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_NETWORK_ERROR,tr("HTTP upload network error"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_HTTP_ERROR,tr("HTTP upload failed"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_ABORTED,tr("Upload aborted"));
	XmppError::registerError(NS_INTERNAL_ERROR,IERR_HTTPUPLOAD_STREAM_CLOSED,tr("Connection closed"));

	if (FNotifications != NULL)
	{
		INotificationType notifyType;
		notifyType.order = NTO_HTTPUPLOAD_NOTIFY;
		notifyType.icon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_FILETRANSFER_SEND);
		notifyType.title = tr("When an HTTP upload fails");
		notifyType.kindMask = INotification::PopupWindow|INotification::TrayNotify;
		notifyType.kindDefs = notifyType.kindMask;
		FNotifications->registerNotificationType(NNT_HTTPUPLOAD,notifyType);
	}

	if (!FFileTransferPresent && FMessageWidgets != NULL)
	{
		FMessageWidgets->insertViewDropHandler(this);
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
			if (window->toolBarWidget() != NULL)
				onToolBarWidgetCreated(window->toolBarWidget());
		foreach(IMessageNormalWindow *window, FMessageWidgets->normalWindows())
			if (window->toolBarWidget() != NULL)
				onToolBarWidgetCreated(window->toolBarWidget());
	}

	if (!FFileTransferPresent && FRostersViewPlugin!=NULL && FRostersViewPlugin->rostersView()!=NULL)
		FRostersViewPlugin->rostersView()->insertDragDropHandler(this);

	return true;
}

bool HttpUploadsManager::isSupported(const Jid &AStreamJid) const
{
	return selectService(AStreamJid).isValid();
}

qint64 HttpUploadsManager::maxFileSize(const Jid &AStreamJid) const
{
	return selectService(AStreamJid).maxFileSize;
}

QList<IHttpUpload *> HttpUploadsManager::uploads() const
{
	QList<IHttpUpload *> result;
	foreach(HttpUploadTask *task, FUploads)
		result.append(task);
	return result;
}

IHttpUpload *HttpUploadsManager::findUpload(const QString &AUploadId) const
{
	return FUploads.value(AUploadId);
}

IHttpUpload *HttpUploadsManager::createUpload(const Jid &AStreamJid, const QString &AFileName, QObject *AParent)
{
	UploadService service = selectService(AStreamJid);
	if (!service.isValid() || AFileName.isEmpty())
		return NULL;

	HttpUploadTask *task = new HttpUploadTask(FStanzaProcessor,FNetworkManager,AStreamJid,service.jid,service.ns,
		AFileName,service.maxFileSize,AParent!=NULL ? AParent : this);
	connect(task,SIGNAL(uploadDestroyed()),SLOT(onUploadDestroyed()));
	FUploads.insert(task->uploadId(),task);
	emit uploadCreated(task);
	return task;
}

bool HttpUploadsManager::uploadFile(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType, const QString &AFileName)
{
	return startUpload(AStreamJid,AContactJid,AMessageType,AFileName);
}

qint64 HttpUploadsManager::parseMaxFileSize(const IDiscoInfo &AInfo, const QString &AServiceNS) const
{
	qint64 maxFileSize = -1;
	foreach(const IDataForm &form, AInfo.extensions)
	{
		QString formType;
		qint64 formMaxSize = -1;
		foreach(const IDataField &field, form.fields)
		{
			if (field.var == QStringLiteral("FORM_TYPE"))
				formType = field.value.toString().trimmed();
			else if (field.var == QStringLiteral("max-file-size"))
			{
				bool ok = false;
				qint64 value = field.value.toString().trimmed().toLongLong(&ok);
				if (ok && value>0)
					formMaxSize = value;
			}
		}
		if (formMaxSize>0 && (formType.isEmpty() || formType==AServiceNS))
			maxFileSize = formMaxSize;
	}
	return maxFileSize;
}

bool HttpUploadsManager::serviceBetter(const UploadService &AFirst, const UploadService &ASecond) const
{
	bool firstCurrent = AFirst.ns == NS_HTTP_UPLOAD;
	bool secondCurrent = ASecond.ns == NS_HTTP_UPLOAD;
	if (firstCurrent != secondCurrent)
		return firstCurrent;

	qint64 firstMax = AFirst.maxFileSize>0 ? AFirst.maxFileSize : Q_INT64_C(0x7fffffffffffffff);
	qint64 secondMax = ASecond.maxFileSize>0 ? ASecond.maxFileSize : Q_INT64_C(0x7fffffffffffffff);
	if (firstMax != secondMax)
		return firstMax > secondMax;

	return AFirst.jid.full() < ASecond.jid.full();
}

HttpUploadsManager::UploadService HttpUploadsManager::selectService(const Jid &AStreamJid) const
{
	UploadService best;
	foreach(const UploadService &service, FServices.value(AStreamJid))
	{
		if (!best.isValid() || serviceBetter(service,best))
			best = service;
	}
	return best;
}

void HttpUploadsManager::processDiscoInfo(const IDiscoInfo &AInfo)
{
	if (!AInfo.error.isNull())
		return;

	QString serviceNS;
	if (AInfo.features.contains(NS_HTTP_UPLOAD))
		serviceNS = NS_HTTP_UPLOAD;
	else if (AInfo.features.contains(NS_HTTP_UPLOAD_LEGACY))
		serviceNS = NS_HTTP_UPLOAD_LEGACY;

	if (serviceNS.isEmpty())
	{
		if (FServices.contains(AInfo.streamJid) && FServices[AInfo.streamJid].contains(AInfo.contactJid))
		{
			FServices[AInfo.streamJid].remove(AInfo.contactJid);
			if (FServices[AInfo.streamJid].isEmpty())
				FServices.remove(AInfo.streamJid);
			updateToolBarActions(AInfo.streamJid);
			emit servicesChanged(AInfo.streamJid);
		}
		return;
	}

	UploadService service;
	service.jid = AInfo.contactJid;
	service.ns = serviceNS;
	service.maxFileSize = parseMaxFileSize(AInfo,serviceNS);

	FServices[AInfo.streamJid].insert(service.jid,service);
	LOG_STRM_INFO(AInfo.streamJid,QString("HTTP upload service found, jid=%1, ns=%2, max-file-size=%3")
		.arg(service.jid.full(),service.ns).arg(service.maxFileSize));
	updateToolBarActions(AInfo.streamJid);
	emit servicesChanged(AInfo.streamJid);
}

int HttpUploadsManager::messageTypeForWindow(IMessageWindow *AWindow) const
{
	if (qobject_cast<IMessageChatWindow *>(AWindow->instance()) != NULL)
		return Message::Chat;
	if (qobject_cast<IMultiUserChatWindow *>(AWindow->instance()) != NULL)
		return Message::GroupChat;
	return Message::Normal;
}

bool HttpUploadsManager::startUpload(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType,
	const QString &AFileName, IMessageViewWidget *AView)
{
	if (AFileName.isEmpty())
		return false;

	if (!isSupported(AStreamJid))
	{
		showStatus(AView,tr("HTTP upload is not supported for this account"));
		return false;
	}

	if (AView == NULL && FMessageWidgets != NULL)
	{
		IMessageChatWindow *chatWindow = FMessageWidgets->findChatWindow(AStreamJid,AContactJid);
		if (chatWindow != NULL)
		{
			AView = chatWindow->viewWidget();
		}
		else
		{
			IMessageNormalWindow *normalWindow = FMessageWidgets->findNormalWindow(AStreamJid,AContactJid);
			if (normalWindow != NULL)
				AView = normalWindow->viewWidget();
		}
	}

	bool encrypted = (AMessageType==Message::Chat || AMessageType==Message::Normal)
		&& FFileEncryption!=NULL
		&& FFileEncryption->isFileEncryptionEnabled(AStreamJid,AContactJid);

	QString uploadFileName = AFileName;
	QString encryptionFragment;
	QSharedPointer<QTemporaryDir> tempDir;
	if (encrypted)
	{
		tempDir = QSharedPointer<QTemporaryDir>(new QTemporaryDir);
		if (!tempDir->isValid())
		{
			showStatus(AView,tr("Failed to create a temporary directory for file encryption"));
			return false;
		}

		uploadFileName = tempDir->filePath(QFileInfo(AFileName).fileName());
		QString error;
		if (!FFileEncryption->encryptFile(AStreamJid,AContactJid,AFileName,uploadFileName,encryptionFragment,&error))
		{
			showStatus(AView,tr("Failed to encrypt file: %1").arg(error.toHtmlEscaped()));
			return false;
		}
	}

	IHttpUpload *upload = createUpload(AStreamJid,uploadFileName,this);
	HttpUploadTask *task = upload!=NULL ? qobject_cast<HttpUploadTask *>(upload->instance()) : NULL;
	if (task == NULL)
	{
		showStatus(AView,tr("Failed to create HTTP upload"));
		return false;
	}
	if (encrypted)
		task->setContentTypeOverride(QStringLiteral("application/octet-stream"));

	UploadContext context;
	context.streamJid = AStreamJid;
	context.contactJid = AContactJid;
	context.messageType = AMessageType;
	context.view = AView;
	context.viewGuard = AView!=NULL ? AView->instance() : NULL;
	context.encrypted = encrypted;
	context.sourceFileName = AFileName;
	context.encryptionFragment = encryptionFragment;
	context.tempDir = tempDir;
	FUploadContexts.insert(task,context);

	connect(task,SIGNAL(uploadFinished()),SLOT(onUploadFinished()));
	connect(task,SIGNAL(insecureUpload()),SLOT(onUploadInsecure()));

	showStatus(AView,tr("Uploading file %1...").arg(QFileInfo(AFileName).fileName().toHtmlEscaped()));
	upload->start();
	return true;
}

void HttpUploadsManager::updateToolBarAction(IMessageToolBarWidget *AWidget)
{
	if (FFileTransferPresent)
		return;

	IMessageWindow *window = AWidget->messageWindow();
	IMultiUserChatWindow *mucWindow = qobject_cast<IMultiUserChatWindow *>(window->instance());

	bool canUpload = isSupported(window->streamJid());
	if (canUpload && mucWindow!=NULL)
		canUpload = mucWindow->multiUserChat()->isOpen();

	Action *action = FUploadActions.value(AWidget);
	if (canUpload)
	{
		if (action == NULL)
		{
			action = new Action(AWidget->instance());
			action->setIcon(RSR_STORAGE_MENUICONS,MNI_FILETRANSFER_SEND);
			action->setText(tr("Upload File"));
			action->setToolTip(tr("Upload file to the HTTP server and send the link"));
			action->setShortcutId(SCT_MESSAGEWINDOWS_UPLOADFILE);
			connect(action,SIGNAL(triggered(bool)),SLOT(onUploadFileByAction(bool)));
			AWidget->toolBarChanger()->insertAction(action,TBG_MWTBW_FILETRANSFER);
			FUploadActions.insert(AWidget,action);
		}
		action->setEnabled(true);
	}
	else if (action != NULL)
	{
		action->setEnabled(false);
	}
}

void HttpUploadsManager::updateToolBarActions(const Jid &AStreamJid)
{
	foreach(IMessageToolBarWidget *widget, FWidgets)
		if (!AStreamJid.isValid() || widget->messageWindow()->streamJid()==AStreamJid)
			updateToolBarAction(widget);
}

void HttpUploadsManager::showStatus(IMessageViewWidget *AView, const QString &AHtml) const
{
	if (AView != NULL && !AHtml.isEmpty())
	{
		IMessageStyleContentOptions options;
		options.kind = IMessageStyleContentOptions::KindStatus;
		options.type |= IMessageStyleContentOptions::TypeEvent;
		options.direction = IMessageStyleContentOptions::DirectionIn;
		options.time = QDateTime::currentDateTime();
		AView->appendHtml(AHtml,options);
	}
}

void HttpUploadsManager::sendUploadedFile(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType,
	const QUrl &AUrl, const QString &AFileName, const QString &AEncryptionFragment) const
{
	if (FMessageProcessor == NULL || !AUrl.isValid())
		return;

	QUrl url = AUrl;
	url.setFragment(QString());
	QString urlString = url.toString(QUrl::FullyEncoded);

	Message message;
	message.setType((Message::MessageType)AMessageType);
	message.setFrom(AStreamJid.full()).setTo(AContactJid.full());

	if (!AEncryptionFragment.isEmpty())
	{
		QString urlPart = urlString;
		if (urlPart.startsWith(QStringLiteral("https://")))
			urlPart = urlPart.mid(8);
		else if (urlPart.startsWith(QStringLiteral("http://")))
			urlPart = urlPart.mid(7);
		message.setBody(QStringLiteral("aesgcm://") + urlPart + QLatin1Char('#') + AEncryptionFragment);
	}
	else
	{
		message.setBody(urlString);

		QDomElement oob = message.stanza().addElement("x",NS_JABBER_X_OOB);
		QDomElement urlElem = message.stanza().createElement("url");
		urlElem.appendChild(message.stanza().createTextNode(urlString));
		oob.appendChild(urlElem);
	}

	if (FMessageProcessor->sendMessage(AStreamJid,message,IMessageProcessor::DirectionOut))
		LOG_STRM_INFO(AStreamJid,QString("Sent HTTP upload link, to=%1, file=%2")
			.arg(AContactJid.full(),QFileInfo(AFileName).fileName()));
	else
		LOG_STRM_WARNING(AStreamJid,QString("Failed to send HTTP upload link, to=%1, file=%2")
			.arg(AContactJid.full(),QFileInfo(AFileName).fileName()));
}

void HttpUploadsManager::notifyUploadFailed(const Jid &AStreamJid, const QString &AFileName, const XmppError &AError) const
{
	if (FNotifications == NULL)
		return;

	INotification notify;
	notify.kinds = FNotifications->enabledTypeNotificationKinds(NNT_HTTPUPLOAD);
	notify.typeId = NNT_HTTPUPLOAD;
	notify.data.insert(NDR_ICON,IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_FILETRANSFER_SEND));
	notify.data.insert(NDR_TOOLTIP,tr("HTTP upload failed"));
	notify.data.insert(NDR_STREAM_JID,AStreamJid.full());
	notify.data.insert(NDR_POPUP_TITLE,tr("HTTP Upload"));
	notify.data.insert(NDR_POPUP_TEXT,tr("Failed to upload '%1': %2")
		.arg(QFileInfo(AFileName).fileName(),AError.errorMessage()));
	FNotifications->appendNotification(notify);
}

void HttpUploadsManager::onDiscoOpened(const Jid &AStreamJid)
{
	Jid serverJid = AStreamJid.domain();
	FDiscovery->requestDiscoInfo(AStreamJid,serverJid);
	FDiscovery->requestDiscoItems(AStreamJid,serverJid);
	updateToolBarActions(AStreamJid);
}

void HttpUploadsManager::onDiscoClosed(const Jid &AStreamJid)
{
	FServices.remove(AStreamJid);

	QList<HttpUploadTask *> tasks;
	foreach(HttpUploadTask *task, FUploads)
		if (task->streamJid() == AStreamJid)
			tasks.append(task);
	foreach(HttpUploadTask *task, tasks)
		task->abort(XmppError(IERR_HTTPUPLOAD_STREAM_CLOSED,tr("Connection closed")));

	updateToolBarActions(AStreamJid);
	emit servicesChanged(AStreamJid);
}

void HttpUploadsManager::onDiscoItemsReceived(const IDiscoItems &AItems)
{
	if (!AItems.error.isNull() || AItems.contactJid.bare()!=AItems.streamJid.domain())
		return;

	foreach(const IDiscoItem &item, AItems.items)
	{
		if (!item.itemJid.isValid() || item.itemJid.bare()==AItems.streamJid.domain())
			continue;

		if (FDiscovery->hasDiscoInfo(AItems.streamJid,item.itemJid,item.node))
			processDiscoInfo(FDiscovery->discoInfo(AItems.streamJid,item.itemJid,item.node));
		else
			FDiscovery->requestDiscoInfo(AItems.streamJid,item.itemJid,item.node);
	}
}

void HttpUploadsManager::onDiscoInfoReceived(const IDiscoInfo &AInfo)
{
	processDiscoInfo(AInfo);
}

void HttpUploadsManager::onDiscoInfoRemoved(const IDiscoInfo &AInfo)
{
	QMap<Jid, UploadService> &services = FServices[AInfo.streamJid];
	services.remove(AInfo.contactJid);
	if (services.isEmpty())
		FServices.remove(AInfo.streamJid);
	updateToolBarActions(AInfo.streamJid);
	emit servicesChanged(AInfo.streamJid);
}

void HttpUploadsManager::onUploadFinished()
{
	HttpUploadTask *task = qobject_cast<HttpUploadTask *>(sender());
	if (task == NULL)
		return;

	UploadContext context = FUploadContexts.take(task);
	bool viewAlive = context.view!=NULL && !context.viewGuard.isNull();

	if (task->state() == IHttpUpload::Finished)
	{
		if (context.encrypted && task->getUrl().scheme().compare(QStringLiteral("https"),Qt::CaseInsensitive)!=0)
		{
			XmppError error(IERR_HTTPUPLOAD_HTTP_ERROR,tr("Encrypted upload requires an HTTPS URL"));
			if (viewAlive)
				showStatus(context.view,tr("File upload failed: %1").arg(error.errorMessage().toHtmlEscaped()));
			else
				notifyUploadFailed(context.streamJid,context.sourceFileName,error);
		}
		else
		{
			if (context.encrypted && FFileEncryption!=NULL)
			{
				QUrl cleanUrl = task->getUrl();
				cleanUrl.setFragment(QString());
				FFileEncryption->registerUploadedFile(context.streamJid,
					cleanUrl.toString(QUrl::FullyEncoded),context.sourceFileName,context.encryptionFragment);
			}
			sendUploadedFile(context.streamJid,context.contactJid,context.messageType,task->getUrl(),
				context.encrypted ? context.sourceFileName : task->fileName(),
				context.encrypted ? context.encryptionFragment : QString());
		}
	}
	else
	{
		if (viewAlive)
			showStatus(context.view,tr("File upload failed: %1").arg(task->error().errorMessage().toHtmlEscaped()));
		else
			notifyUploadFailed(context.streamJid,
				context.encrypted ? context.sourceFileName : task->fileName(),task->error());
	}

	task->deleteLater();
}

void HttpUploadsManager::onUploadInsecure()
{
	HttpUploadTask *task = qobject_cast<HttpUploadTask *>(sender());
	if (task == NULL)
		return;

	UploadContext context = FUploadContexts.value(task);
	if (context.view!=NULL && !context.viewGuard.isNull())
		showStatus(context.view,tr("Uploading over insecure HTTP"));
	LOG_STRM_WARNING(task->streamJid(),QString("HTTP upload uses an insecure URL, file=%1")
		.arg(QFileInfo(task->fileName()).fileName()));
}

void HttpUploadsManager::onUploadDestroyed()
{
	HttpUploadTask *task = qobject_cast<HttpUploadTask *>(sender());
	if (task != NULL)
	{
		FUploads.remove(task->uploadId());
		FUploadContexts.remove(task);
		emit uploadDestroyed(task);
	}
}

void HttpUploadsManager::onToolBarWidgetCreated(IMessageToolBarWidget *AWidget)
{
	if (FFileTransferPresent)
		return;

	if (!FWidgets.contains(AWidget))
	{
		FWidgets.append(AWidget);
		connect(AWidget->instance(),SIGNAL(destroyed(QObject *)),SLOT(onToolBarWidgetDestroyed(QObject *)));
		IMessageAddress *address = AWidget->messageWindow()->address();
		if (address != NULL)
			connect(address->instance(),SIGNAL(addressChanged(const Jid &,const Jid &)),SLOT(onToolBarWidgetAddressChanged(const Jid &,const Jid &)));

		IMultiUserChatWindow *mucWindow = qobject_cast<IMultiUserChatWindow *>(AWidget->messageWindow()->instance());
		if (mucWindow != NULL)
			connect(mucWindow->multiUserChat()->instance(),SIGNAL(stateChanged(int)),SLOT(onMultiUserChatStateChanged(int)));
	}
	updateToolBarAction(AWidget);
}

void HttpUploadsManager::onToolBarWidgetDestroyed(QObject *AObject)
{
	for (QList<IMessageToolBarWidget *>::iterator it=FWidgets.begin(); it!=FWidgets.end(); )
	{
		IMessageToolBarWidget *widget = *it;
		if (widget->instance() == AObject)
		{
			FUploadActions.remove(widget);
			it = FWidgets.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void HttpUploadsManager::onToolBarWidgetAddressChanged(const Jid &AStreamBefore, const Jid &AContactBefore)
{
	Q_UNUSED(AStreamBefore);
	Q_UNUSED(AContactBefore);
	foreach(IMessageToolBarWidget *widget, FWidgets)
		updateToolBarAction(widget);
}

void HttpUploadsManager::onMultiUserChatStateChanged(int AState)
{
	Q_UNUSED(AState);
	IMultiUserChat *multiChat = qobject_cast<IMultiUserChat *>(sender());
	if (multiChat != NULL)
	{
		foreach(IMessageToolBarWidget *widget, FWidgets)
		{
			IMultiUserChatWindow *mucWindow = qobject_cast<IMultiUserChatWindow *>(widget->messageWindow()->instance());
			if (mucWindow!=NULL && mucWindow->multiUserChat()==multiChat)
				updateToolBarAction(widget);
		}
	}
}

void HttpUploadsManager::onUploadFileByAction(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	IMessageToolBarWidget *widget = FUploadActions.key(action);
	if (widget == NULL)
		return;

	IMessageWindow *window = widget->messageWindow();
	IMultiUserChatWindow *mucWindow = qobject_cast<IMultiUserChatWindow *>(window->instance());
	if (mucWindow!=NULL && !mucWindow->multiUserChat()->isOpen())
		return;

	QString fileName = QFileDialog::getOpenFileName(window->instance(),tr("Select File"));
	if (fileName.isEmpty())
		return;

	startUpload(window->streamJid(),window->contactJid(),messageTypeForWindow(window),fileName,window->viewWidget());
}

void HttpUploadsManager::onDropUploadByAction(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	if (action == NULL)
		return;

	startUpload(action->data(ADR_STREAM_JID).toString(),
		action->data(ADR_CONTACT_JID).toString(),
		action->data(ADR_MESSAGE_TYPE).toInt(),
		action->data(ADR_FILE_NAME).toString());
}

Qt::DropActions HttpUploadsManager::rosterDragStart(const QMouseEvent *AEvent, IRosterIndex *AIndex, QDrag *ADrag)
{
	Q_UNUSED(AEvent); Q_UNUSED(AIndex); Q_UNUSED(ADrag);
	return Qt::IgnoreAction;
}

bool HttpUploadsManager::rosterDragEnter(const QDragEnterEvent *AEvent)
{
	if (AEvent->mimeData()->hasUrls())
	{
		foreach(const QUrl &url, AEvent->mimeData()->urls())
			if (!QFileInfo(url.toLocalFile()).isFile())
				return false;
		return !AEvent->mimeData()->urls().isEmpty();
	}
	return false;
}

bool HttpUploadsManager::rosterDragMove(const QDragMoveEvent *AEvent, IRosterIndex *AHover)
{
	if (AEvent->mimeData()->urls().isEmpty())
		return false;

	int kind = AHover->data(RDR_KIND).toInt();
	if (kind == RIK_MUC_ITEM)
	{
		int show = AHover->data(RDR_SHOW).toInt();
		return isSupported(AHover->data(RDR_STREAM_JID).toString()) && show!=IPresence::Offline && show!=IPresence::Error;
	}
	else if (kind != RIK_STREAM_ROOT)
	{
		return isSupported(AHover->data(RDR_STREAM_JID).toString());
	}
	return false;
}

void HttpUploadsManager::rosterDragLeave(const QDragLeaveEvent *AEvent)
{
	Q_UNUSED(AEvent);
}

bool HttpUploadsManager::rosterDropAction(const QDropEvent *AEvent, IRosterIndex *AIndex, Menu *AMenu)
{
	if (AEvent->dropAction()==Qt::IgnoreAction)
		return false;

	QList<QUrl> urls = AEvent->mimeData()->urls();
	if (urls.count()!=1 || !QFileInfo(urls.first().toLocalFile()).isFile())
		return false;

	Jid streamJid(AIndex->data(RDR_STREAM_JID).toString());
	if (!isSupported(streamJid))
		return false;

	int kind = AIndex->data(RDR_KIND).toInt();
	Jid contactJid;
	int messageType;
	if (kind == RIK_MUC_ITEM)
	{
		contactJid = Jid(AIndex->data(RDR_FULL_JID).toString()).bare();
		messageType = Message::GroupChat;
	}
	else if (kind != RIK_STREAM_ROOT)
	{
		contactJid = AIndex->data(RDR_FULL_JID).toString();
		messageType = Message::Chat;
	}
	else
	{
		return false;
	}

	Action *uploadAction = new Action(AMenu);
	uploadAction->setText(tr("Upload File"));
	uploadAction->setIcon(RSR_STORAGE_MENUICONS,MNI_FILETRANSFER_SEND);
	uploadAction->setData(ADR_STREAM_JID,streamJid.full());
	uploadAction->setData(ADR_CONTACT_JID,contactJid.full());
	uploadAction->setData(ADR_FILE_NAME,urls.first().toLocalFile());
	uploadAction->setData(ADR_MESSAGE_TYPE,messageType);
	connect(uploadAction,SIGNAL(triggered(bool)),SLOT(onDropUploadByAction(bool)));
	AMenu->addAction(uploadAction,AG_DEFAULT,true);
	AMenu->setDefaultAction(uploadAction);
	return true;
}

bool HttpUploadsManager::messageViewDragEnter(IMessageViewWidget *AWidget, const QDragEnterEvent *AEvent)
{
	if (!AEvent->mimeData()->hasUrls())
		return false;

	foreach(const QUrl &url, AEvent->mimeData()->urls())
		if (!QFileInfo(url.toLocalFile()).isFile())
			return false;

	if (AEvent->mimeData()->urls().isEmpty())
		return false;

	IMessageWindow *window = AWidget->messageWindow();
	if (!isSupported(window->streamJid()))
		return false;

	IMultiUserChatWindow *mucWindow = qobject_cast<IMultiUserChatWindow *>(window->instance());
	return mucWindow==NULL || mucWindow->multiUserChat()->isOpen();
}

bool HttpUploadsManager::messageViewDragMove(IMessageViewWidget *AWidget, const QDragMoveEvent *AEvent)
{
	Q_UNUSED(AWidget); Q_UNUSED(AEvent);
	return true;
}

void HttpUploadsManager::messageViewDragLeave(IMessageViewWidget *AWidget, const QDragLeaveEvent *AEvent)
{
	Q_UNUSED(AWidget); Q_UNUSED(AEvent);
}

bool HttpUploadsManager::messageViewDropAction(IMessageViewWidget *AWidget, const QDropEvent *AEvent, Menu *AMenu)
{
	if (AEvent->dropAction()==Qt::IgnoreAction)
		return false;

	QList<QUrl> urls = AEvent->mimeData()->urls();
	if (urls.isEmpty())
		return false;
	if (urls.count()!=1 || !QFileInfo(urls.first().toLocalFile()).isFile())
	{
		showStatus(AWidget,tr("Only one file can be uploaded at a time"));
		return false;
	}

	IMessageWindow *window = AWidget->messageWindow();
	if (!isSupported(window->streamJid()))
		return false;

	int messageType = messageTypeForWindow(window);
	Jid contactJid = window->contactJid();
	if (messageType == Message::GroupChat)
		contactJid = contactJid.bare();

	Action *uploadAction = new Action(AMenu);
	uploadAction->setText(tr("Upload File"));
	uploadAction->setIcon(RSR_STORAGE_MENUICONS,MNI_FILETRANSFER_SEND);
	uploadAction->setData(ADR_STREAM_JID,window->streamJid().full());
	uploadAction->setData(ADR_CONTACT_JID,contactJid.full());
	uploadAction->setData(ADR_FILE_NAME,urls.first().toLocalFile());
	uploadAction->setData(ADR_MESSAGE_TYPE,messageType);
	connect(uploadAction,SIGNAL(triggered(bool)),SLOT(onDropUploadByAction(bool)));
	AMenu->addAction(uploadAction,AG_DEFAULT,true);
	AMenu->setDefaultAction(uploadAction);
	return true;
}
