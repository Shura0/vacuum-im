#include "messagedisplayedsync.h"

#include <definitions/namespaces.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
#include <definitions/messagedataroles.h>
#include <definitions/optionnodes.h>
#include <definitions/optionvalues.h>
#include <definitions/optionwidgetorders.h>
#include <utils/options.h>
#include <utils/logger.h>
#include <QDomElement>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>
#include <algorithm>

#define DISPLAYED_STATE_REQUEST_TIMEOUT   30000
#define DISPLAYED_STATE_PUBLISH_DELAY     1000
#define DISPLAYED_STATE_SAVE_DELAY        2000
#define DISPLAYED_STATE_MAX_CHATS         1000

// XEP-0490 Message Displayed Synchronization.
//
// The server injects a XEP-0359 <stanza-id by='user@domain' id='...'/> into
// every message it archives. The plugin remembers the newest received message
// of every chat and, once the chat window is actually shown to the user,
// publishes that identifier into the 'urn:xmpp:mds:displayed:0' PEP node:
//
//   <iq type='set'>
//     <pubsub xmlns='http://jabber.org/protocol/pubsub'>
//       <publish node='urn:xmpp:mds:displayed:0'>
//         <item id='contact@domain'>
//           <displayed xmlns='urn:xmpp:mds:displayed:0'>
//             <stanza-id xmlns='urn:xmpp:sid:0' by='user@domain' id='...'/>
//           </displayed>
//         </item>
//       </publish>
//       <publish-options>...</publish-options>
//     </pubsub>
//   </iq>
//
// The plugin also listens to the same node (live events and the catch-up
// items) and clears the notifications of the chats which were read in the
// other clients of the account. The publish-options keep the node private:
// without a whitelist access model the read state would be visible to the
// contacts.

static void addPublishOptionField(Stanza &AStanza, QDomElement &AForm, const QString &AVar, const QString &AValue, bool AHidden=false)
{
	QDomElement field = AStanza.createElement("field");
	field.setAttribute("var", AVar);
	if (AHidden)
		field.setAttribute("type", "hidden");
	QDomElement value = AStanza.createElement("value");
	value.appendChild(AStanza.createTextNode(AValue));
	field.appendChild(value);
	AForm.appendChild(field);
}

MessageDisplayedSync::MessageDisplayedSync()
{
	FPluginManager = NULL;
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FMessageProcessor = NULL;
	FMessageWidgets = NULL;
	FOptionsManager = NULL;
	FArchiver = NULL;
	FStreamManager = NULL;
	FPEPManager = NULL;
	FStateLoaded = false;
	FPEPHandleId = 0;

	FPublishTimer.setSingleShot(true);
	connect(&FPublishTimer,&QTimer::timeout,this,&MessageDisplayedSync::onPublishTimeout);

	FSaveTimer.setSingleShot(true);
	connect(&FSaveTimer,&QTimer::timeout,this,&MessageDisplayedSync::onStateSaveTimeout);
}

MessageDisplayedSync::~MessageDisplayedSync()
{
	if (FSaveTimer.isActive() && FStateLoaded)
		saveState();
	if (FMessageWidgets)
		FMessageWidgets->removeStyleHandler(MSHO_MESSAGEDISPLAYEDSYNC,this);
	if (FMessageProcessor)
		FMessageProcessor->removeMessageEditor(MEO_DEFAULT,this);
	if (FOptionsManager)
		FOptionsManager->removeOptionsDialogHolder(this);
	if (FPEPManager && FPEPHandleId>0)
		FPEPManager->removeNodeHandler(FPEPHandleId);
}

void MessageDisplayedSync::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Message Displayed Sync");
	APluginInfo->description = tr("Synchronizes the displayed state of chats between the user clients (XEP-0490)");
	APluginInfo->version = "1.1";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->dependences.append(QUuid(OPTIONSMANAGER_UUID));
	APluginInfo->dependences.append(QUuid(XMPPSTREAMS_UUID));
	APluginInfo->dependences.append(QUuid(PEPMANAGER_UUID));
}

bool MessageDisplayedSync::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0,NULL);
	if (plugin)
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0,NULL);
	if (plugin)
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IMessageProcessor").value(0,NULL);
	if (plugin)
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IMessageWidgets").value(0,NULL);
	if (plugin)
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IOptionsManager").value(0,NULL);
	if (plugin)
		FOptionsManager = qobject_cast<IOptionsManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IMessageArchiver").value(0,NULL);
	if (plugin)
		FArchiver = qobject_cast<IMessageArchiver *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IXmppStreamManager").value(0,NULL);
	if (plugin)
		FStreamManager = qobject_cast<IXmppStreamManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IPEPManager").value(0,NULL);
	if (plugin)
		FPEPManager = qobject_cast<IPEPManager *>(plugin->instance());
	else
		LOG_WARNING("PEP manager is not available, the displayed states of the other clients will not be received");

	return FStanzaProcessor!=NULL && FMessageProcessor!=NULL && FMessageWidgets!=NULL && FDiscovery!=NULL;
}

