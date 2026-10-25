#include <utils/qtcompat.h>
#include "mamarchive.h"

#include <algorithm>
#include <QDomDocument>
#include <definitions/namespaces.h>
#include <definitions/stanzahandlerorders.h>
#include <definitions/archivecapabilityorders.h>
#include <definitions/archivehandlerorders.h>
#include <definitions/messagedataroles.h>
#include <interfaces/imessageprocessor.h>
#include <utils/datetime.h>
#include <utils/logger.h>

// Maximum number of messages requested from the server in a single MAM query.
// Prosody (and most servers) cap this internally (max_archive_query_results).
#define MAM_RESULT_PAGE           50
// Safety limit for the number of pages fetched to satisfy a single request.
#define MAM_MAX_PAGES             10
#define MAM_MAX_MESSAGES          (MAM_RESULT_PAGE*MAM_MAX_PAGES)
// Gap between two messages that starts a new conversation (matches the local
// file archive collection timeout used by ChatMessageHandler).
#define MAM_SESSION_TIMEOUT       (20*60)
// Maximum number of cached conversation bodies kept in memory.
#define MAM_MAX_CACHE             300
// Maximum number of session end timestamps kept in memory.
#define MAM_MAX_SESSION_ENDS      1000

#define MAM_QUERY_TIMEOUT         30000

#define SHC_MAM_RESULT_2          "/message/result[@xmlns='" NS_MAM "']"
#define SHC_MAM_RESULT_1          "/message/result[@xmlns='" NS_MAM_1 "']"
#define SHC_MAM_RESULT_0          "/message/result[@xmlns='" NS_MAM_0 "']"

static void appendFormField(QDomDocument &ADoc, QDomElement &AX, const QString &AVar, const QString &AType, const QString &AValue)
{
	QDomElement field = ADoc.createElement("field");
	field.setAttribute("var",AVar);
	if (!AType.isEmpty())
		field.setAttribute("type",AType);
	QDomElement value = ADoc.createElement("value");
	value.appendChild(ADoc.createTextNode(AValue));
	field.appendChild(value);
	AX.appendChild(field);
}

MamArchive::MamArchive()
{
	FArchiver = NULL;
	FStanzaProcessor = NULL;
	FDiscovery = NULL;
	FXmppStreamManager = NULL;
	FAccountManager = NULL;

	FDeferredTimer.setSingleShot(true);
	connect(&FDeferredTimer,SIGNAL(timeout()),SLOT(onDeferredTimerTimeout()));
}

MamArchive::~MamArchive()
{
}

void MamArchive::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Message Archive Management");
	APluginInfo->description = tr("Retrieves message history from the server using XEP-0313");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(MESSAGEARCHIVER_UUID));
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(SERVICEDISCOVERY_UUID));
	APluginInfo->dependences.append(QUuid(XMPPSTREAMS_UUID));
	APluginInfo->dependences.append(QUuid(ACCOUNTMANAGER_UUID));
}

