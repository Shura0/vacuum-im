#include "contactmuter.h"

#include <QDate>
#include <QDateTime>
#include <definitions/notificationdataroles.h>
#include <definitions/notificationhandlerorders.h>
#include <definitions/actiongroups.h>
#include <definitions/optionvalues.h>
#include <definitions/menuicons.h>
#include <definitions/resources.h>
#include <definitions/rosterindexroles.h>
#include <definitions/rosterindexkinds.h>
#include <definitions/rosterlabels.h>
#include <definitions/rostertooltiporders.h>
#include <utils/iconstorage.h>
#include <utils/options.h>
#include <utils/logger.h>

enum MuteActionDataRole {
	MADR_STREAM_JID = 0,
	MADR_CONTACT_JID
};

ContactMuter::ContactMuter()
{
	FMuterLabelId = 0;
	FNotifications = NULL;
	FRostersViewPlugin = NULL;
	FRostersModel = NULL;
}

ContactMuter::~ContactMuter()
{
	if (FNotifications)
		FNotifications->removeNotificationHandler(NHO_CONTACTMUTER,this);
}

void ContactMuter::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Contact Muter");
	APluginInfo->description = tr("Allows to mute the notifications for individual contacts");
	APluginInfo->version = "1.1";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "https://github.com/Vacuum-IM";
	APluginInfo->dependences.append(QUuid(NOTIFICATIONS_UUID));
	APluginInfo->dependences.append(QUuid(ROSTERSVIEW_UUID));
}

bool ContactMuter::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	IPlugin *plugin = APluginManager->pluginInterface("INotifications").value(0,NULL);
	if (plugin)
	{
		FNotifications = qobject_cast<INotifications *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IRostersViewPlugin").value(0,NULL);
	if (plugin)
	{
		FRostersViewPlugin = qobject_cast<IRostersViewPlugin *>(plugin->instance());
		if (FRostersViewPlugin && FRostersViewPlugin->rostersView()!=NULL)
		{
			connect(FRostersViewPlugin->rostersView()->instance(),SIGNAL(indexContextMenu(const QList<IRosterIndex *> &, quint32, Menu *)),
				SLOT(onRostersViewIndexContextMenu(const QList<IRosterIndex *> &, quint32, Menu *)));
			connect(FRostersViewPlugin->rostersView()->instance(),SIGNAL(indexToolTips(IRosterIndex *, quint32, QMap<int,QString> &)),
				SLOT(onRosterIndexToolTips(IRosterIndex *, quint32, QMap<int,QString> &)));
		}
	}

	plugin = APluginManager->pluginInterface("IRostersModel").value(0,NULL);
	if (plugin)
	{
		FRostersModel = qobject_cast<IRostersModel *>(plugin->instance());
		if (FRostersModel)
		{
			connect(FRostersModel->instance(),SIGNAL(indexInserted(IRosterIndex *)),SLOT(onRosterIndexInserted(IRosterIndex *)));
		}
	}

	//The profile options are opened later than the plugins are loaded
	connect(Options::instance(),SIGNAL(optionsOpened()),SLOT(onOptionsOpened()));

	return FNotifications!=NULL && FRostersViewPlugin!=NULL;
}

bool ContactMuter::initObjects()
{
	if (FNotifications)
	{
		FNotifications->insertNotificationHandler(NHO_CONTACTMUTER,this);
	}
	if (FRostersViewPlugin && FRostersViewPlugin->rostersView()!=NULL)
	{
		AdvancedDelegateItem label(RLID_CONTACTMUTER);
		label.d->kind = AdvancedDelegateItem::CustomData;
		label.d->data = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)->getIcon(MNI_MUC_NOTIFY_SILENCE);
		FMuterLabelId = FRostersViewPlugin->rostersView()->registerLabel(label);
	}

	rebuildCache();

	return true;
}

bool ContactMuter::initSettings()
{
	Options::setDefaultValue(OPV_CONTACTMUTER_MUTERS_ROOT,QString());
	Options::setDefaultValue(OPV_CONTACTMUTER_MUTER_ITEM,QString());
	return true;
}

bool ContactMuter::startPlugin()
{
	updateAllMutersLabels();
	return true;
}