bool MessageDisplayedSync::initObjects()
{
	if (FPEPManager)
	{
		FPEPHandleId = FPEPManager->insertNodeHandler(NS_MESSAGE_DISPLAYED_SYNC,this);
	}

	if (FStreamManager)
	{
		connect(FStreamManager->instance(),SIGNAL(streamOpened(IXmppStream *)),SLOT(onStreamOpened(IXmppStream *)));
		connect(FStreamManager->instance(),SIGNAL(streamClosed(IXmppStream *)),SLOT(onStreamClosed(IXmppStream *)));
		foreach(IXmppStream *stream, FStreamManager->xmppStreams())
			onStreamOpened(stream);
	}

	if (FDiscovery)
	{
		connect(FDiscovery->instance(),SIGNAL(discoInfoReceived(const IDiscoInfo &)),SLOT(onDiscoInfoReceived(const IDiscoInfo &)));

		IDiscoFeature dfeature;
		dfeature.var = QString(NS_MESSAGE_DISPLAYED_SYNC)+"+notify";
		dfeature.active = true;
		dfeature.name = tr("Message Displayed Synchronization");
		dfeature.description = tr("Synchronizes the displayed state of chats between the user clients");
		FDiscovery->insertDiscoFeature(dfeature);
	}

	if (FMessageProcessor)
	{
		FMessageProcessor->insertMessageEditor(MEO_DEFAULT,this);
	}

	if (FMessageWidgets)
	{
		FMessageWidgets->insertStyleHandler(MSHO_MESSAGEDISPLAYEDSYNC,this);
		connect(FMessageWidgets->instance(),SIGNAL(chatWindowCreated(IMessageChatWindow *)),SLOT(onChatWindowCreated(IMessageChatWindow *)));
		connect(FMessageWidgets->instance(),SIGNAL(tabWindowCreated(IMessageTabWindow *)),SLOT(onTabWindowCreated(IMessageTabWindow *)));
		foreach(IMessageTabWindow *tabWindow, FMessageWidgets->tabWindows())
			onTabWindowCreated(tabWindow);
	}

	if (FArchiver)
	{
		connect(FArchiver->instance(),SIGNAL(collectionLoaded(const QString &, const IArchiveCollection &)),SLOT(onCollectionLoaded(const QString &, const IArchiveCollection &)));
	}
	else
	{
		LOG_WARNING("Message archiver is not available, only live messages will be synchronized");
	}

	return true;
}

bool MessageDisplayedSync::initSettings()
{
	Options::setDefaultValue(OPV_MESSAGES_DISPLAYEDSYNCENABLED,true);

	if (FOptionsManager)
	{
		FOptionsManager->insertOptionsDialogHolder(this);
	}

	return true;
}

QMultiMap<int, IOptionsDialogWidget *> MessageDisplayedSync::optionsDialogWidgets(const QString &ANodeId, QWidget *AParent)
{
	QMultiMap<int, IOptionsDialogWidget *> widgets;
	if (FOptionsManager && ANodeId==OPN_MESSAGES)
	{
		widgets.insert(OWO_MESSAGES_DISPLAYEDSYNCENABLED, FOptionsManager->newOptionsDialogWidget(Options::node(OPV_MESSAGES_DISPLAYEDSYNCENABLED),tr("Synchronize the read state between my clients"),AParent));
	}
	return widgets;
}

bool MessageDisplayedSync::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder);

	if (ADirection!=IMessageProcessor::DirectionIn || !isEnabled())
		return false;

	// Only one-to-one chat messages delivered by the server are tracked.
	if (AMessage.type()!=Message::Chat || !AStreamJid.isValid())
		return false;

	// The message must carry the id the user own server assigned to it.
	QDomElement sidElem = AMessage.stanza().firstElement("stanza-id",NS_STANZA_ID);
	if (sidElem.isNull() || sidElem.attribute("id").isEmpty())
		return false;

	Jid sidBy(sidElem.attribute("by"));
	if (!sidBy.isValid() || sidBy.pBare()!=AStreamJid.pBare())
		return false;

	// Messages of the own account (carbon copies of the sent ones) are not
	// received chat messages and never mark anything as displayed.
	if (AMessage.fromJid().pBare()==AStreamJid.pBare())
		return false;

	recordStanzaId(AStreamJid,AMessage.fromJid(),sidElem.attribute("id"),AMessage.dateTime());
	return false;
}