bool MamArchive::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);
	IPlugin *plugin = APluginManager->pluginInterface("IMessageArchiver").value(0,NULL);
	if (plugin)
	{
		FArchiver = qobject_cast<IMessageArchiver *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IStanzaProcessor").value(0,NULL);
	if (plugin)
	{
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IAccountManager").value(0,NULL);
	if (plugin)
	{
		FAccountManager = qobject_cast<IAccountManager *>(plugin->instance());
	}

	plugin = APluginManager->pluginInterface("IServiceDiscovery").value(0,NULL);
	if (plugin)
	{
		FDiscovery = qobject_cast<IServiceDiscovery *>(plugin->instance());
		if (FDiscovery)
			connect(FDiscovery->instance(),SIGNAL(discoInfoReceived(const IDiscoInfo &)),SLOT(onDiscoInfoReceived(const IDiscoInfo &)));
	}

	plugin = APluginManager->pluginInterface("IXmppStreamManager").value(0,NULL);
	if (plugin)
	{
		FXmppStreamManager = qobject_cast<IXmppStreamManager *>(plugin->instance());
		if (FXmppStreamManager)
		{
			connect(FXmppStreamManager->instance(),SIGNAL(streamOpened(IXmppStream *)),SLOT(onXmppStreamOpened(IXmppStream *)));
			connect(FXmppStreamManager->instance(),SIGNAL(streamClosed(IXmppStream *)),SLOT(onXmppStreamClosed(IXmppStream *)));
		}
	}

	return FArchiver!=NULL && FStanzaProcessor!=NULL && FDiscovery!=NULL && FXmppStreamManager!=NULL && FAccountManager!=NULL;
}

bool MamArchive::initObjects()
{
	if (FArchiver)
	{
		FArchiver->registerArchiveEngine(this);
		// When the server archives everything via MAM there is no need to write
		// the same messages into the local file archive: the archives overlap
		// and the chat windows get every message in a double copy.
		FArchiver->insertArchiveHandler(AHO_DEFAULT,this);
	}
	return true;
}

bool MamArchive::archiveMessageEdit(int AOrder, const Jid &AStreamJid, Message &AMessage, bool ADirectionIn)
{
	Q_UNUSED(AOrder); Q_UNUSED(AMessage); Q_UNUSED(ADirectionIn);
	// Dropping the message tells MessageArchiver to not save it locally.
	return isServerMamActive(AStreamJid) && !isArchiveDuplicationEnabled(AStreamJid);
}

bool MamArchive::stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept)
{
	if (FSHIResults.value(AStreamJid) == AHandleId)
	{
		QDomElement result = AStanza.firstElement("result",NS_MAM);
		if (result.isNull())
			result = AStanza.firstElement("result",NS_MAM_1);
		if (result.isNull())
			result = AStanza.firstElement("result",NS_MAM_0);
		if (!result.isNull())
		{
			AAccept = true;
			appendStanzaResult(AStreamJid,result);
			return true;
		}
	}
	return false;
}

void MamArchive::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (FQueries.contains(AStanza.id()))
		processQueryResult(AStreamJid,AStanza);
}

QUuid MamArchive::engineId() const
{
	return QUuid(MAMARCHIVE_UUID);
}

QString MamArchive::engineName() const
{
	return tr("Server Message Archive (MAM)");
}

QString MamArchive::engineDescription() const
{
	return tr("Message history is stored on the server and synchronized using XEP-0313");
}

IOptionsDialogWidget *MamArchive::engineSettingsWidget(QWidget *AParent)
{
	Q_UNUSED(AParent);
	return NULL;
}

quint32 MamArchive::capabilities(const Jid &AStreamJid) const
{
	quint32 caps = 0;
	if (!mamNamespace(AStreamJid).isEmpty())
		caps |= ArchiveManagement;
	return caps;
}

bool MamArchive::isCapable(const Jid &AStreamJid, quint32 ACapability) const
{
	return (capabilities(AStreamJid) & ACapability) == ACapability;
}

int MamArchive::capabilityOrder(quint32 ACapability, const Jid &AStreamJid) const
{
	if (ACapability==ArchiveManagement && isCapable(AStreamJid,ACapability))
		return ACO_MANAGE_MAMARCHIVE;
	return -1;
}

bool MamArchive::saveMessage(const Jid &AStreamJid, const Message &AMessage, bool ADirectionIn)
{
	Q_UNUSED(AStreamJid); Q_UNUSED(AMessage); Q_UNUSED(ADirectionIn);
	return false;
}

bool MamArchive::saveNote(const Jid &AStreamJid, const Message &AMessage, bool ADirectionIn)
{
	Q_UNUSED(AStreamJid); Q_UNUSED(AMessage); Q_UNUSED(ADirectionIn);
	return false;
}

QString MamArchive::saveCollection(const Jid &AStreamJid, const IArchiveCollection &ACollection)
{
	Q_UNUSED(AStreamJid); Q_UNUSED(ACollection);
	return QString();
}

QString MamArchive::loadHeaders(const Jid &AStreamJid, const IArchiveRequest &ARequest)
{
	QString ns = mamNamespace(AStreamJid);
	if (ns.isEmpty())
		return QString();

	MamQueryRequest request;
	request.id = QUuid::createUuid().toString();
	request.streamJid = AStreamJid;
	request.namespaceUri = ns;
	request.headers = true;
	request.request = ARequest;
	request.maxPages = ARequest.maxItems <= MAM_RESULT_PAGE ? 1 : MAM_MAX_PAGES;

	if (startIq(request).isEmpty())
	{
		LOG_STRM_WARNING(AStreamJid,"Failed to send MAM headers request");
		return QString();
	}

	LOG_STRM_INFO(AStreamJid,QString("MAM headers request sent, with=%1, id=%2").arg(ARequest.with.bare(),request.id));
	return request.id;
}