bool ContactMuter::showNotification(int AOrder, ushort AKind, int ANotifyId, const INotification &ANotification)
{
	Q_UNUSED(AOrder);
	Jid streamJid = ANotification.data.value(NDR_STREAM_JID).toString();
	Jid contactJid = ANotification.data.value(NDR_CONTACT_JID).toString();
	if (streamJid.isValid() && contactJid.isValid() && isContactMuted(streamJid,contactJid))
	{
		LOG_DEBUG(QString("Contact muter: kind 0x%1 of notification %2 suppressed, contact=%3").arg(AKind,4,16).arg(ANotifyId).arg(contactJid.pBare()));
		return true;	//Swallow every notification kind, the notification is cleaned up by 'RemoveInvisible'
	}
	return false;
}

void ContactMuter::onRosterIndexInserted(IRosterIndex *AIndex)
{
	if (FMuterLabelId!=0 && FRostersViewPlugin!=NULL && isContactMuted(Jid(AIndex->data(RDR_STREAM_JID).toString()),Jid(AIndex->data(RDR_PREP_BARE_JID).toString())))
		FRostersViewPlugin->rostersView()->insertLabel(FMuterLabelId,AIndex);
}

void ContactMuter::onRosterIndexToolTips(IRosterIndex *AIndex, quint32 ALabelId, QMap<int,QString> &AToolTips)
{
	if (ALabelId==AdvancedDelegateItem::DisplayId || ALabelId==FMuterLabelId)
	{
		if (isContactMuted(Jid(AIndex->data(RDR_STREAM_JID).toString()),Jid(AIndex->data(RDR_PREP_BARE_JID).toString())))
			AToolTips.insert(RTTO_CONTACTMUTER,tr("Notifications disabled"));
	}
}

void ContactMuter::rebuildCache()
{
	FMutedContacts.clear();
	foreach(const QString &nspace, Options::node(OPV_CONTACTMUTER_MUTERS_ROOT).childNSpaces("muter"))
	{
		OptionsNode node = Options::node(OPV_CONTACTMUTER_MUTER_ITEM,nspace);
		QString streamBare = node.value("stream").toString();
		QString contactBare = node.value("contact").toString();
		if (!streamBare.isEmpty() && !contactBare.isEmpty())
			FMutedContacts[streamBare].insert(contactBare);
	}
}

void ContactMuter::onOptionsOpened()
{
	//Rebuild the cache from the freshly loaded profile options
	rebuildCache();

	if (FMuterLabelId!=0 && FRostersViewPlugin!=NULL)
		FRostersViewPlugin->rostersView()->removeLabel(FMuterLabelId);	//Drop stale labels of a changed profile

	updateAllMutersLabels();
	LOG_DEBUG(QString("Contact muter: %1 stream(s) with mute rules loaded on options open").arg(FMutedContacts.size()));
}

void ContactMuter::updateContactLabels(const Jid &AStreamJid, const Jid &AContactJid)
{
	if (FMuterLabelId!=0 && FRostersViewPlugin!=NULL && FRostersModel!=NULL)
	{
		QString streamBare = AStreamJid.pBare();
		QString contactBare = AContactJid.pBare();

		QMultiMap<int,QVariant> findData;
		findData.insert(RDR_PREP_BARE_JID,contactBare);
		foreach(IRosterIndex *index, FRostersModel->rootIndex()->findChilds(findData,true))
		{
			if (Jid(index->data(RDR_STREAM_JID).toString()).pBare()==streamBare)
			{
				if (isContactMuted(AStreamJid,AContactJid))
					FRostersViewPlugin->rostersView()->insertLabel(FMuterLabelId,index);
				else
					FRostersViewPlugin->rostersView()->removeLabel(FMuterLabelId,index);
			}
		}
	}
}

void ContactMuter::updateAllMutersLabels()
{
	if (FMuterLabelId!=0 && FRostersViewPlugin!=NULL && FRostersModel!=NULL)
	{
		//Remove the labels from all the roster indexes, then apply the actual rules
		FRostersViewPlugin->rostersView()->removeLabel(FMuterLabelId);
		for (QMap<QString,QSet<QString> >::const_iterator it=FMutedContacts.constBegin(); it!=FMutedContacts.constEnd(); ++it)
		{
			foreach(const QString &contactBare, it.value())
			{
				QMultiMap<int,QVariant> findData;
				findData.insert(RDR_PREP_BARE_JID,contactBare);
				foreach(IRosterIndex *index, FRostersModel->rootIndex()->findChilds(findData,true))
				{
					if (Jid(index->data(RDR_STREAM_JID).toString()).pBare()==it.key())
						FRostersViewPlugin->rostersView()->insertLabel(FMuterLabelId,index);
				}
			}
		}
	}
}