bool MessageDisplayedSync::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AHtml);
	Q_UNUSED(AModifiedHtml);
	Q_UNUSED(AOptions);

	if (AOrder != MSHO_MESSAGEDISPLAYEDSYNC || AWidget==NULL)
		return false;

	// A chat window is being rendered: everything up to the newest received
	// message is about to be shown to the user.
	IMessageChatWindow *window = findChatWindow(AWidget);
	if (window!=NULL && isChatWindowDisplayed(window))
		schedulePublish(window->streamJid(),window->contactJid());
	return true;
}

QString MessageDisplayedSync::messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AOrder);
	Q_UNUSED(AWidget);
	Q_UNUSED(ASlotName);
	Q_UNUSED(ASlotInTemplate);
	Q_UNUSED(AOptions);
	return QString();
}

void MessageDisplayedSync::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	QString requestId = AStanza.id();

	if (FCatchUpRequests.contains(requestId))
	{
		Jid streamJid(FCatchUpRequests.take(requestId));
		if (AStanza.type()==STANZA_TYPE_ERROR)
			LOG_STRM_WARNING(AStreamJid,QString("Failed to retrieve the displayed states catch-up, id=%1").arg(requestId));
		else
		{
			QDomElement pubsub = AStanza.firstElement("pubsub",NS_PUBSUB);
			processDisplayedItems(streamJid,pubsub.firstChildElement("items"));
		}
		return;
	}

	if (!FPublishRequests.contains(requestId))
		return;

	PublishRequest request = FPublishRequests.take(requestId);
	if (!FChatStates.contains(request.chatKey))
		return;

	ChatState &state = FChatStates[request.chatKey];
	if (AStanza.type()==STANZA_TYPE_ERROR)
	{
		// Do not retry the same identifier: the next received message or the
		// next display of the chat will publish a newer state.
		state.failed = request.stanzaId;
		LOG_STRM_WARNING(AStreamJid,QString("Failed to publish displayed state, with=%1, id=%2").arg(state.contactJid.full(),request.stanzaId));
	}
	else
	{
		if (state.stanzaId==request.stanzaId)
			state.failed.clear();
		state.published = request.stanzaId;
		state.publishedTime = request.time;
		LOG_STRM_INFO(AStreamJid,QString("Displayed state published, with=%1, id=%2").arg(state.contactJid.full(),request.stanzaId));
		emit displayedStatePublished(state.streamJid,state.contactJid,request.stanzaId);
	}
	scheduleStateSave();
}

bool MessageDisplayedSync::processPEPEvent(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (!isEnabled() || !AStreamJid.isValid())
		return false;

	// XEP-0490 v1.1: only the events from the user's own bare JID may be
	// applied, otherwise a contact could reset our unread markers.
	Jid from = AStanza.fromJid();
	if (!from.isValid() || from.pBare()!=AStreamJid.pBare())
		return false;

	QDomElement event = AStanza.firstElement("event",NS_PUBSUB_EVENT);
	QDomElement items = event.firstChildElement("items");
	if (items.isNull() || items.attribute("node")!=NS_MESSAGE_DISPLAYED_SYNC)
		return false;

	processDisplayedItems(AStreamJid,items);
	return true;
}

void MessageDisplayedSync::processDisplayedItems(const Jid &AStreamJid, const QDomElement &AItems)
{
	if (AItems.isNull() || !isEnabled())
		return;

	for (QDomElement item = AItems.firstChildElement("item");
		!item.isNull(); item = item.nextSiblingElement("item"))
	{
		Jid contactJid(item.attribute("id"));
		QDomElement displayed = item.firstChildElement("displayed");
		if (!contactJid.isValid() || displayed.isNull()
			|| displayed.namespaceURI()!=NS_MESSAGE_DISPLAYED_SYNC)
			continue;

		QDomElement sidElem = displayed.firstChildElement("stanza-id");
		if (sidElem.isNull() || sidElem.namespaceURI()!=NS_STANZA_ID)
			continue;

		Jid by(sidElem.attribute("by"));
		if (!by.isValid() || by.pBare()!=AStreamJid.pBare())
			continue;

		applyDisplayedState(AStreamJid,contactJid,sidElem.attribute("id"));
	}
}