QString MamArchive::loadCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader)
{
	QString ns = mamNamespace(AStreamJid);
	if (ns.isEmpty())
		return QString();

	QString id = QUuid::createUuid().toString();

	QList<Message> messages;
	QDateTime end;
	QString key = collectionKey(AStreamJid,AHeader);
	if (findCachedCollection(AStreamJid,AHeader,messages,end))
	{
		IArchiveCollection collection;
		collection.header = AHeader;
		collection.header.engineId = engineId();
		collection.body.messages = messages;
		emitCollectionDeferred(id,collection);
		return id;
	}
	else
	{
		// The messages have been evicted or were fetched in another session.
		// The session end date recorded during groupHeaders still bounds the
		// query, otherwise the archive would return all messages starting with
		// AHeader.start and the later sessions would be loaded more than once.
		end = FSessionEnds.value(key);
	}

	MamQueryRequest request;
	request.id = id;
	request.streamJid = AStreamJid;
	request.namespaceUri = ns;
	request.headers = false;
	request.header = AHeader;
	request.header.engineId = engineId();
	request.request.with = AHeader.with;
	request.request.start = AHeader.start;
	request.request.end = end;
	request.request.order = Qt::AscendingOrder;
	request.maxPages = MAM_MAX_PAGES;

	if (startIq(request).isEmpty())
	{
		LOG_STRM_WARNING(AStreamJid,"Failed to send MAM collection request");
		return QString();
	}

	LOG_STRM_INFO(AStreamJid,QString("MAM collection request sent, with=%1, id=%2").arg(AHeader.with.bare(),id));
	return id;
}

QString MamArchive::removeCollections(const Jid &AStreamJid, const IArchiveRequest &ARequest)
{
	Q_UNUSED(AStreamJid); Q_UNUSED(ARequest);
	return QString();
}

QString MamArchive::loadModifications(const Jid &AStreamJid, const QDateTime &AStart, int ACount, const QString &ANextRef)
{
	Q_UNUSED(AStreamJid); Q_UNUSED(AStart); Q_UNUSED(ACount); Q_UNUSED(ANextRef);
	return QString();
}

QString MamArchive::mamNamespace(const Jid &AStreamJid) const
{
	if (FDiscovery!=NULL && AStreamJid.isValid())
	{
		QStringList entities;
		entities << AStreamJid.bare();
		entities << AStreamJid.domain();
		foreach(const QString &entity, entities)
		{
			IDiscoInfo info = FDiscovery->discoInfo(AStreamJid,Jid(entity));
			if (info.features.contains(NS_MAM))
				return NS_MAM;
			if (info.features.contains(NS_MAM_1))
				return NS_MAM_1;
			if (info.features.contains(NS_MAM_0))
				return NS_MAM_0;
		}
	}
	return QString();
}

bool MamArchive::isServerMamActive(const Jid &AStreamJid) const
{
	return !mamNamespace(AStreamJid).isEmpty() && (FArchiver==NULL || FArchiver->isArchiveEngineEnabled(engineId()));
}

bool MamArchive::isArchiveDuplicationEnabled(const Jid &AStreamJid) const
{
	IAccount *account = FAccountManager!=NULL ? FAccountManager->findAccountByStream(AStreamJid) : NULL;
	return account!=NULL ? account->optionsNode().value("history-duplicate").toBool() : false;
}