bool ContactMuter::isContactMuted(const Jid &AStreamJid, const Jid &AContactJid) const
{
	QString streamBare = AStreamJid.pBare();
	QString contactBare = AContactJid.pBare();
	return !streamBare.isEmpty() && !contactBare.isEmpty() && FMutedContacts.value(streamBare).contains(contactBare);
}

void ContactMuter::setContactMuted(const Jid &AStreamJid, const Jid &AContactJid, bool AMuted)
{
	QString streamBare = AStreamJid.pBare();
	QString contactBare = AContactJid.pBare();
	if (streamBare.isEmpty() || contactBare.isEmpty())
		return;

	//Sync the in-memory cache
	bool changed = AMuted ? !FMutedContacts[streamBare].contains(contactBare) : FMutedContacts.contains(streamBare);
	if (AMuted)
		FMutedContacts[streamBare].insert(contactBare);
	else if (FMutedContacts.contains(streamBare))
	{
		FMutedContacts[streamBare].remove(contactBare);
		if (FMutedContacts.value(streamBare).isEmpty())
			FMutedContacts.remove(streamBare);
	}

	//Look up the existing rule and remove it
	OptionsNode mutersNode = Options::node(OPV_CONTACTMUTER_MUTERS_ROOT);
	foreach(const QString &nspace, mutersNode.childNSpaces("muter"))
	{
		OptionsNode node = mutersNode.node("muter",nspace);
		if (node.value("stream").toString()==streamBare && node.value("contact").toString()==contactBare)
			mutersNode.removeChilds("muter",nspace);
	}

	//Append a new rule
	if (AMuted)
	{
		OptionsNode node = mutersNode.node("muter",QString::number(QDateTime::currentMSecsSinceEpoch()));
		node.setValue(streamBare,"stream");
		node.setValue(contactBare,"contact");
	}

	if (changed)
		updateContactLabels(AStreamJid,AContactJid);
}

bool ContactMuter::isSelectionAccepted(const QList<IRosterIndex *> &AIndexes) const
{
	int singleKind = -1;
	foreach(IRosterIndex *index, AIndexes)
	{
		Jid contactJid = index->data(RDR_FULL_JID).toString();
		if (!contactJid.isValid())
			return false;
		else if (singleKind!=-1 && singleKind!=index->kind())
			return false;
		singleKind = index->kind();
	}
	return !AIndexes.isEmpty();
}

void ContactMuter::onRostersViewIndexContextMenu(const QList<IRosterIndex *> &AIndexes, quint32 ALabelId, Menu *AMenu)
{
	if (ALabelId==AdvancedDelegateItem::DisplayId && isSelectionAccepted(AIndexes))
	{
		QMap<int, QStringList> rolesMap = FRostersViewPlugin->rostersView()->indexesRolesMap(AIndexes,
			QList<int>()<<RDR_STREAM_JID<<RDR_PREP_BARE_JID,RDR_PREP_BARE_JID,RDR_STREAM_JID);
		QStringList streams = rolesMap.value(RDR_STREAM_JID);
		QStringList contacts = rolesMap.value(RDR_PREP_BARE_JID);

		bool isAllMuted = streams.count()>0 && contacts.count()>0;
		for (int i=0; isAllMuted && i<contacts.count(); i++)
			isAllMuted = isAllMuted && isContactMuted(Jid(streams.at(i)),Jid(contacts.at(i)));

		Menu *menu = new Menu(AMenu);
		menu->setTitle(tr("Notifications"));
		menu->setIcon(RSR_STORAGE_MENUICONS,MNI_NOTIFICATIONS);

		Action *muteAction = new Action(menu);
		muteAction->setCheckable(true);
		muteAction->setText(tr("Mute notifications"));
		muteAction->setData(MADR_STREAM_JID,streams);
		muteAction->setData(MADR_CONTACT_JID,contacts);
		muteAction->setChecked(isAllMuted);
		connect(muteAction,SIGNAL(triggered(bool)),SLOT(onMuteByAction(bool)));
		menu->addAction(muteAction,AG_DEFAULT,false);

		AMenu->addAction(menu->menuAction(),AG_RVCM_CONTACTMUTER_OPTIONS,true);
	}
}

void ContactMuter::onMuteByAction(bool AChecked)
{
	Action *action = qobject_cast<Action *>(sender());
	if (action)
	{
		QStringList streams = action->data(MADR_STREAM_JID).toStringList();
		QStringList contacts = action->data(MADR_CONTACT_JID).toStringList();
		for (int i=0; i<contacts.count(); i++)
			setContactMuted(Jid(streams.at(i)),Jid(contacts.at(i)),AChecked);
	}
}
