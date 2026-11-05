#ifndef HTTPUPLOAD_H
#define HTTPUPLOAD_H

#include <QMap>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include <QNetworkAccessManager>
#include <interfaces/ipluginmanager.h>
#include <interfaces/ihttpupload.h>
#include <interfaces/ifileuploadencryption.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/iservicediscovery.h>
#include <interfaces/iurlprocessor.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/irostersview.h>
#include <interfaces/inotifications.h>

class Action;
class HttpUploadTask;
class QTemporaryDir;

class HttpUploadsManager :
	public QObject,
	public IPlugin,
	public IHttpUploadsManager,
	public IRostersDragDropHandler,
	public IMessageViewDropHandler
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IHttpUploadsManager IRostersDragDropHandler IMessageViewDropHandler);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.HttpUpload");
public:
	HttpUploadsManager();
	~HttpUploadsManager();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(HTTPUPLOAD_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings() { return true; }
	virtual bool startPlugin() { return true; }
	//IHttpUploadsManager
	virtual bool isSupported(const Jid &AStreamJid) const;
	virtual qint64 maxFileSize(const Jid &AStreamJid) const;
	virtual QList<IHttpUpload *> uploads() const;
	virtual IHttpUpload *findUpload(const QString &AUploadId) const;
	virtual IHttpUpload *createUpload(const Jid &AStreamJid, const QString &AFileName, QObject *AParent = NULL);
	virtual bool uploadFile(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType, const QString &AFileName);
	//IRostersDragDropHandler
	virtual Qt::DropActions rosterDragStart(const QMouseEvent *AEvent, IRosterIndex *AIndex, QDrag *ADrag);
	virtual bool rosterDragEnter(const QDragEnterEvent *AEvent);
	virtual bool rosterDragMove(const QDragMoveEvent *AEvent, IRosterIndex *AHover);
	virtual void rosterDragLeave(const QDragLeaveEvent *AEvent);
	virtual bool rosterDropAction(const QDropEvent *AEvent, IRosterIndex *AIndex, Menu *AMenu);
	//IMessageViewDropHandler
	virtual bool messageViewDragEnter(IMessageViewWidget *AWidget, const QDragEnterEvent *AEvent);
	virtual bool messageViewDragMove(IMessageViewWidget *AWidget, const QDragMoveEvent *AEvent);
	virtual void messageViewDragLeave(IMessageViewWidget *AWidget, const QDragLeaveEvent *AEvent);
	virtual bool messageViewDropAction(IMessageViewWidget *AWidget, const QDropEvent *AEvent, Menu *AMenu);
signals:
	void uploadCreated(IHttpUpload *AUpload);
	void uploadDestroyed(IHttpUpload *AUpload);
	void servicesChanged(const Jid &AStreamJid);
protected:
	struct UploadService
	{
		UploadService() : maxFileSize(-1) {}
		bool isValid() const { return jid.isValid() && !ns.isEmpty(); }
		Jid jid;
		QString ns;
		qint64 maxFileSize;
	};
	struct UploadContext
	{
		UploadContext() : messageType(0), view(NULL), encrypted(false) {}
		Jid streamJid;
		Jid contactJid;
		int messageType;
		IMessageViewWidget *view;
		QPointer<QObject> viewGuard;
		bool encrypted;
		QString sourceFileName;
		QString encryptionFragment;
		QSharedPointer<QTemporaryDir> tempDir;
	};
protected:
	qint64 parseMaxFileSize(const IDiscoInfo &AInfo, const QString &AServiceNS) const;
	UploadService selectService(const Jid &AStreamJid) const;
	bool serviceBetter(const UploadService &AFirst, const UploadService &ASecond) const;
	void processDiscoInfo(const IDiscoInfo &AInfo);
	int messageTypeForWindow(IMessageWindow *AWindow) const;
	bool startUpload(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType,
		const QString &AFileName, IMessageViewWidget *AView = NULL);
	void updateToolBarAction(IMessageToolBarWidget *AWidget);
	void updateToolBarActions(const Jid &AStreamJid = Jid::null);
	void showStatus(IMessageViewWidget *AView, const QString &AHtml) const;
	void sendUploadedFile(const Jid &AStreamJid, const Jid &AContactJid, int AMessageType,
		const QUrl &AUrl, const QString &AFileName, const QString &AEncryptionFragment = QString()) const;
	void notifyUploadFailed(const Jid &AStreamJid, const QString &AFileName, const XmppError &AError) const;
protected slots:
	void onDiscoOpened(const Jid &AStreamJid);
	void onDiscoClosed(const Jid &AStreamJid);
	void onDiscoItemsReceived(const IDiscoItems &AItems);
	void onDiscoInfoReceived(const IDiscoInfo &AInfo);
	void onDiscoInfoRemoved(const IDiscoInfo &AInfo);
	void onUploadFinished();
	void onUploadInsecure();
	void onUploadDestroyed();
	void onToolBarWidgetCreated(IMessageToolBarWidget *AWidget);
	void onToolBarWidgetDestroyed(QObject *AObject);
	void onToolBarWidgetAddressChanged(const Jid &AStreamBefore, const Jid &AContactBefore);
	void onMultiUserChatStateChanged(int AState);
	void onUploadFileByAction(bool);
	void onDropUploadByAction(bool);
private:
	IStanzaProcessor *FStanzaProcessor;
	IServiceDiscovery *FDiscovery;
	IUrlProcessor *FUrlProcessor;
	IFileUploadEncryption *FFileEncryption;
	IMessageWidgets *FMessageWidgets;
	IMessageProcessor *FMessageProcessor;
	INotifications *FNotifications;
	IRostersViewPlugin *FRostersViewPlugin;
private:
	QNetworkAccessManager *FNetworkManager;
	bool FFileTransferPresent;
private:
	QMap<Jid, QMap<Jid, UploadService> > FServices;
	QMap<QString, HttpUploadTask *> FUploads;
	QMap<HttpUploadTask *, UploadContext> FUploadContexts;
private:
	QList<IMessageToolBarWidget *> FWidgets;
	QMap<IMessageToolBarWidget *, Action *> FUploadActions;
};

#endif // HTTPUPLOAD_H