QString MamArchive::startIq(MamQueryRequest &ARequest)
{
	if (FStanzaProcessor==NULL || ARequest.namespaceUri.isEmpty())
		return QString();

	bool descending = (ARequest.request.order == Qt::DescendingOrder);

	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET).setUniqueId().setTo(ARequest.streamJid.bare());
	QDomDocument doc = iq.document();

	QDomElement query = iq.addElement("query",ARequest.namespaceUri);
	query.setAttribute("queryid",iq.id());

	QDomElement x = doc.createElementNS(NS_JABBER_DATA,"x");
	x.setAttribute("type","submit");
	query.appendChild(x);

	appendFormField(doc,x,"FORM_TYPE","hidden",ARequest.namespaceUri);
	if (ARequest.request.with.isValid())
		appendFormField(doc,x,"with",QString(),ARequest.request.with.bare());
	if (ARequest.request.start.isValid())
		appendFormField(doc,x,"start",QString(),DateTime(ARequest.request.start).toX85UTC());
	if (ARequest.request.end.isValid())
		appendFormField(doc,x,"end",QString(),DateTime(ARequest.request.end).toX85UTC());

	QDomElement set = doc.createElementNS(NS_RESULTSET,"set");
	QDomElement maxElem = doc.createElement("max");
	maxElem.appendChild(doc.createTextNode(QString::number(MAM_RESULT_PAGE)));
	set.appendChild(maxElem);

	if (ARequest.pages == 0)
	{
		if (descending)
			set.appendChild(doc.createElement("before"));
	}
	else if (descending && !ARequest.first.isEmpty())
	{
		QDomElement before = doc.createElement("before");
		before.appendChild(doc.createTextNode(ARequest.first));
		set.appendChild(before);
	}
	else if (!descending && !ARequest.last.isEmpty())
	{
		QDomElement after = doc.createElement("after");
		after.appendChild(doc.createTextNode(ARequest.last));
		set.appendChild(after);
	}
	query.appendChild(set);

	if (FStanzaProcessor->sendStanzaRequest(this,ARequest.streamJid,iq,MAM_QUERY_TIMEOUT))
	{
		ARequest.iqId = iq.id();
		ARequest.pages++;
		FQueries.insert(iq.id(),ARequest);
		return iq.id();
	}
	return QString();
}

void MamArchive::processQueryResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	MamQueryRequest request = FQueries.take(AStanza.id());

	if (AStanza.isError())
	{
		XmppStanzaError err(AStanza);
		LOG_STRM_WARNING(AStreamJid,QString("MAM request failed, id=%1: %2").arg(request.id,err.condition()));
		finishQuery(request,err);
		return;
	}

	bool more = false;
	QDomElement fin = AStanza.firstElement("fin",request.namespaceUri);
	if (!fin.isNull())
	{
		request.complete = fin.attribute("complete")=="true";
		QDomElement set = fin.firstChildElement("set");
		request.first = set.firstChildElement("first").text();
		request.last = set.firstChildElement("last").text();
		more = !request.complete;
	}

	bool limitReached = (request.messages.count() >= MAM_MAX_MESSAGES) || (request.pages >= request.maxPages);
	bool canPage = request.request.order==Qt::DescendingOrder ? !request.first.isEmpty() : !request.last.isEmpty();
	if (more && !limitReached && canPage)
	{
		if (!startIq(request).isEmpty())
			return;
	}

	LOG_STRM_INFO(AStreamJid,QString("MAM request completed, with=%1, messages=%2, id=%3")
		.arg(request.request.with.bare()).arg(request.messages.count()).arg(request.id));
	finishQuery(request);
}

void MamArchive::finishQuery(MamQueryRequest &ARequest, const XmppError &AError)
{
	if (!AError.isNull())
	{
		emit requestFailed(ARequest.id,AError);
		return;
	}

	if (ARequest.headers)
	{
		QList<IArchiveHeader> headers = groupHeaders(ARequest.streamJid,ARequest.messages);
		LOG_STRM_INFO(ARequest.streamJid,QString("MAM headers prepared, with=%1, count=%2, id=%3")
			.arg(ARequest.request.with.bare()).arg(headers.count()).arg(ARequest.id));
		emit headersLoaded(ARequest.id,headers);
	}
	else
	{
		IArchiveCollection collection;
		collection.header = ARequest.header;
		collection.header.engineId = engineId();
		collection.body.messages = ARequest.messages;
		LOG_STRM_INFO(ARequest.streamJid,QString("MAM collection prepared, with=%1, messages=%2, id=%3")
			.arg(ARequest.header.with.bare()).arg(ARequest.messages.count()).arg(ARequest.id));
		emit collectionLoaded(ARequest.id,collection);
	}
}

void MamArchive::appendStanzaResult(const Jid &AStreamJid, const QDomElement &AResultElem)
{
	QString queryId = AResultElem.attribute("queryid");
	if (FQueries.contains(queryId))
	{
		QDomElement forwarded = Stanza::findElement(AResultElem,"forwarded",NS_MESSAGE_FORWARD);
		QDomElement messageElem = forwarded.firstChildElement("message");
		if (!messageElem.isNull())
			FQueries[queryId].messages.append(resultToMessage(AStreamJid,forwarded,messageElem));
	}
}