void MessageDisplayedSync::applyDisplayedState(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId)
{
	if (!isEnabled() || AStanzaId.isEmpty())
		return;

	loadState();

	Jid streamJid = AStreamJid.pBare();
	Jid contactJid = AContactJid.pBare();
	if (!streamJid.isValid() || !contactJid.isValid())
		return;

	// The state moves forward only: an older item from the node must not
	// restore the notifications which were already cleared.
	qint64 time = 0;
	for (QHash<QString,ChatState>::const_iterator it=FChatStates.constBegin(); it!=FChatStates.constEnd(); ++it)
	{
		if (it->streamJid==streamJid && it->contactJid==contactJid && it->stanzaId==AStanzaId)
		{
			time = it->time;
			break;
		}
	}
	if (time==0)
		time = QDateTime::currentSecsSinceEpoch();

	QString key = chatKey(streamJid,contactJid);
	ChatState &state = FChatStates[key];
	if (!state.received.isEmpty() && state.receivedTime>time)
		return;
	state.streamJid = streamJid;
	state.contactJid = contactJid;
	state.received = AStanzaId;
	state.receivedTime = time;
	scheduleStateSave();

	// The notification for every message up to this identifier is removed.
	// If the identifier is not known (the message arrived while offline), all
	// notifications of the chat are removed: the peer has seen them all.
	foreach(int messageId, FMessageProcessor->notifiedMessages())
	{
		Message message = FMessageProcessor->notifiedMessage(messageId);
		if (message.toJid().pBare()!=streamJid.pBare() || message.fromJid().pBare()!=contactJid.pBare())
			continue;
		if (message.data(MDR_MESSAGE_DIRECTION).toInt()!=IMessageProcessor::DirectionIn)
			continue;

		QDomElement sidElem = message.stanza().firstElement("stanza-id",NS_STANZA_ID);
		QString id = sidElem.attribute("id");
		bool shown = id.isEmpty() || id==AStanzaId || message.dateTime().toSecsSinceEpoch()<=time;
		if (shown)
			FMessageProcessor->removeMessageNotify(messageId);
	}

	LOG_STRM_INFO(streamJid,QString("Displayed state received, with=%1, id=%2").arg(contactJid.full(),AStanzaId));
	emit displayedStateReceived(streamJid,contactJid,AStanzaId);
}

Jid MessageDisplayedSync::activeStreamJid(const Jid &ABareJid) const
{
	if (FStreamManager!=NULL)
	{
		foreach(IXmppStream *stream, FStreamManager->xmppStreams())
		{
			if (stream->isOpen() && stream->streamJid().pBare()==ABareJid.pBare())
				return stream->streamJid();
		}
	}
	return Jid();
}

void MessageDisplayedSync::subscribeAndCatchUp(const Jid &AStreamJid)
{
	if (FStanzaProcessor==NULL || !AStreamJid.isValid())
		return;

	QString bare = AStreamJid.pBare();
	if (FSubscribedStreams.contains(bare))
		return;

	Jid streamJid = activeStreamJid(AStreamJid);
	if (!streamJid.isValid())
		return;
	FSubscribedStreams.insert(bare);

	// PEP subscription: the node sends its live events to all our resources.
	Stanza subscribeIq(STANZA_KIND_IQ);
	subscribeIq.setType(STANZA_TYPE_SET).setUniqueId();
	QDomElement subscribePubsub = subscribeIq.addElement("pubsub",NS_PUBSUB);
	QDomElement subscribe = subscribePubsub.appendChild(subscribeIq.createElement("subscribe")).toElement();
	subscribe.setAttribute("node",NS_MESSAGE_DISPLAYED_SYNC);
	subscribe.setAttribute("jid",streamJid.full());
	if (!FStanzaProcessor->sendStanzaOut(streamJid,subscribeIq))
		LOG_STRM_WARNING(streamJid,"Failed to subscribe to the displayed state node");

	// Catch-up: retrieve the items published while the client was offline.
	Stanza itemsIq(STANZA_KIND_IQ);
	itemsIq.setType(STANZA_TYPE_GET).setUniqueId();
	QDomElement itemsPubsub = itemsIq.addElement("pubsub",NS_PUBSUB);
	QDomElement items = itemsPubsub.appendChild(itemsIq.createElement("items")).toElement();
	items.setAttribute("node",NS_MESSAGE_DISPLAYED_SYNC);
	FCatchUpRequests.insert(itemsIq.id(),bare);
	if (!FStanzaProcessor->sendStanzaRequest(this,streamJid,itemsIq,DISPLAYED_STATE_REQUEST_TIMEOUT))
	{
		FCatchUpRequests.remove(itemsIq.id());
		LOG_STRM_WARNING(streamJid,"Failed to request the displayed states catch-up");
	}
}

