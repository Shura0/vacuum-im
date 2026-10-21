#ifndef CONTACTMUTER_H
#define CONTACTMUTER_H

#include <QObject>
#include <QUuid>
#include <QMap>
#include <QSet>
#include <interfaces/ipluginmanager.h>
#include <interfaces/inotifications.h>
#include <interfaces/irostersview.h>
#include <interfaces/irostersmodel.h>

#define CONTACTMUTER_UUID "0AEA7C01-A85D-4D65-A8D7-7CF0FCED56B9"

class IRosterIndex;

class ContactMuter :
	public QObject,
	public IPlugin,
	public INotificationHandler
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin INotificationHandler);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.ContactMuter");
public:
	ContactMuter();
	virtual ~ContactMuter();
	//IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(CONTACTMUTER_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin();
	//INotificationHandler
	virtual bool showNotification(int AOrder, ushort AKind, int ANotifyId, const INotification &ANotification);
public:
	bool isContactMuted(const Jid &AStreamJid, const Jid &AContactJid) const;
	void setContactMuted(const Jid &AStreamJid, const Jid &AContactJid, bool AMuted);
protected:
	bool isSelectionAccepted(const QList<IRosterIndex *> &AIndexes) const;
protected slots:
	void onRostersViewIndexContextMenu(const QList<IRosterIndex *> &AIndexes, quint32 ALabelId, Menu *AMenu);
	void onMuteByAction(bool AChecked);
	void onRosterIndexInserted(IRosterIndex *AIndex);
	void onRosterIndexToolTips(IRosterIndex *AIndex, quint32 ALabelId, QMap<int,QString> &AToolTips);
	void onOptionsOpened();
protected:
	void rebuildCache();
	void updateContactLabels(const Jid &AStreamJid, const Jid &AContactJid);
	void updateAllMutersLabels();
private:
	quint32 FMuterLabelId;
	INotifications *FNotifications;
	IRostersViewPlugin *FRostersViewPlugin;
	IRostersModel *FRostersModel;
	QMap<QString,QSet<QString> > FMutedContacts;
};

#endif // CONTACTMUTER_H