Message MamArchive::resultToMessage(const Jid &AStreamJid, const QDomElement &AForwarded, const QDomElement &AMessageElem) const
{
	Stanza stanza(AMessageElem);
	Message message(stanza);

	QDomElement delay = Stanza::findElement(AForwarded,"delay",NS_XMPP_DELAY);
	QDateTime dateTime = DateTime(delay.attribute("stamp")).toLocal();
	if (dateTime.isValid())
		message.setDateTime(dateTime);

	int direction = message.fromJid().isBareEqual(AStreamJid) ? IMessageProcessor::DirectionOut : IMessageProcessor::DirectionIn;
	message.setData(MDR_MESSAGE_DIRECTION,direction);

	return message;
}

Jid MamArchive::messagePartner(const Jid &AStreamJid, const Message &AMessage) const
{
	Q_UNUSED(AStreamJid);
	if (AMessage.data(MDR_MESSAGE_DIRECTION).toInt() == IMessageProcessor::DirectionOut)
		return Jid(AMessage.toJid().bare());
	return Jid(AMessage.fromJid().bare());
}

QList<IArchiveHeader> MamArchive::groupHeaders(const Jid &AStreamJid, const QList<Message> &AMessages)
{
	QList<Message> sorted = AMessages;
	std::sort(sorted.begin(),sorted.end(),qLess<Message>());

	// Some servers return the boundary results of a page twice when the query
	// is paged with the rsm item id, drop the copies before grouping.
	QList<Message> unique;
	for (int i=0; i<sorted.count(); ++i)
	{
		const Message &message = sorted.at(i);
		bool encrypted = !message.stanza().firstElement(QStringLiteral("encrypted")).isNull()
			|| (!unique.isEmpty() && !unique.last().stanza().firstElement(QStringLiteral("encrypted")).isNull());
		bool sameId = !unique.isEmpty() && !unique.last().id().isEmpty()
			&& unique.last().id()==message.id();
		bool differentIds = !unique.isEmpty() && !unique.last().id().isEmpty()
			&& !message.id().isEmpty() && unique.last().id()!=message.id();
		bool duplicate = !differentIds && !unique.isEmpty()
			&& unique.last().body()==message.body()
			&& unique.last().data(MDR_MESSAGE_DIRECTION)==message.data(MDR_MESSAGE_DIRECTION)
			&& (sameId || (!encrypted && qAbs(unique.last().dateTime().secsTo(message.dateTime()))<2));
		if (!duplicate)
			unique.append(message);
	}

	QList<IArchiveHeader> headers;
	QList<Message> session;

	for (int i=0; i<unique.count(); ++i)
	{
		const Message &message = unique.at(i);
		if (!session.isEmpty())
		{
			bool newConversation = messagePartner(AStreamJid,message) != messagePartner(AStreamJid,session.last());
			bool newSession = session.last().dateTime().secsTo(message.dateTime()) > MAM_SESSION_TIMEOUT;
			if (newConversation || newSession)
			{
				IArchiveHeader header;
				header.engineId = engineId();
				header.with = messagePartner(AStreamJid,session.first());
				header.start = session.first().dateTime();
				headers.append(header);
				cacheCollection(AStreamJid,header,session,session.last().dateTime());
				session.clear();
			}
		}
		session.append(message);
	}

	if (!session.isEmpty())
	{
		IArchiveHeader header;
		header.engineId = engineId();
		header.with = messagePartner(AStreamJid,session.first());
		header.start = session.first().dateTime();
		headers.append(header);
		cacheCollection(AStreamJid,header,session,session.last().dateTime());
	}

	return headers;
}

void MamArchive::cacheCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader, const QList<Message> &AMessages, const QDateTime &AEnd)
{
	QString key = collectionKey(AStreamJid,AHeader);
	if (!FCollections.contains(key))
		FCollectionsOrder.append(key);

	MamCachedCollection cached;
	cached.messages = AMessages;
	cached.end = AEnd;
	FCollections.insert(key,cached);

	while (FCollectionsOrder.count() > MAM_MAX_CACHE)
		FCollections.remove(FCollectionsOrder.takeFirst());

	// The end date is small enough to be kept even after the messages of the
	// session were evicted.
	if (!FSessionEnds.contains(key))
		FSessionEndsOrder.append(key);
	FSessionEnds.insert(key,AEnd);

	while (FSessionEndsOrder.count() > MAM_MAX_SESSION_ENDS)
		FSessionEnds.remove(FSessionEndsOrder.takeFirst());
}