bool MessageDisplayedSync::isEnabled() const
{
	return Options::node(OPV_MESSAGES_DISPLAYEDSYNCENABLED).value().toBool();
}

MessageDisplayedSync::DiscoSupport MessageDisplayedSync::publishOptionsSupport(const Jid &AStreamJid)
{
	if (FDiscovery==NULL || !AStreamJid.isValid())
		return DiscoUnsupported;

	// PEP is provided by the server for the account JID, not by the domain:
	// the pubsub#publish-options feature is advertised on the own bare JID.
	// The domain is checked as a fallback for the servers which publish the
	// feature there instead.
	// Remember a positive answer: the cached info for the own JID is later
	// overwritten with the client caps (which naturally lack the pubsub
	// feature), but the server capability itself does not disappear.
	if (FPublishOptionsStreams.contains(AStreamJid.pBare()))
		return DiscoSupported;

	Jid ownJid(AStreamJid.bare());
	IDiscoInfo info = FDiscovery->discoInfo(AStreamJid,ownJid);
	if (info.features.isEmpty())
	{
		IDiscoInfo domainInfo = FDiscovery->discoInfo(AStreamJid,AStreamJid.domain());
		if (!domainInfo.features.isEmpty())
			info = domainInfo;
	}
	if (info.features.isEmpty())
	{
		// The capabilities are not known yet: ask once for both entities and
		// publish only after the server confirms the required feature.
		QString key = AStreamJid.pBare();
		if (!FRequestedDisco.contains(key))
		{
			FRequestedDisco.insert(key);
			FDiscovery->requestDiscoInfo(AStreamJid,ownJid);
			FDiscovery->requestDiscoInfo(AStreamJid,AStreamJid.domain());
		}
		LOG_STRM_DEBUG(AStreamJid,QString("[DisplayedSync] PEP disco not loaded yet, publish-options support unknown"));
		return DiscoUnknown;
	}
	bool supported = info.features.contains(NS_PUBSUB_PUBLISH_OPTIONS);
	if (supported)
		FPublishOptionsStreams.insert(AStreamJid.pBare());
	LOG_STRM_DEBUG(AStreamJid,QString("[DisplayedSync] PEP disco features=%1, publish-options=%2")
		.arg(info.features.count()).arg(supported));
	return supported ? DiscoSupported : DiscoUnsupported;
}

void MessageDisplayedSync::recordStanzaId(const Jid &AStreamJid, const Jid &AContactJid, const QString &AStanzaId, const QDateTime &ATime)
{
	if (AStanzaId.isEmpty() || !AStreamJid.isValid() || !AContactJid.isValid())
		return;

	loadState();

	const qint64 time = (ATime.isValid() ? ATime : QDateTime::currentDateTime()).toSecsSinceEpoch();
	QString key = chatKey(AStreamJid,AContactJid);

	ChatState &state = FChatStates[key];
	state.streamJid = AStreamJid.pBare();
	state.contactJid = AContactJid.pBare();
	if (state.time<=time && state.stanzaId!=AStanzaId)
	{
		state.stanzaId = AStanzaId;
		state.time = time;
		FRetryAttempts.remove(key);
		LOG_STRM_DEBUG(AStreamJid,QString("Displayed state candidate, with=%1, id=%2").arg(state.contactJid.full(),AStanzaId));
		pruneState();
		scheduleStateSave();
	}
}

void MessageDisplayedSync::schedulePublish(const Jid &AStreamJid, const Jid &AContactJid)
{
	if (!isEnabled())
		return;

	// The state may only live in the file after a restart.
	loadState();

	QString key = chatKey(AStreamJid,AContactJid);
	if (!FChatStates.contains(key))
		return;

	LOG_STRM_DEBUG(AStreamJid,QString("[DisplayedSync] chat displayed, publish scheduled, with=%1, enabled=%2")
		.arg(AContactJid.full()).arg(isEnabled()));
	FPendingPublishes.insert(key);
	if (!FPublishTimer.isActive())
		FPublishTimer.start(DISPLAYED_STATE_PUBLISH_DELAY);
}