bool MamArchive::findCachedCollection(const Jid &AStreamJid, const IArchiveHeader &AHeader, QList<Message> &AMessages, QDateTime &AEnd) const
{
	QString key = collectionKey(AStreamJid,AHeader);
	QMap<QString,MamCachedCollection>::const_iterator it = FCollections.constFind(key);
	if (it != FCollections.constEnd())
	{
		AMessages = it->messages;
		AEnd = it->end;
		return true;
	}
	return false;
}

QString MamArchive::collectionKey(const Jid &AStreamJid, const IArchiveHeader &AHeader) const
{
	return AStreamJid.pBare() + "\n" + AHeader.with.pBare() + "\n" + QString::number(AHeader.start.toMSecsSinceEpoch());
}

void MamArchive::emitCollectionDeferred(const QString &AId, const IArchiveCollection &ACollection)
{
	FDeferredCollections.append(qMakePair(AId,ACollection));
	FDeferredTimer.start(0);
}

void MamArchive::onDeferredTimerTimeout()
{
	QList<QPair<QString,IArchiveCollection> > deferred = FDeferredCollections;
	FDeferredCollections.clear();
	for (int i=0; i<deferred.count(); ++i)
		emit collectionLoaded(deferred.at(i).first,deferred.at(i).second);
}

void MamArchive::onXmppStreamOpened(IXmppStream *AXmppStream)
{
	if (FStanzaProcessor)
	{
		// A stream can be opened again after a reconnection, so drop any state
		// left from the previous session and detect server archive support anew.
		if (FSHIResults.contains(AXmppStream->streamJid()))
			FStanzaProcessor->removeStanzaHandle(FSHIResults.take(AXmppStream->streamJid()));
		FSupported.remove(AXmppStream->streamJid());
		FAccountDiscoRequested.remove(AXmppStream->streamJid());

		IStanzaHandle shandle;
		shandle.handler = this;
		shandle.order = SHO_DEFAULT;
		shandle.direction = IStanzaHandle::DirectionIn;
		shandle.streamJid = AXmppStream->streamJid();
		shandle.conditions.append(SHC_MAM_RESULT_2);
		shandle.conditions.append(SHC_MAM_RESULT_1);
		shandle.conditions.append(SHC_MAM_RESULT_0);
		FSHIResults.insert(shandle.streamJid,FStanzaProcessor->insertStanzaHandle(shandle));
	}
}

void MamArchive::onXmppStreamClosed(IXmppStream *AXmppStream)
{
	if (FStanzaProcessor)
		FStanzaProcessor->removeStanzaHandle(FSHIResults.take(AXmppStream->streamJid()));

	FSupported.remove(AXmppStream->streamJid());
	FAccountDiscoRequested.remove(AXmppStream->streamJid());
}

void MamArchive::onDiscoInfoReceived(const IDiscoInfo &AInfo)
{
	if (!AInfo.node.isEmpty())
		return;

	bool isServer = (AInfo.contactJid == AInfo.streamJid.domain());
	bool isAccount = (AInfo.contactJid == Jid(AInfo.streamJid.bare()));
	if (!isServer && !isAccount)
		return;

	// XEP-0313 advertises MAM on the user's own account JID (Prosody uses the
	// "account-disco-info" hook), but nothing else in the client queries it, so
	// request it here. This is done from the first disco response rather than
	// from onXmppStreamOpened because service discovery is not ready yet there.
	if (FDiscovery && !FAccountDiscoRequested.contains(AInfo.streamJid))
	{
		FAccountDiscoRequested.insert(AInfo.streamJid);
		FDiscovery->requestDiscoInfo(AInfo.streamJid,Jid(AInfo.streamJid.bare()));
	}

	// A server may also advertise MAM on its domain JID, so evaluate there only
	// when that is the case; otherwise wait for the account JID response.
	bool evaluate = isAccount || !mamNamespace(AInfo.streamJid).isEmpty();
	if (evaluate)
	{
		QString ns = mamNamespace(AInfo.streamJid);
		bool supported = !ns.isEmpty();
		if (!FSupported.contains(AInfo.streamJid) || FSupported.value(AInfo.streamJid)!=supported)
		{
			FSupported.insert(AInfo.streamJid,supported);
			if (supported)
				LOG_STRM_INFO(AInfo.streamJid,QString("Message Archive Management is supported by server, ns=%1").arg(ns));
			else
				LOG_STRM_WARNING(AInfo.streamJid,"Message Archive Management is not supported by server");
			emit capabilitiesChanged(AInfo.streamJid);
		}
	}
}