void MessageDisplayedSync::publishDisplayedState(const QString &AChatKey)
{
	loadState();

	ChatState state = FChatStates.value(AChatKey);
	if (state.stanzaId.isEmpty() || state.stanzaId==state.published || state.stanzaId==state.failed)
	{
		LOG_STRM_DEBUG(state.streamJid,QString("[DisplayedSync] nothing to publish for %1, has-id=%2, published=%3, failed=%4")
			.arg(state.contactJid.full()).arg(!state.stanzaId.isEmpty()).arg(state.published).arg(state.failed));
		return;
	}

	// The displayed state only moves forward.
	if (state.time<state.publishedTime)
		return;

	DiscoSupport support = publishOptionsSupport(state.streamJid);
	if (support==DiscoUnsupported)
	{
		LOG_STRM_INFO(state.streamJid,QString("[DisplayedSync] server does not support publish-options, skipping %1")
			.arg(state.contactJid.full()));
		return;
	}

	if (support==DiscoUnknown)
	{
		// The server capabilities may not be loaded yet, e.g. a chat window
		// restored before the stream is connected. Retry a few times.
		int &attempts = FRetryAttempts[AChatKey];
		if (attempts<5)
		{
			attempts++;
			FPendingPublishes.insert(AChatKey);
			FPublishTimer.start(5000);
		}
		return;
	}

	FRetryAttempts.remove(AChatKey);
	LOG_STRM_INFO(state.streamJid,QString("[DisplayedSync] publishing displayed state, with=%1, id=%2")
		.arg(state.contactJid.full()).arg(state.stanzaId));
	sendDisplayedState(AChatKey,state);
}

bool MessageDisplayedSync::sendDisplayedState(const QString &AChatKey, const ChatState &AState)
{
	if (FStanzaProcessor==NULL || !AState.streamJid.isValid())
		return false;

	// The saved state only has the bare account JID, but StanzaProcessor
	// resolves an active stream by its exact (full) JID.
	Jid streamJid = activeStreamJid(AState.streamJid);
	if (!streamJid.isValid())
		return false;

	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET).setUniqueId();

	QDomElement pubsub = iq.addElement("pubsub",NS_PUBSUB);

	QDomElement publish = pubsub.appendChild(iq.createElement("publish")).toElement();
	publish.setAttribute("node",NS_MESSAGE_DISPLAYED_SYNC);

	QDomElement item = iq.createElement("item");
	item.setAttribute("id",AState.contactJid.pBare());

	QDomElement displayed = iq.createElement("displayed");
	displayed.setAttribute("xmlns",NS_MESSAGE_DISPLAYED_SYNC);

	QDomElement sid = iq.createElement("stanza-id");
	sid.setAttribute("xmlns",NS_STANZA_ID);
	sid.setAttribute("by",AState.streamJid.pBare());
	sid.setAttribute("id",AState.stanzaId);

	displayed.appendChild(sid);
	item.appendChild(displayed);
	publish.appendChild(item);

	// XEP-0490 requires a private node: the read state must not be visible to
	// the contacts, so publish-options are mandatory and never optional.
	QDomElement publishOptions = pubsub.appendChild(iq.createElement("publish-options")).toElement();
	QDomElement form = publishOptions.appendChild(iq.createElement("x")).toElement();
	form.setAttribute("xmlns",NS_JABBER_DATA);
	form.setAttribute("type","submit");
	addPublishOptionField(iq,form,"FORM_TYPE","http://jabber.org/protocol/pubsub#publish-options",true);
	addPublishOptionField(iq,form,"pubsub#persist_items","true");
	addPublishOptionField(iq,form,"pubsub#max_items","max");
	addPublishOptionField(iq,form,"pubsub#send_last_published_item","never");
	addPublishOptionField(iq,form,"pubsub#access_model","whitelist");

	PublishRequest request;
	request.chatKey = AChatKey;
	request.stanzaId = AState.stanzaId;
	request.time = AState.time;
	FPublishRequests.insert(iq.id(),request);

	if (FStanzaProcessor->sendStanzaRequest(this,streamJid,iq,DISPLAYED_STATE_REQUEST_TIMEOUT))
	{
		LOG_STRM_DEBUG(streamJid,QString("Displayed state publish request sent, with=%1, id=%2").arg(AState.contactJid.full(),AState.stanzaId));
		return true;
	}

	FPublishRequests.remove(iq.id());
	LOG_STRM_WARNING(streamJid,QString("Failed to send displayed state publish request, with=%1").arg(AState.contactJid.full()));
	return false;
}

bool MessageDisplayedSync::isChatWindowDisplayed(IMessageChatWindow *AWindow) const
{
	QWidget *page = AWindow!=NULL ? AWindow->instance() : NULL;
	if (page==NULL || !page->isVisible())
		return false;

	// The page is either a window itself or the current tab of its parent.
	QWidget *top = page->window();
	return top!=NULL && top->isVisible() && !top->isMinimized();
}

IMessageChatWindow *MessageDisplayedSync::findChatWindow(QWidget *AStyleWidget) const
{
	if (FMessageWidgets!=NULL && AStyleWidget!=NULL)
	{
		foreach(IMessageChatWindow *window, FMessageWidgets->chatWindows())
		{
			if (window->viewWidget()!=NULL && window->viewWidget()->styleWidget()==AStyleWidget)
				return window;
		}
	}
	return NULL;
}

QString MessageDisplayedSync::chatKey(const Jid &AStreamJid, const Jid &AContactJid)
{
	return AStreamJid.pBare()+"\n"+AContactJid.pBare();
}

QString MessageDisplayedSync::stateFilePath() const
{
	QString dir = Options::filesPath();
	if (dir.isEmpty() && FPluginManager!=NULL)
		dir = FPluginManager->homePath();
	return dir.isEmpty() ? QString() : dir + "/messagedisplayedsync.state";
}

void MessageDisplayedSync::loadState()
{
	if (FStateLoaded)
		return;
	FStateLoaded = true;

	QFile file(stateFilePath());
	if (!file.open(QFile::ReadOnly))
		return;

	QJsonParseError error;
	QJsonDocument doc = QJsonDocument::fromJson(file.readAll(),&error);
	if (error.error!=QJsonParseError::NoError || !doc.isObject())
	{
		LOG_WARNING(QString("Failed to parse displayed state file: %1").arg(error.errorString()));
		return;
	}

	foreach(const QJsonValue &value, doc.object().value("chats").toArray())
	{
		QJsonObject object = value.toObject();
		ChatState state;
		state.streamJid = Jid(object.value("stream").toString()).pBare();
		state.contactJid = Jid(object.value("contact").toString()).pBare();
		state.stanzaId = object.value("stanza").toString();
		state.time = (qint64)object.value("time").toDouble();
		state.published = object.value("published").toString();
		state.publishedTime = (qint64)object.value("publishedTime").toDouble();
		state.failed = object.value("failed").toString();
		state.received = object.value("received").toString();
		state.receivedTime = (qint64)object.value("receivedTime").toDouble();
		if (state.streamJid.isValid() && state.contactJid.isValid())
			FChatStates.insert(chatKey(state.streamJid,state.contactJid),state);
	}
}

void MessageDisplayedSync::scheduleStateSave()
{
	if (!FSaveTimer.isActive())
		FSaveTimer.start(DISPLAYED_STATE_SAVE_DELAY);
}

void MessageDisplayedSync::saveState() const
{
	QString filePath = stateFilePath();
	if (filePath.isEmpty())
		return;

	QJsonArray chats;
	for (QHash<QString, ChatState>::const_iterator it=FChatStates.constBegin(); it!=FChatStates.constEnd(); ++it)
	{
		QJsonObject object;
		object.insert("stream",it->streamJid.pBare());
		object.insert("contact",it->contactJid.pBare());
		object.insert("stanza",it->stanzaId);
		object.insert("time",(double)it->time);
		object.insert("published",it->published);
		object.insert("publishedTime",(double)it->publishedTime);
		object.insert("failed",it->failed);
		object.insert("received",it->received);
		object.insert("receivedTime",(double)it->receivedTime);
		chats.append(object);
	}

	QJsonObject root;
	root.insert("version",1);
	root.insert("chats",chats);

	QSaveFile file(filePath);
	if (file.open(QFile::WriteOnly))
	{
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
		if (!file.commit())
			LOG_WARNING(QString("Failed to save displayed state to file=%1: %2").arg(filePath,file.errorString()));
	}
	else
	{
		LOG_WARNING(QString("Failed to open displayed state file=%1: %2").arg(filePath,file.errorString()));
	}
}

void MessageDisplayedSync::pruneState()
{
	if (FChatStates.count() <= DISPLAYED_STATE_MAX_CHATS)
		return;

	QList<QPair<qint64, QString> > sorted;
	for (QHash<QString, ChatState>::const_iterator it=FChatStates.constBegin(); it!=FChatStates.constEnd(); ++it)
		sorted.append(qMakePair(qMax(it->time,it->publishedTime),it.key()));
	std::sort(sorted.begin(),sorted.end());

	int removeCount = FChatStates.count()-DISPLAYED_STATE_MAX_CHATS;
	for (int i=0; i<removeCount; i++)
		FChatStates.remove(sorted.at(i).second);
}

void MessageDisplayedSync::onChatWindowCreated(IMessageChatWindow *AWindow)
{
	connect(AWindow->instance(),SIGNAL(tabPageActivated()),SLOT(onChatWindowActivated()));
}

void MessageDisplayedSync::onChatWindowActivated()
{
	IMessageChatWindow *window = qobject_cast<IMessageChatWindow *>(sender());
	if (window!=NULL && isChatWindowDisplayed(window))
		schedulePublish(window->streamJid(),window->contactJid());
}

void MessageDisplayedSync::onTabWindowCreated(IMessageTabWindow *AWindow)
{
	connect(AWindow->instance(),SIGNAL(currentTabPageChanged(IMessageTabPage *)),SLOT(onCurrentTabPageChanged(IMessageTabPage *)));
}

void MessageDisplayedSync::onCurrentTabPageChanged(IMessageTabPage *APage)
{
	IMessageChatWindow *window = APage!=NULL ? qobject_cast<IMessageChatWindow *>(APage->instance()) : NULL;
	if (window!=NULL && isChatWindowDisplayed(window))
		schedulePublish(window->streamJid(),window->contactJid());
}

void MessageDisplayedSync::onCollectionLoaded(const QString &AId, const IArchiveCollection &ACollection)
{
	Q_UNUSED(AId);

	if (!isEnabled())
		return;

	Jid contactJid = ACollection.header.with.pBare();
	if (!contactJid.isValid())
		return;

	// The stanza ids of the server archive history are captured here, so the
	// chats that were read while the client was offline can be synchronized
	// without waiting for a new live message.
	foreach(const Message &message, ACollection.body.messages)
	{
		if (message.type()!=Message::Chat)
			continue;

		QDomElement sidElem = message.stanza().firstElement("stanza-id",NS_STANZA_ID);
		if (sidElem.isNull() || sidElem.attribute("id").isEmpty())
			continue;

		Jid sidBy(sidElem.attribute("by"));
		if (!sidBy.isValid())
			continue;

		Jid streamJid = sidBy.pBare();
		if (message.fromJid().pBare()==streamJid.pBare())
			continue;

		recordStanzaId(streamJid,contactJid,sidElem.attribute("id"),message.dateTime());
	}
}

void MessageDisplayedSync::onDiscoInfoReceived(const IDiscoInfo &AInfo)
{
	if (!AInfo.streamJid.isValid() || !AInfo.node.isEmpty())
		return;

	bool isSelf = AInfo.contactJid.pBare()==AInfo.streamJid.pBare();
	bool isServer = AInfo.contactJid.pBare()==AInfo.streamJid.domain();
	if (!isSelf && !isServer)
		return;

	bool supported = AInfo.features.contains(NS_PUBSUB_PUBLISH_OPTIONS);
	LOG_STRM_DEBUG(AInfo.streamJid,QString("[DisplayedSync] %1 caps received, jid=%2, features=%3, publish-options=%4")
		.arg(isSelf ? "PEP" : "server")
		.arg(AInfo.contactJid.full())
		.arg(AInfo.features.count())
		.arg(supported));

	if (supported && isSelf)
		FPublishOptionsStreams.insert(AInfo.streamJid.pBare());

	// The server capabilities are the gate for publishing. Once they arrive
	// the postponed publishes can be tried again immediately.
	bool hasPending = false;
	foreach(const QString &key, FPendingPublishes)
	{
		ChatState state = FChatStates.value(key);
		if (state.streamJid.pBare()==AInfo.streamJid.pBare())
		{
			FRetryAttempts.remove(key);
			hasPending = true;
		}
	}
	if (hasPending && !FPublishTimer.isActive())
		FPublishTimer.start(DISPLAYED_STATE_PUBLISH_DELAY);
}

void MessageDisplayedSync::onStreamOpened(IXmppStream *AStream)
{
	if (AStream!=NULL)
		subscribeAndCatchUp(AStream->streamJid());
}

void MessageDisplayedSync::onStreamClosed(IXmppStream *AStream)
{
	if (AStream!=NULL)
	{
		FSubscribedStreams.remove(AStream->streamJid().pBare());
		FSubscribedStreams.remove(AStream->streamJid().bare());
	}
}

void MessageDisplayedSync::onPublishTimeout()
{
	QSet<QString> pending = FPendingPublishes;
	FPendingPublishes.clear();

	LOG_DEBUG(QString("[DisplayedSync] publish timer fired, chats=%1").arg(pending.count()));

	foreach(const QString &key, pending)
	{
		ChatState state = FChatStates.value(key);
		if (!state.streamJid.isValid())
			continue;

		// The chat was already confirmed as displayed when it was scheduled.
		// Re-checking the window here drops the state if the user switched
		// tabs during the short delay, even though the messages were read.
		publishDisplayedState(key);
	}
}

void MessageDisplayedSync::onStateSaveTimeout()
{
	saveState();
}
