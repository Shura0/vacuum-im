#include "omemo.h"

#include <definitions/actiongroups.h>
#include <definitions/menuicons.h>
#include <definitions/messagemarks.h>
#include <definitions/messageeditororders.h>
#include <definitions/messagestylehandlerorders.h>
#include <definitions/messageviewurlhandlerorders.h>
#include <definitions/namespaces.h>
#include <definitions/messagedataroles.h>
#include <definitions/resources.h>
#include <definitions/stanzahandlerorders.h>
#include <definitions/toolbargroups.h>
#include <utils/iconstorage.h>
#include <utils/logger.h>
#include <utils/message.h>
#include <utils/options.h>
#include <utils/textmanager.h>

#include <QColor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFile>
#include <QFontMetricsF>
#include <QRegularExpression>
#include <QUrl>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QCryptographicHash>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextOption>

#include "omemodefinitions.h"
#include "omemodevicesdialog.h"
#include "omemostore.h"

#include <QDateTime>

#define SHC_OMEMO_ENCRYPTED "/message/encrypted[@xmlns='" OMEMO_NS "']"
#define SHC_OMEMO_MESSAGE   "/message"

// Message anchors whose href is a sanitized XEP-0454 attachment url.
static const QRegularExpression AnchorsRegex(QStringLiteral(
	R"REGEX(<a\b(?:(?!>).)*?(?<![\w-])href\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'`=<>]+))(?:(?!>).)*?>.*?</a\s*>)REGEX"),
	QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

static QIcon statusIcon(const QIcon &AIcon, const QColor &AColor)
{
	QPixmap pixmap = AIcon.pixmap(16,16);
	if (pixmap.isNull())
		return AIcon;
	QPainter painter(&pixmap);
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(pixmap.rect(),AColor);
	painter.end();
	return QIcon(pixmap);
}

static bool isChatMessage(const Message &AMessage)
{
	return (AMessage.type()==Message::Chat || AMessage.type()==Message::Normal)
		&& !AMessage.body().isEmpty();
}

static QString omemoPayloadKey(const QDomElement &AEncrypted)
{
	const QDomElement payload = AEncrypted.firstChildElement(QStringLiteral("payload"));
	if (payload.isNull())
		return QString();
	const QByteArray data = payload.text().trimmed().toLatin1();
	if (data.isEmpty())
		return QString();
	return QStringLiteral("payload:") + QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());
}

// Archive copies keep the payload, but re-assign stanza ids and may drop the
// <encrypted> element entirely.  A key derived from the peer and the rounded
// timestamp bridges all three forms; the +/-3s window covers client/server
// clock skew between the live stamp and the stored <delay> stamp.
static void cacheOmemoTimeKeys(OmemoAccount *AAccount, const QString &APeer,
	const QDateTime &ADateTime, const QByteArray &APlain)
{
	if (AAccount == nullptr || APeer.isEmpty() || APlain.isEmpty() || !ADateTime.isValid())
		return;
	const qint64 secs = ADateTime.toSecsSinceEpoch();
	for (qint64 delta = -3; delta <= 3; ++delta)
		AAccount->cacheMessage(QStringLiteral("time:%1#%2").arg(APeer).arg(secs + delta),APlain);
}

static QByteArray cachedOmemoTimeMessage(OmemoAccount *AAccount, const QString &APeer, const QDateTime &ADateTime)
{
	if (AAccount == nullptr || APeer.isEmpty() || !ADateTime.isValid())
		return QByteArray();
	// Nearest-second first: two messages a few seconds apart must not swap texts.
	static const qint64 deltas[] = {0,-1,1,-2,2,-3,3};
	const qint64 secs = ADateTime.toSecsSinceEpoch();
	for (int i = 0; i < int(sizeof(deltas)/sizeof(deltas[0])); ++i)
	{
		QByteArray cached;
		if (AAccount->cachedMessage(QStringLiteral("time:%1#%2").arg(APeer).arg(secs + deltas[i]),cached) && !cached.isEmpty())
			return cached;
	}
	return QByteArray();
}

// The archive may hand us a Message whose stanza tree is shared with the
// archive list or nested inside a MAM result document.  Editing that tree
// through QDomElement handles is unreliable (the caller may keep reading the
// fallback body).  Rebuilding through the public Message API is safe.
static void rebuildOmemoMessage(Message &AMessage, const QString &ABody)
{
	const Message::MessageType type = AMessage.type();
	const QString id = AMessage.id();
	const QString from = AMessage.from();
	const QString to = AMessage.to();
	const QString threadId = AMessage.threadId();
	const QString lang = AMessage.defLang();
	const QDateTime dateTime = AMessage.dateTime();
	const QVariant directionData = AMessage.data(MDR_MESSAGE_DIRECTION);
	const QVariant messageIdData = AMessage.data(MDR_MESSAGE_ID);

	Message rebuilt;
	rebuilt.setType(type);
	rebuilt.setId(id);
	rebuilt.setFrom(from);
	rebuilt.setTo(to);
	if (!threadId.isEmpty())
		rebuilt.setThreadId(threadId);
	rebuilt.setDefLang(lang);
	rebuilt.setDateTime(dateTime);
	rebuilt.setBody(ABody);
	if (!directionData.isNull())
		rebuilt.setData(MDR_MESSAGE_DIRECTION,directionData);
	if (!messageIdData.isNull())
		rebuilt.setData(MDR_MESSAGE_ID,messageIdData);

	AMessage = rebuilt;
}

static bool contactSupportsOmemo(OmemoAccount *AAccount, const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	return AAccount != nullptr && !jid.isEmpty()
		&& AAccount->isDeviceListKnown(jid) && AAccount->hasDevices(jid);
}

static QString omemoMarkToken(const QString &AMessageId)
{
	quint32 hash = qHash(AMessageId);
	QString token(QChar(0x2064)); // invisible plus, distinct from chatmarker tokens
	static const ushort keys[] = {0x2064,0x2060,0x2061,0x2062,0x2063,0x200B,0x200C,0x200D};
	const int keyCount = sizeof(keys)/sizeof(keys[0]);
	for (int i=0; i<7; i++)
	{
		token += QChar(keys[hash % keyCount]);
		hash >>= 3;
	}
	return token;
}

Omemo::Omemo() :
	FPluginManager(nullptr),
	FAccountManager(nullptr),
	FStanzaProcessor(nullptr),
	FPepManager(nullptr),
	FXmppStreamManager(nullptr),
	FMessageProcessor(nullptr),
	FMessageWidgets(nullptr),
	FAttachments(nullptr),
	FMessageContextManager(nullptr),
	FMediaConsumerActive(false),
	FAccountRemoving(false)
{
}

Omemo::~Omemo()
{
	if (FMessageProcessor != nullptr)
		FMessageProcessor->removeMessageEditor(MEO_OMEMO_DECRYPT,this);
	if (FMessageWidgets != nullptr)
		FMessageWidgets->removeStyleHandler(MSHO_OMEMO,this);
	if (FMessageWidgets != nullptr)
		FMessageWidgets->removeViewUrlHandler(MVUHO_OMEMO,this);
	FMediaRequests.clear();
	if (FStanzaProcessor != nullptr)
	{
		foreach (const StanzaHandles &handles, FStanzaHandles)
		{
			if (handles.in >= 0)
				FStanzaProcessor->removeStanzaHandle(handles.in);
			if (handles.out >= 0)
				FStanzaProcessor->removeStanzaHandle(handles.out);
		}
	}
	FStanzaHandles.clear();
	FPendingMessages.clear();
	FLockActions.clear();
	FLockToggles.clear();
	FMessageStatus.clear();

	foreach (OmemoAccount *account, FAccounts)
	{
		account->stop();
		delete account;
	}
	FAccounts.clear();
	FStreamAccounts.clear();
}

void Omemo::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("OMEMO");
	APluginInfo->description = tr("Legacy OMEMO 0.3 end-to-end encryption for one-to-one chats");
	APluginInfo->version = "0.1";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(ACCOUNTMANAGER_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->dependences.append(QUuid(STANZAPROCESSOR_UUID));
	APluginInfo->dependences.append(QUuid(PEPMANAGER_UUID));
	APluginInfo->dependences.append(QUuid(XMPPSTREAMS_UUID));
}

bool Omemo::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);
	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface(QStringLiteral("IAccountManager")).value(0,nullptr);
	if (plugin != nullptr)
		FAccountManager = qobject_cast<IAccountManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface(QStringLiteral("IStanzaProcessor")).value(0,nullptr);
	if (plugin != nullptr)
		FStanzaProcessor = qobject_cast<IStanzaProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface(QStringLiteral("IPEPManager")).value(0,nullptr);
	if (plugin != nullptr)
		FPepManager = qobject_cast<IPEPManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface(QStringLiteral("IXmppStreamManager")).value(0,nullptr);
	if (plugin != nullptr)
		FXmppStreamManager = qobject_cast<IXmppStreamManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface(QStringLiteral("IMessageProcessor")).value(0,nullptr);
	if (plugin != nullptr)
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());

	plugin = APluginManager->pluginInterface(QStringLiteral("IMessageWidgets")).value(0,nullptr);
	if (plugin != nullptr)
	{
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());
		FMessageContextManager = qobject_cast<IMessageDocumentContextManager *>(plugin->instance());
	}

	// The plugins we unregister from at destruction may already be dead by then
	// (shutdown order is not guaranteed), so drop the pointers as soon as the
	// owning plugin object is destroyed.  ~Omemo() then becomes a no-op.
	if (FStanzaProcessor != nullptr)
		connect(FStanzaProcessor->instance(),&QObject::destroyed,this,[this]() { FStanzaProcessor = nullptr; });
	if (FMessageProcessor != nullptr)
		connect(FMessageProcessor->instance(),&QObject::destroyed,this,[this]() { FMessageProcessor = nullptr; });
	if (FMessageWidgets != nullptr)
		connect(FMessageWidgets->instance(),&QObject::destroyed,this,[this]() { FMessageWidgets = nullptr; });

	return FAccountManager != nullptr && FStanzaProcessor != nullptr
		&& FPepManager != nullptr && FXmppStreamManager != nullptr
		&& FMessageProcessor != nullptr && FMessageWidgets != nullptr;
}

bool Omemo::initObjects()
{
	if (FAccountManager == nullptr)
		return false;

	FAttachments = new OmemoAttachments(this);
	connect(FAttachments,&OmemoAttachments::attachmentReady,
		this,&Omemo::onAttachmentReady);

	connect(FAccountManager->instance(),SIGNAL(accountInserted(IAccount *)),
		SLOT(onAccountInserted(IAccount *)));
	connect(FAccountManager->instance(),SIGNAL(accountRemoved(IAccount *)),
		SLOT(onAccountRemoved(IAccount *)));
	connect(FAccountManager->instance(),SIGNAL(accountDestroyed(const QUuid &)),
		SLOT(onAccountDestroyed(const QUuid &)));

	if (FXmppStreamManager != nullptr)
	{
		connect(FXmppStreamManager->instance(),SIGNAL(streamOpened(IXmppStream *)),
			SLOT(onStreamOpened(IXmppStream *)));
		connect(FXmppStreamManager->instance(),SIGNAL(streamClosed(IXmppStream *)),
			SLOT(onStreamClosed(IXmppStream *)));
	}

	foreach (IAccount *account, FAccountManager->accounts())
		onAccountInserted(account);

	if (FMessageProcessor != nullptr)
		FMessageProcessor->insertMessageEditor(MEO_OMEMO_DECRYPT,this);

	if (FMessageWidgets != nullptr)
	{
		FMessageWidgets->insertStyleHandler(MSHO_OMEMO,this);
		FMessageWidgets->insertViewUrlHandler(MVUHO_OMEMO,this);
		connect(FMessageWidgets->instance(),SIGNAL(chatWindowCreated(IMessageChatWindow *)),
			SLOT(onChatWindowCreated(IMessageChatWindow *)));
		connect(FMessageWidgets->instance(),SIGNAL(chatWindowDestroyed(IMessageChatWindow *)),
			SLOT(onChatWindowDestroyed(IMessageChatWindow *)));
		foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
			onChatWindowCreated(window);
	}

	return true;
}

bool Omemo::startPlugin()
{
	if (FXmppStreamManager != nullptr)
	{
		foreach (IXmppStream *stream, FXmppStreamManager->xmppStreams())
		{
			if (FXmppStreamManager->isXmppStreamActive(stream))
				onStreamOpened(stream);
		}
	}
	return true;
}

void Omemo::onAccountInserted(IAccount *AAccount)
{
	if (AAccount == nullptr || FAccounts.contains(AAccount->accountId()))
		return;

	OmemoAccount *account = new OmemoAccount(this,AAccount,FStanzaProcessor,FPepManager);
	connect(account,SIGNAL(error(QString)),this,SLOT(onAccountError(QString)));
	connect(account,&OmemoAccount::stateChanged,this,[this,account](const QString &AJid){
		onAccountStateChanged(account,AJid);
	});
	FAccounts.insert(AAccount->accountId(),account);

	if (FXmppStreamManager != nullptr && AAccount->xmppStream() != nullptr
		&& FXmppStreamManager->isXmppStreamActive(AAccount->xmppStream()))
	{
		startAccount(account);
	}
	// Offline local history is handled lazily by processInbound(), which
	// starts the account when the first encrypted archived message is shown.
}

void Omemo::onAccountRemoved(IAccount *AAccount)
{
	if (AAccount == nullptr)
		return;

	OmemoAccount *account = FAccounts.take(AAccount->accountId());
	if (account != nullptr)
	{
		FAccountRemoving = true;
		for (IMessageChatWindow *window : FLockActions.keys())
			updateLock(window);
		const QString streamKey = account->streamJid().full();
		for (auto it=FStreamAccounts.begin(); it!=FStreamAccounts.end(); )
		{
			if (it.value() == account)
				it = FStreamAccounts.erase(it);
			else
				++it;
		}
		for (auto it=FPendingMessages.begin(); it!=FPendingMessages.end(); )
		{
			QList<PendingMessage> remaining;
			for (const PendingMessage &message : it.value())
			{
				if (message.streamJid.full() != streamKey)
					remaining.append(message);
			}
			if (remaining.isEmpty())
				it = FPendingMessages.erase(it);
			else
			{
				it.value() = remaining;
				++it;
			}
		}
		account->stop();
		delete account;
		FAccountRemoving = false;
	}
}

void Omemo::onAccountDestroyed(const QUuid &AAccountId)
{
	// accountRemoved is also emitted when the profile is closed, so the
	// encrypted file must only be deleted on an actual account destruction.
	const QString path = Options::filesPath() + QStringLiteral("/omemo/")
		+ AAccountId.toString(QUuid::WithoutBraces) + QStringLiteral(".dat");
	if (QFile::exists(path))
		QFile::remove(path);
}

void Omemo::onStreamOpened(IXmppStream *AXmppStream)
{
	if (FAccountManager == nullptr || AXmppStream == nullptr)
		return;

	IAccount *account = FAccountManager->findAccountByStream(AXmppStream->streamJid());
	if (account == nullptr)
		return;

	const QString streamKey = AXmppStream->streamJid().full();
	if (!FStanzaHandles.contains(streamKey) && FStanzaProcessor != nullptr)
	{
		StanzaHandles handles;
		IStanzaHandle inHandle;
		inHandle.handler = this;
		inHandle.order = SHO_MI_OMEMO_DECRYPT;
		inHandle.direction = IStanzaHandle::DirectionIn;
		inHandle.streamJid = AXmppStream->streamJid();
		inHandle.conditions.append(QString::fromLatin1(SHC_OMEMO_ENCRYPTED));
		handles.in = FStanzaProcessor->insertStanzaHandle(inHandle);

		IStanzaHandle outHandle;
		outHandle.handler = this;
		outHandle.order = SHO_MO_OMEMO_ENCRYPT;
		outHandle.direction = IStanzaHandle::DirectionOut;
		outHandle.streamJid = AXmppStream->streamJid();
		outHandle.conditions.append(QString::fromLatin1(SHC_OMEMO_MESSAGE));
		handles.out = FStanzaProcessor->insertStanzaHandle(outHandle);

		FStanzaHandles.insert(streamKey,handles);
	}

	OmemoAccount *omemoAccount = FAccounts.value(account->accountId());
	if (omemoAccount != nullptr)
	{
		startAccount(omemoAccount);
		if (omemoAccount->isStarted())
			omemoAccount->streamOpened(AXmppStream->streamJid());
		updateLocks(omemoAccount);
	}
}

void Omemo::onStreamClosed(IXmppStream *AXmppStream)
{
	if (FAccountManager == nullptr || AXmppStream == nullptr)
		return;

	const QString streamKey = AXmppStream->streamJid().full();
	const StanzaHandles handles = FStanzaHandles.take(streamKey);
	if (FStanzaProcessor != nullptr)
	{
		if (handles.in >= 0)
			FStanzaProcessor->removeStanzaHandle(handles.in);
		if (handles.out >= 0)
			FStanzaProcessor->removeStanzaHandle(handles.out);
	}

	for (auto it=FPendingMessages.begin(); it!=FPendingMessages.end(); )
	{
		QList<PendingMessage> remaining;
		for (const PendingMessage &message : it.value())
		{
			if (message.streamJid.full() != streamKey)
				remaining.append(message);
		}
		if (remaining.isEmpty())
			it = FPendingMessages.erase(it);
		else
		{
			it.value() = remaining;
			++it;
		}
	}

	IAccount *account = FAccountManager->findAccountByStream(AXmppStream->streamJid());
	if (account == nullptr)
		return;

	OmemoAccount *omemoAccount = FAccounts.value(account->accountId());
	if (omemoAccount != nullptr)
	{
		for (auto it=FStreamAccounts.begin(); it!=FStreamAccounts.end(); )
		{
			if (it.value() == omemoAccount)
				it = FStreamAccounts.erase(it);
			else
				++it;
		}
		omemoAccount->streamClosed();
		updateLocks(omemoAccount);
	}
}

void Omemo::onAccountError(const QString &AMessage)
{
	LOG_ERROR(QStringLiteral("OMEMO: %1").arg(AMessage));
}

void Omemo::startAccount(OmemoAccount *AAccount)
{
	if (AAccount == nullptr || AAccount->isStarted())
		return;
	if (!AAccount->start())
		return;
	FStreamAccounts.insert(AAccount->streamJid().full(),AAccount);
	updateLocks(AAccount);
}

bool Omemo::stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept)
{
	Q_UNUSED(AAccept);
	const StanzaHandles handles = FStanzaHandles.value(AStreamJid.full());
	if (AHandleId == handles.in)
	{
		processInbound(AStreamJid,AStanza);
		return false;
	}
	if (AHandleId == handles.out)
		return processOutbound(AStreamJid,AStanza);
	return false;
}

bool Omemo::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	Q_UNUSED(AOrder);
	if (AMessage.id().isEmpty() && isChatMessage(AMessage))
		AMessage.setRandomId();

	QDomElement encrypted = AMessage.stanza().firstElement(QStringLiteral("encrypted"),
		QString::fromLatin1(OMEMO_NS));
	if (!encrypted.isNull())
	{
		// Make the stanza tree exclusive to this Message before decrypt-in-place
		AMessage.detach();
		if (processInbound(AStreamJid,AMessage.stanza()))
			cacheOmemoTimeKeys(accountForStream(AStreamJid),AMessage.fromJid().bare(),
				AMessage.dateTime(),AMessage.body().toUtf8());
		else
		{
			setMessageStatus(AMessage.id(),StatusFailed);
			markStanzaStatus(AMessage.stanza(),StatusFailed);
		}
		return false;
	}

	if (!AMessage.id().isEmpty() && FMessageStatus.contains(AMessage.id()))
		return false;
	if (!isChatMessage(AMessage))
		return false;

	if (!AMessage.stanza().firstElement(QStringLiteral("sent"),
		QString::fromLatin1(NS_MESSAGE_CARBONS)).isNull())
	{
		OmemoAccount *account = accountForStream(AStreamJid);
		const int status = contactSupportsOmemo(account,AMessage.toJid().bare())
			? StatusUnencryptedExpected : StatusNotEncrypted;
		setMessageStatus(AMessage.id(),status);
		return false;
	}

	if (ADirection == IMessageProcessor::DirectionOut)
	{
		markOutgoingStatus(AStreamJid,AMessage);
	}
	else
	{
		OmemoAccount *account = accountForStream(AStreamJid);
		const int status = contactSupportsOmemo(account,AMessage.fromJid().bare())
			? StatusUnencryptedExpected : StatusNotEncrypted;
		setMessageStatus(AMessage.id(),status);
	}
	return false;
}

bool Omemo::decryptMessage(const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	if (AMessage.id().isEmpty() && isChatMessage(AMessage))
		AMessage.setRandomId();

	OmemoAccount *account = accountForStream(AStreamJid);
	if (account != nullptr && !account->isStarted())
		account->start(false);

	const QString contact = ADirection == IMessageProcessor::DirectionOut
		? AMessage.toJid().bare() : AMessage.fromJid().bare();

	QDomElement encrypted = AMessage.stanza().firstElement(QStringLiteral("encrypted"),
		QString::fromLatin1(OMEMO_NS));
	if (!encrypted.isNull())
	{
		// The plaintext cache is only meaningful for an encrypted archive
		// message.  Applying it to an ordinary message whose stanza id happens
		// to collide with a cached OMEMO id can replace its body with an empty
		// value, making the message invisible in the chat window.
		QByteArray cached;
		const QString payloadKey = omemoPayloadKey(encrypted);
		bool fromCache = account != nullptr && account->cachedMessage(AMessage.id(),cached);
		if (!fromCache && !payloadKey.isEmpty())
			fromCache = account != nullptr && account->cachedMessage(payloadKey,cached);
		if (!fromCache)
			cached = cachedOmemoTimeMessage(account,contact,AMessage.dateTime());
		if (fromCache || !cached.isEmpty())
		{
			const QString plain = sanitizeOmemoBody(account,QString::fromUtf8(cached),false);
			if (!plain.isEmpty())
			{
				rebuildOmemoMessage(AMessage,plain);
				setMessageStatus(AMessage.id(),StatusEncrypted);
				return true;
			}
		}
		QString decrypted;
		const bool ok = processInbound(AStreamJid,AMessage.stanza(),&decrypted);
		if (!decrypted.isEmpty() && decrypted != AMessage.body())
		{
			// The in-place edit is not visible through this shared copy
			rebuildOmemoMessage(AMessage,decrypted);
		}
		return ok;
	}

	// Local archive engines may store only the readable OMEMO fallback body
	// and drop the <encrypted> payload.  If the original plaintext was cached
	// while the message was live, recover it by the preserved id, payload or
	// peer+timestamp key.
	const QString archiveBody = AMessage.body();
	if (account != nullptr
		&& (archiveBody.contains(QStringLiteral("OMEMO encrypted"))
			|| archiveBody.contains(QStringLiteral("I sent you an OMEMO encrypted message"))))
	{
		QByteArray cached;
		bool fromCache = account->cachedMessage(AMessage.id(),cached);
		if (!fromCache)
			cached = cachedOmemoTimeMessage(account,contact,AMessage.dateTime());
		if (fromCache || !cached.isEmpty())
		{
			const QString plain = sanitizeOmemoBody(account,QString::fromUtf8(cached),false);
			if (!plain.isEmpty())
			{
				rebuildOmemoMessage(AMessage,plain);
				setMessageStatus(AMessage.id(),StatusEncrypted);
				return true;
			}
		}
	}

	QDomElement marker = AMessage.stanza().firstElement(QStringLiteral("omemo"),
		QString::fromLatin1(OMEMO_NS_STATUS));
	if (!marker.isNull() && !FMessageStatus.contains(AMessage.id()))
	{
		int status = statusFromName(marker.attribute(QStringLiteral("status")));
		if (status == StatusNotEncrypted || status == StatusUnencryptedExpected)
		{
			status = contactSupportsOmemo(account,contact)
				? StatusUnencryptedExpected : StatusNotEncrypted;
		}
		setMessageStatus(AMessage.id(),status);
		return status == StatusEncrypted;
	}

	if (!FMessageStatus.contains(AMessage.id()) && isChatMessage(AMessage))
	{
		const int status = contactSupportsOmemo(account,contact)
			? StatusUnencryptedExpected : StatusNotEncrypted;
		setMessageStatus(AMessage.id(),status);
	}
	return false;
}

OmemoAccount *Omemo::accountForStream(const Jid &AStreamJid)
{
	if (FAccountManager == nullptr)
		return nullptr;
	IAccount *account = FAccountManager->findAccountByStream(AStreamJid);
	if (account == nullptr)
		return nullptr;

	OmemoAccount *result = FAccounts.value(account->accountId());
	if (result == nullptr && !FAccountRemoving
		&& (FPluginManager == nullptr || !FPluginManager->isShutingDown()))
	{
		// A restored chat window can ask for history before the account
		// insertion signal has been processed by this plugin.
		onAccountInserted(account);
		result = FAccounts.value(account->accountId());
	}
	return result;
}

QString Omemo::sanitizeOmemoBody(OmemoAccount *AAccount, const QString &APlain, bool AAutoDownload) const
{
	// The attachments sanitizer may not exist before the plugin is fully
	// initialized; then the plaintext passes through unchanged.
	return FAttachments != nullptr
		? FAttachments->sanitizeBody(AAccount,APlain,AAutoDownload) : APlain;
}

bool Omemo::processInbound(const Jid &AStreamJid, Stanza &AStanza, QString *APlainOut)
{
	QDomElement encrypted = AStanza.firstElement(QStringLiteral("encrypted"),QString::fromLatin1(OMEMO_NS));
	if (encrypted.isNull())
		return false;

	if (AStanza.id().isEmpty())
		AStanza.setUniqueId();
	const QString messageId = AStanza.id();

	OmemoAccount *account = accountForStream(AStreamJid);
	if (account != nullptr && !account->isStarted())
		account->start(false);
	if (account == nullptr || !account->isStarted())
	{
		LOG_STRM_WARNING(AStreamJid,QString("OMEMO decrypt skipped, account not ready, from=%1").arg(AStanza.from()));
		setMessageStatus(messageId,StatusFailed);
		markStanzaStatus(AStanza,StatusFailed);
		return false;
	}

	const QString sender = AStanza.fromJid().isValid() ? AStanza.fromJid().bare() : AStanza.from();
	QStringList keyInfo;
	for (QDomElement key = encrypted.firstChildElement(QStringLiteral("header")).firstChildElement(QStringLiteral("key"));
		!key.isNull(); key = key.nextSiblingElement(QStringLiteral("key")))
	{
		const QByteArray raw = QByteArray::fromBase64(key.text().trimmed().toLatin1());
		keyInfo.append(QStringLiteral("%1:%2:%3:%4").arg(key.attribute(QStringLiteral("rid")))
			.arg(raw.size()).arg(key.attribute(QStringLiteral("prekey")),QString::fromLatin1(raw.left(2).toHex())));
	}
	LOG_STRM_DEBUG(AStreamJid,QString("OMEMO inbound id=%1, sid=%2, keys=%3")
		.arg(messageId).arg(encrypted.firstChildElement(QStringLiteral("header")).attribute(QStringLiteral("sid")))
		.arg(keyInfo.join(QLatin1Char(','))));
	QByteArray plain;
	QString error;
	const bool decrypted = account->decryptMessage(sender,encrypted,plain,&error);
	AStanza.element().removeChild(encrypted);

	if (decrypted)
	{
		// XEP-0454: rewrite aesgcm:// urls before anything else stores or
		// shows the plaintext.  This removes the key fragment and the data
		// URI thumbnail from the message body; the key material goes into the
		// encrypted store and the file download starts here.
		QString plainText = sanitizeOmemoBody(account,QString::fromUtf8(plain),true);
		const QByteArray sanitized = plainText.toUtf8();

		account->cacheMessage(messageId,sanitized);
		const QString payloadKey = omemoPayloadKey(encrypted);
		if (!payloadKey.isEmpty())
			account->cacheMessage(payloadKey,sanitized);
		if (APlainOut != nullptr)
			*APlainOut = plainText;
		replaceBody(AStanza,plainText);
		setMessageStatus(messageId,StatusEncrypted);
		markStanzaStatus(AStanza,StatusEncrypted);
		LOG_STRM_DEBUG(AStreamJid,QString("OMEMO message decrypted, from=%1").arg(sender));
		return true;
	}

	// Do not overwrite the readable fallback body another client added: it
	// carries the "this is an OMEMO message" notice and is more informative
	// than our generic placeholder.
	if (AStanza.firstElement(QStringLiteral("body")).text().isEmpty())
	{
		const QString placeholder = error.contains(QStringLiteral("not encrypted to this device"))
			? tr("[OMEMO message is encrypted for another device]") : tr("[OMEMO: cannot decrypt message]");
		if (APlainOut != nullptr)
			*APlainOut = placeholder;
		replaceBody(AStanza,placeholder);
	}
	setMessageStatus(messageId,StatusFailed);
	markStanzaStatus(AStanza,StatusFailed);
	LOG_STRM_WARNING(AStreamJid,QString("Cannot decrypt OMEMO message from %1: %2").arg(sender,error));
	return false;
}

bool Omemo::processOutbound(const Jid &AStreamJid, Stanza &AStanza)
{
	if (AStanza.id().isEmpty())
		AStanza.setUniqueId();
	const QString messageId = AStanza.id();

	if (!AStanza.firstElement(QStringLiteral("encrypted"),QString::fromLatin1(OMEMO_NS)).isNull())
	{
		setMessageStatus(messageId,StatusEncrypted);
		return false;
	}

	const QString messageType = AStanza.type();
	if (!messageType.isEmpty() && messageType != QStringLiteral("chat") && messageType != QStringLiteral("normal"))
		return false;
	if (AStanza.firstElement(QStringLiteral("body")).isNull())
		return false;

	OmemoAccount *account = accountForStream(AStreamJid);
	if (account == nullptr || !account->isStarted())
	{
		setMessageStatus(messageId,StatusNotEncrypted);
		markStanzaStatus(AStanza,StatusNotEncrypted);
		return false;
	}

	const QString contactJid = AStanza.toJid().bare();
	if (contactJid.isEmpty() || !account->isChatEnabled(contactJid))
	{
		const int status = contactSupportsOmemo(account,contactJid)
			? StatusUnencryptedExpected : StatusNotEncrypted;
		setMessageStatus(messageId,status);
		markStanzaStatus(AStanza,status);
		return false;
	}

	const QString ownJid = AStreamJid.bare();
	account->ensureDeviceList(contactJid);

	if (account->isDeviceListKnown(contactJid) && !account->hasDevices(contactJid))
	{
		setMessageStatus(messageId,StatusNotEncrypted);
		markStanzaStatus(AStanza,StatusNotEncrypted);
		return false;
	}

	if (!account->isDeviceListKnown(contactJid))
	{
		account->ensureDeviceList(contactJid);
		account->ensureDeviceList(ownJid);
		setMessageStatus(messageId,StatusPending);
		markStanzaStatus(AStanza,StatusPending);
		queuePending(AStreamJid,contactJid,AStanza);
		return true;
	}

	account->ensureSessions(contactJid);
	account->ensureSessions(ownJid);
	if (!account->sessionsReady(contactJid))
	{
		setMessageStatus(messageId,StatusPending);
		markStanzaStatus(AStanza,StatusPending);
		queuePending(AStreamJid,contactJid,AStanza);
		return true;
	}
	if (!account->hasEncryptableDevices(contactJid))
	{
		LOG_STRM_WARNING(AStreamJid,QString("OMEMO message to %1 has no trusted destination devices").arg(contactJid));
		setMessageStatus(messageId,StatusFailed);
		markStanzaStatus(AStanza,StatusFailed);
		return true;
	}

	const QByteArray plain = AStanza.firstElement(QStringLiteral("body")).text().toUtf8();
	if (!encryptStanza(account,contactJid,AStanza))
	{
		LOG_STRM_WARNING(AStreamJid,QString("Cannot encrypt OMEMO message to %1").arg(contactJid));
		setMessageStatus(messageId,StatusPending);
		markStanzaStatus(AStanza,StatusPending);
		queuePending(AStreamJid,contactJid,AStanza);
		return true;
	}

	account->cacheMessage(messageId,plain);
	const QDomElement encrypted = AStanza.firstElement(QStringLiteral("encrypted"),QString::fromLatin1(OMEMO_NS));
	const QString payloadKey = omemoPayloadKey(encrypted);
	if (!payloadKey.isEmpty())
		account->cacheMessage(payloadKey,plain);
	cacheOmemoTimeKeys(account,contactJid,QDateTime::currentDateTime(),plain);
	setMessageStatus(messageId,StatusEncrypted);
	markStanzaStatus(AStanza,StatusEncrypted);
	return false;
}

bool Omemo::encryptStanza(OmemoAccount *AAccount, const QString &AContactJid, Stanza &AStanza)
{
	QDomElement body = AStanza.firstElement(QStringLiteral("body"));
	if (body.isNull())
		return false;

	const QByteArray plain = body.text().toUtf8();
	QDomElement encrypted;
	QString error;
	if (!AAccount->encryptMessage(AContactJid,plain,encrypted,&error))
	{
		LOG_STRM_WARNING(AAccount->streamJid(),QString("Cannot encrypt OMEMO message to %1: %2")
			.arg(AContactJid,error));
		return false;
	}

	QDomNodeList bodies = AStanza.element().elementsByTagName(QStringLiteral("body"));
	for (int i=bodies.count()-1; i>=0; --i)
		AStanza.element().removeChild(bodies.at(i));
	AStanza.element().appendChild(encrypted.cloneNode(true));
	// Ask the server to archive the encrypted stanza even though its only
	// readable body is a fallback.
	AStanza.addElement(QStringLiteral("store"),QString::fromLatin1(OMEMO_NS_HINTS));
	// Legacy clients and servers expect a readable fallback for clients that
	// do not understand OMEMO.
	replaceBody(AStanza,QStringLiteral("[This message is OMEMO encrypted]"));
	return true;
}

void Omemo::queuePending(const Jid &AStreamJid, const QString &AContactJid, const Stanza &AStanza)
{
	PendingMessage message;
	message.streamJid = AStreamJid;
	message.contactJid = Jid(AContactJid).bare();
	message.stanza = AStanza;
	FPendingMessages[message.contactJid].append(message);
	LOG_STRM_DEBUG(AStreamJid,QString("OMEMO message queued until keys are ready, to=%1").arg(message.contactJid));
}

void Omemo::flushPending(OmemoAccount *AAccount, const QString &AJid)
{
	if (AAccount == nullptr)
		return;
	const QString contactJid = Jid(AJid).bare();
	auto it = FPendingMessages.find(contactJid);
	if (it == FPendingMessages.end())
		return;

	const QString ownJid = AAccount->bareJid();
	const QString streamKey = AAccount->streamJid().full();
	QList<PendingMessage> remaining;
	for (const PendingMessage &message : it.value())
	{
		if (message.streamJid.full() != streamKey)
		{
			remaining.append(message);
			continue;
		}

		if (!AAccount->isChatEnabled(contactJid))
		{
			Stanza stanza = message.stanza;
			const int status = contactSupportsOmemo(AAccount,contactJid)
				? StatusUnencryptedExpected : StatusNotEncrypted;
			setMessageStatus(stanza.id(),status);
			markStanzaStatus(stanza,status);
			if (!sendRaw(message.streamJid,stanza))
				remaining.append(message);
			continue;
		}

		AAccount->ensureDeviceList(contactJid);
		if (!AAccount->isDeviceListKnown(contactJid))
		{
			setMessageStatus(message.stanza.id(),StatusPending);
			remaining.append(message);
			continue;
		}

		if (!AAccount->hasDevices(contactJid))
		{
			Stanza stanza = message.stanza;
			setMessageStatus(stanza.id(),StatusNotEncrypted);
			markStanzaStatus(stanza,StatusNotEncrypted);
			if (!sendRaw(message.streamJid,stanza))
				remaining.append(message);
			continue;
		}

		AAccount->ensureDeviceList(ownJid);
		AAccount->ensureSessions(contactJid);
		AAccount->ensureSessions(ownJid);
		if (!AAccount->sessionsReady(contactJid))
		{
			setMessageStatus(message.stanza.id(),StatusPending);
			remaining.append(message);
			continue;
		}
		if (!AAccount->hasEncryptableDevices(contactJid))
		{
			LOG_STRM_WARNING(message.streamJid,QString("Dropping queued OMEMO message to %1: no trusted devices").arg(contactJid));
			setMessageStatus(message.stanza.id(),StatusFailed);
			continue;
		}

		Stanza stanza = message.stanza;
		const QByteArray plain = stanza.firstElement(QStringLiteral("body")).text().toUtf8();
		if (!encryptStanza(AAccount,contactJid,stanza))
		{
			LOG_STRM_WARNING(message.streamJid,QString("Dropping queued OMEMO message to %1").arg(contactJid));
			setMessageStatus(stanza.id(),StatusFailed);
			continue;
		}
		AAccount->cacheMessage(stanza.id(),plain);
		setMessageStatus(stanza.id(),StatusEncrypted);
		markStanzaStatus(stanza,StatusEncrypted);
		if (!sendRaw(message.streamJid,stanza))
			remaining.append(message);
	}

	if (remaining.isEmpty())
		FPendingMessages.erase(it);
	else
		it.value() = remaining;
}

bool Omemo::sendRaw(const Jid &AStreamJid, Stanza &AStanza)
{
	if (FXmppStreamManager == nullptr)
		return false;
	IXmppStream *stream = FXmppStreamManager->findXmppStream(AStreamJid);
	if (stream == nullptr || stream->sendStanza(AStanza) < 0)
	{
		LOG_STRM_WARNING(AStreamJid,QStringLiteral("Cannot send queued OMEMO stanza"));
		return false;
	}
	return true;
}

void Omemo::onAccountStateChanged(OmemoAccount *AAccount, const QString &AJid)
{
	if (AAccount == nullptr)
		return;
	updateLocks(AAccount);
	if (Jid(AJid).bare() == AAccount->bareJid())
	{
		const QList<QString> contacts = FPendingMessages.keys();
		for (const QString &contact : contacts)
			flushPending(AAccount,contact);
	}
	else
	{
		flushPending(AAccount,AJid);
	}
}

void Omemo::onChatWindowCreated(IMessageChatWindow *AWindow)
{
	if (AWindow == nullptr || AWindow->infoWidget() == nullptr)
		return;

	if (!FLockActions.contains(AWindow) && AWindow->infoWidget()->infoToolBarChanger() != nullptr)
	{
		Menu *menu = new Menu(AWindow->instance());
		Action *toggleAction = new Action(menu);
		toggleAction->setCheckable(true);
		toggleAction->setText(tr("Use OMEMO in this chat"));
		connect(toggleAction,&QAction::triggered,this,[this,AWindow](bool AChecked) {
			OmemoAccount *account = accountForStream(AWindow->streamJid());
			if (account != nullptr && account->isStarted())
				account->setChatEnabled(AWindow->contactJid().bare(),AChecked);
			updateLock(AWindow);
		});
		menu->addAction(toggleAction);

		Action *devicesAction = new Action(menu);
		devicesAction->setText(tr("OMEMO devices..."));
		connect(devicesAction,&QAction::triggered,this,[this,AWindow]() {
			showDevicesDialog(AWindow);
		});
		menu->addAction(devicesAction);

		Action *lockAction = new Action(AWindow->instance());
		lockAction->setText(tr("OMEMO"));
		lockAction->setMenu(menu);
		QToolButton *lockButton = AWindow->infoWidget()->infoToolBarChanger()
			->insertAction(lockAction,TBG_MWIWTB_OMEMO);
		if (lockButton != nullptr)
			lockButton->setPopupMode(QToolButton::InstantPopup);
		FLockActions.insert(AWindow,lockAction);
		FLockToggles.insert(AWindow,toggleAction);

		if (AWindow->address() != nullptr)
		{
			connect(AWindow->address()->instance(),SIGNAL(addressChanged(const Jid &, const Jid &)),
				this,SLOT(onWindowAddressChanged()));
			connect(AWindow->address()->instance(),SIGNAL(streamJidChanged(const Jid &, const Jid &)),
				this,SLOT(onWindowAddressChanged()));
		}
	}

	connect(AWindow->infoWidget()->instance(),SIGNAL(contextMenuRequested(Menu *)),
		this,SLOT(onWindowContextMenuRequested(Menu *)),Qt::UniqueConnection);

	if (AWindow->viewWidget() != nullptr)
	{
		connect(AWindow->viewWidget()->instance(),SIGNAL(viewContextMenu(const QPoint &, Menu *)),
			this,SLOT(onViewContextMenu(const QPoint &, Menu *)),Qt::UniqueConnection);
	}

	updateLock(AWindow);
}

void Omemo::onChatWindowDestroyed(IMessageChatWindow *AWindow)
{
	FLockActions.remove(AWindow);
	FLockToggles.remove(AWindow);
}

void Omemo::onWindowAddressChanged()
{
	IMessageAddress *address = qobject_cast<IMessageAddress *>(sender());
	if (address == nullptr)
		return;
	for (auto it=FLockActions.constBegin(); it!=FLockActions.constEnd(); ++it)
	{
		if (it.key()->address() == address)
			updateLock(it.key());
	}
}

void Omemo::updateLock(IMessageChatWindow *AWindow)
{
	Action *action = FLockActions.value(AWindow);
	Action *toggleAction = FLockToggles.value(AWindow);
	if (action == nullptr)
		return;

	QIcon baseIcon = IconStorage::staticStorage(RSR_STORAGE_MENUICONS)
		->getIcon(MNI_CONNECTION_ENCRYPTED);
	if (baseIcon.isNull())
		baseIcon = QIcon::fromTheme(QStringLiteral("security-high"));
	const QColor orange(235,150,0);
	const QColor red(200,30,30);
	const QColor gray(145,145,145);

	OmemoAccount *account = accountForStream(AWindow->streamJid());
	if (account == nullptr || !account->isStarted())
	{
		action->setIcon(statusIcon(baseIcon,gray));
		action->setToolTip(tr("OMEMO is not active for this account"));
		action->setEnabled(false);
		if (toggleAction != nullptr)
		{
			toggleAction->setChecked(false);
			toggleAction->setEnabled(false);
		}
		return;
	}

	const QString contact = AWindow->contactJid().bare();
	if (toggleAction != nullptr)
	{
		toggleAction->setEnabled(true);
		toggleAction->setChecked(account->isChatEnabled(contact));
	}

	if (!account->isChatEnabled(contact))
	{
		action->setIcon(statusIcon(baseIcon,gray));
		action->setToolTip(tr("OMEMO is disabled for this chat. "
			"Open the lock menu to enable it."));
		action->setEnabled(true);
		return;
	}

	account->ensureDeviceList(contact);
	const QList<quint32> devices = account->devices(contact);
	int trusted = 0;
	int undecided = 0;
	int blocked = 0;
	for (quint32 deviceId : devices)
	{
		switch (account->deviceTrust(contact,deviceId))
		{
		case OmemoStore::TrustTrusted: ++trusted; break;
		case OmemoStore::TrustDistrusted: ++blocked; break;
		default: ++undecided; break;
		}
	}

	QIcon icon;
	QString tooltip;
	if (trusted > 0 && account->isDeviceListKnown(contact))
		account->ensureSessions(contact);
	if (!account->isDeviceListKnown(contact))
	{
		icon = statusIcon(baseIcon,gray);
		tooltip = tr("Checking OMEMO support...");
	}
	else if (devices.isEmpty())
	{
		icon = statusIcon(baseIcon,gray);
		tooltip = tr("This contact has no OMEMO devices. Messages are not encrypted.");
	}
	else if (trusted > 0)
	{
		if (!account->sessionsReady(contact))
		{
			icon = statusIcon(baseIcon,orange);
			tooltip = tr("OMEMO is enabled. Preparing an encrypted session...");
		}
		else if (undecided > 0)
		{
			icon = statusIcon(baseIcon,orange);
			tooltip = tr("OMEMO is enabled, but %1 device(s) are not verified. "
				"Messages are encrypted only to %2 trusted device(s).")
				.arg(undecided).arg(trusted);
		}
		else
		{
			icon = baseIcon;
			tooltip = tr("OMEMO is enabled. Trusted devices: %1.").arg(trusted);
		}
	}
	else if (blocked >= devices.count())
	{
		icon = statusIcon(baseIcon,red);
		tooltip = tr("All OMEMO devices of this contact are blocked.");
	}
	else
	{
		icon = statusIcon(baseIcon,orange);
		tooltip = tr("No trusted OMEMO devices. Messages will not be sent "
			"until a device is trusted.");
	}

	if (blocked > 0 && trusted > 0)
		tooltip += QLatin1Char('\n') + tr("Blocked devices: %1.").arg(blocked);

	action->setIcon(icon);
	action->setToolTip(tooltip);
	action->setEnabled(true);
}

void Omemo::updateLocks(OmemoAccount *AAccount)
{
	for (auto it=FLockActions.constBegin(); it!=FLockActions.constEnd(); ++it)
	{
		if (accountForStream(it.key()->streamJid()) == AAccount)
			updateLock(it.key());
	}
}

void Omemo::showDevicesDialog(IMessageChatWindow *AWindow)
{
	OmemoAccount *account = accountForStream(AWindow->streamJid());
	if (account == nullptr || !account->isStarted())
		return;
	OmemoDevicesDialog dialog(account,AWindow->streamJid(),AWindow->contactJid(),AWindow->instance());
	dialog.exec();
}

void Omemo::onWindowContextMenuRequested(Menu *AMenu)
{
	IMessageInfoWidget *infoWidget = qobject_cast<IMessageInfoWidget *>(sender());
	if (infoWidget == nullptr || infoWidget->messageWindow() == nullptr || AMenu == nullptr)
		return;

	IMessageWindow *window = infoWidget->messageWindow();
	OmemoAccount *account = accountForStream(window->streamJid());
	if (account == nullptr || !account->isStarted())
		return;

	Action *action = new Action(AMenu);
	action->setText(tr("OMEMO devices..."));
	connect(action,&QAction::triggered,this,[this,window]() {
		IMessageChatWindow *chatWindow = qobject_cast<IMessageChatWindow *>(window->instance());
		if (chatWindow != nullptr)
			showDevicesDialog(chatWindow);
	});
	AMenu->addAction(action,AG_DEFAULT);
}

void Omemo::onViewContextMenu(const QPoint &APosition, Menu *AMenu)
{
	IMessageViewWidget *widget = qobject_cast<IMessageViewWidget *>(sender());
	if (widget == nullptr || FAttachments == nullptr || AMenu == nullptr)
		return;

	// The attachment link under the cursor is detected the same way the
	// MessageWidgets menu does: first the anchor/image format property, then
	// the fragment href.
	QString href = widget->textFormatAt(APosition).property(TMI_IMAGE_LINK_URL).toString();
	if (href.isEmpty())
	{
		QTextDocumentFragment fragment = widget->textFragmentAt(APosition);
		href = TextManager::getTextFragmentHref(fragment.isEmpty() ? widget->selection() : fragment);
	}
	// The href arrives url-decoded through the anchor click pipeline as well;
	// both encodings are tried.
	OmemoAccount *account = nullptr;
	if (!isAttachmentUrl(href,&account))
	{
		const QString encoded = QString::fromUtf8(QUrl(href).toEncoded());
		if (!isAttachmentUrl(encoded,&account))
			return;
		href = encoded;
	}

	Action *openAction = new Action(AMenu);
	openAction->setText(tr("Open"));
	openAction->setData(Action::DR_Parametr1,href);
	connect(openAction,SIGNAL(triggered(bool)),SLOT(onAttachmentOpenActionTriggered(bool)));
	AMenu->addAction(openAction,AG_MWVWCM_OMEMO,true);

	// A pending "Save as..." is disabled until the decrypted file arrives.
	Action *saveAction = new Action(AMenu);
	saveAction->setText(tr("Save as..."));
	saveAction->setEnabled(!FPendingSaves.contains(href));
	saveAction->setData(Action::DR_Parametr1,href);
	connect(saveAction,SIGNAL(triggered(bool)),SLOT(onAttachmentSaveAsActionTriggered(bool)));
	AMenu->addAction(saveAction,AG_MWVWCM_OMEMO,true);
}

void Omemo::onAttachmentOpenActionTriggered(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	OmemoAccount *account = nullptr;
	const QString href = action != nullptr ? action->data(Action::DR_Parametr1).toString() : QString();
	if (action == nullptr || !isAttachmentUrl(href,&account))
		return;
	if (OmemoAttachments::isFileCached(account,href))
		QDesktopServices::openUrl(QUrl::fromLocalFile(OmemoAttachments::attachmentFilePath(account,href)));
	else
		FAttachments->downloadAttachment(account,href);
}

void Omemo::onAttachmentSaveAsActionTriggered(bool)
{
	Action *action = qobject_cast<Action *>(sender());
	OmemoAccount *account = nullptr;
	const QString href = action != nullptr ? action->data(Action::DR_Parametr1).toString() : QString();
	if (action == nullptr || !isAttachmentUrl(href,&account) || FPendingSaves.contains(href))
		return;

	const QString name = OmemoAttachments::attachmentFileName(href);
	const QString target = QFileDialog::getSaveFileName(
		action->parent() != nullptr ? qobject_cast<QWidget *>(action->parent()) : nullptr,
		tr("Save decrypted file"),name);
	if (target.isEmpty())
		return;
	if (OmemoAttachments::isFileCached(account,href))
		saveAttachment(account,href,target);
	else
	{
		// The file is still downloading: save it automatically when the
		// download and decryption complete.
		FPendingSaves.insert(href,target);
		FAttachments->downloadAttachment(account,href);
	}
}

void Omemo::saveAttachment(OmemoAccount *AAccount, const QString &ACleanUrl, const QString &ATargetPath)
{
	const QString localFile = OmemoAttachments::attachmentFilePath(AAccount,ACleanUrl);
	if (QFile::exists(ATargetPath) && !QFile::remove(ATargetPath))
	{
		LOG_WARNING(QString("OMEMO attachment save failed (target busy), target=%1").arg(ATargetPath));
		return;
	}
	if (!QFile::copy(localFile,ATargetPath))
		LOG_WARNING(QString("OMEMO attachment save failed, target=%1").arg(ATargetPath));
	else
		LOG_STRM_INFO(AAccount->streamJid(),QString("OMEMO attachment saved, target=%1, url=%2").arg(ATargetPath,ACleanUrl));
}

bool Omemo::canHandleMediaUrl(const QUrl &AUrl)
{
	OmemoAccount *account = nullptr;
	return isAttachmentUrl(QString::fromUtf8(AUrl.toEncoded()),&account);
}

void Omemo::requestMediaPath(const QUrl &AUrl, const std::function<void(const QString &AFilePath)> &AHandler)
{
	const QString href = QString::fromUtf8(AUrl.toEncoded());
	OmemoAccount *account = nullptr;
	if (AHandler == nullptr || !isAttachmentUrl(href,&account))
	{
		if (AHandler != nullptr)
			AHandler(QString());
		return;
	}

	// The answers are always given on the main thread, the download ready
	// signal comes from the same thread.
	if (OmemoAttachments::isFileCached(account,href))
	{
		const QString path = OmemoAttachments::attachmentFilePath(account,href);
		QTimer::singleShot(0,this,[AHandler,path]() { AHandler(path); });
	}
	else
	{
		// The consumer requests are appended for every message embedding the
		// url; each of them is answered exactly once.
		FMediaRequests[href].append(AHandler);
		FAttachments->downloadAttachment(account,href);
	}
}void Omemo::setMediaConsumerActive(bool AActive)
{
	if (FMediaConsumerActive != AActive)
	{
		FMediaConsumerActive = AActive;
		LOG_INFO(QString("OMEMO media consumer (image previews) is %1").arg(AActive ? QStringLiteral("active") : QStringLiteral("inactive")));
	}
}

void Omemo::setMessageStatus(const QString &AMessageId, int AStatus)
{
	if (AMessageId.isEmpty())
		return;
	if (AStatus == StatusUnknown)
	{
		FMessageStatus.remove(AMessageId);
		return;
	}
	if (!FMessageStatus.contains(AMessageId) && FMessageStatus.size() >= 5000)
		FMessageStatus.clear();
	FMessageStatus.insert(AMessageId,AStatus);
}

int Omemo::messageStatus(const QString &AMessageId) const
{
	return FMessageStatus.value(AMessageId,StatusUnknown);
}

QString Omemo::statusName(int AStatus)
{
	switch (AStatus)
	{
	case StatusEncrypted: return QStringLiteral("encrypted");
	case StatusNotEncrypted: return QStringLiteral("not-encrypted");
	case StatusUnencryptedExpected: return QStringLiteral("unencrypted");
	case StatusPending: return QStringLiteral("pending");
	case StatusFailed: return QStringLiteral("failed");
	default: return QStringLiteral("unknown");
	}
}

int Omemo::statusFromName(const QString &AName)
{
	if (AName == QStringLiteral("encrypted"))
		return StatusEncrypted;
	if (AName == QStringLiteral("not-encrypted"))
		return StatusNotEncrypted;
	if (AName == QStringLiteral("unencrypted"))
		return StatusUnencryptedExpected;
	if (AName == QStringLiteral("pending"))
		return StatusPending;
	if (AName == QStringLiteral("failed"))
		return StatusFailed;
	return StatusUnknown;
}

void Omemo::markStanzaStatus(Stanza &AStanza, int AStatus)
{
	QDomElement marker = AStanza.firstElement(QStringLiteral("omemo"),
		QString::fromLatin1(OMEMO_NS_STATUS));
	if (marker.isNull())
		marker = AStanza.addElement(QStringLiteral("omemo"),QString::fromLatin1(OMEMO_NS_STATUS));
	marker.setAttribute(QStringLiteral("status"),statusName(AStatus));
}

void Omemo::markOutgoingStatus(const Jid &AStreamJid, Message &AMessage)
{
	const QString contactJid = AMessage.toJid().bare();
	OmemoAccount *account = accountForStream(AStreamJid);
	const bool supportsOmemo = contactSupportsOmemo(account,contactJid);
	int status = supportsOmemo ? StatusUnencryptedExpected : StatusNotEncrypted;
	if (account != nullptr && account->isStarted() && account->isChatEnabled(contactJid))
	{
		if (!account->isDeviceListKnown(contactJid))
			status = StatusPending;
		else if (account->hasEncryptableDevices(contactJid))
			status = account->sessionsReady(contactJid) ? StatusEncrypted : StatusPending;
	}
	setMessageStatus(AMessage.id(),status);
	markStanzaStatus(AMessage.stanza(),status);
}

bool Omemo::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml,
	QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	if (AOrder != MSHO_OMEMO)
		return false;

	AModifiedHtml = AHtml;
	if (FAttachments == nullptr || AWidget == nullptr)
		return true;

	QTextBrowser *browser = qobject_cast<QTextBrowser *>(AWidget);
	QTextDocument *doc = browser != nullptr ? browser->document() : nullptr;
	if (doc == nullptr)
		return true;

	// The anchors of the message are matched against the sanitized
	// attachment urls the same way the Image Viewer does it.  When the
	// decrypted file is in the cache the link is replaced by an inline image,
	// otherwise the download is scheduled and completed lazily.
	MessageDocumentContext context;
	bool contextRegistered = false;

	struct AnchorMatch
	{
		qsizetype start;
		QString token;
		QString href;
	};
	QList<AnchorMatch> matches;
	QRegularExpressionMatchIterator it = AnchorsRegex.globalMatch(AHtml);
	while (it.hasNext())
	{
		const QRegularExpressionMatch match = it.next();
		const QString rawHref = !match.captured(1).isNull() ? match.captured(1) : (!match.captured(2).isNull() ? match.captured(2) : match.captured(3));
		const QString href = decodedHtmlAttribute(rawHref).trimmed();
		OmemoAccount *account = nullptr;
		if (isAttachmentUrl(href,&account))
			matches.append({match.capturedStart(0),match.captured(0),href});
	}

	for (int i = matches.count()-1; i >= 0; --i)	// from the back: positions are stable
	{
		const AnchorMatch &anchor = matches.at(i);
		OmemoAccount *account = nullptr;
		isAttachmentUrl(anchor.href,&account);

		// The path is computed from the url alone and does not need the file
		// to exist yet; the extension is decided at the same way when the
		// download is stored.
		const QString prospectivePath = OmemoAttachments::attachmentFilePath(account,anchor.href);
		const bool imageFile = OmemoAttachments::isImageFilePath(prospectivePath);

		if (OmemoAttachments::isFileCached(account,anchor.href))
		{
			QString replacement;
			if (imageFile)
			{
				// The image viewer bound to this plugin shows its urls in the
				// gallery previews, the inline image would be a duplicate.
				if (FMediaConsumerActive)
					continue;
				const QString fileUrl = QUrl::fromLocalFile(prospectivePath).toString();
				replacement = QStringLiteral("<a href=\"%1\"><img src=\"%2\" alt=\"%3\"></a>")
					.arg(anchor.href.toHtmlEscaped(),fileUrl.toHtmlEscaped(),
						OmemoAttachments::attachmentFileName(anchor.href).toHtmlEscaped());
			}
			else
			{
				// A non-image file shows under its own name with the size;
				// the anchor keeps pointing at the original url.
				const QString label = OmemoAttachments::attachmentFileName(anchor.href)
					+ QLatin1String(" (")
					+ OmemoAttachments::formatFileSize(QFileInfo(prospectivePath).size())
					+ QLatin1String(")");
				replacement = QStringLiteral("<a href=\"%1\">%2</a>")
					.arg(anchor.href.toHtmlEscaped(),label.toHtmlEscaped());
			}
			AModifiedHtml.replace(anchor.start,anchor.token.size(),replacement);
		}
		else
		{
			if (FMediaConsumerActive && imageFile)
				continue;	// the image viewer will request the decrypted file itself

			// The file download is deduplicated, the image is inserted into
			// the document when the download completes.
			if (!contextRegistered && FMessageContextManager != nullptr)
				contextRegistered = FMessageContextManager->beginMessageContext(doc,context);
			FAttachments->downloadAttachment(account,anchor.href);
			if (contextRegistered)
				appendPendingAttachmentInsert(doc,context,anchor.href);
		}
	}

	return true;
}

QString Omemo::decodedHtmlAttribute(const QString &AValue)
{
	// Same limited unescape the Image Viewer uses for anchor hrefs: the named
	// references of the xml special characters and the numeric references.
	QString result;
	result.reserve(AValue.size());
	for (int i = 0; i < AValue.size();)
	{
		if (AValue.at(i) != QLatin1Char('&') || i + 2 >= AValue.size())
		{
			result.append(AValue.at(i));
			++i;
			continue;
		}
		const int semicolon = AValue.indexOf(QLatin1Char(';'),i + 2);
		if (semicolon < 0 || semicolon - i > 10)
		{
			result.append(AValue.at(i));
			++i;
			continue;
		}
		const QString ref = AValue.mid(i+1,semicolon - i - 1);
		QChar character;
		if (ref == QStringLiteral("amp")) character = QLatin1Char('&');
		else if (ref == QStringLiteral("lt")) character = QLatin1Char('<');
		else if (ref == QStringLiteral("gt")) character = QLatin1Char('>');
		else if (ref == QStringLiteral("quot")) character = QLatin1Char('"');
		else if (ref == QStringLiteral("apos")) character = QLatin1Char('\'');
		else character = QChar::Null;

		if (!character.isNull())
		{
			result.append(character);
			i = semicolon + 1;
			continue;
		}

		bool valid = false;
		int code = 0;
		if (ref.startsWith(QStringLiteral("#x"),Qt::CaseInsensitive))
			code = ref.mid(2).toInt(&valid,16);
		else if (ref.startsWith(QLatin1Char('#')))
			code = ref.mid(1).toInt(&valid,10);
		if (valid && code > 0 && code <= 0x10FFFF)
		{
			result.append(QChar(code));
			i = semicolon + 1;
		}
		else
		{
			result.append(AValue.at(i));
			++i;
		}
	}
	return result;
}

bool Omemo::isAttachmentUrl(const QString &ACleanUrl, OmemoAccount **AAccount) const
{
	if (AAccount != nullptr)
		*AAccount = nullptr;
	if (FAttachments == nullptr || ACleanUrl.isEmpty())
		return false;
	for (QHash<QUuid, OmemoAccount *>::const_iterator it=FAccounts.constBegin();
		it!=FAccounts.constEnd(); ++it)
	{
		if (FAttachments->isAttachment(it.value(),ACleanUrl))
		{
			if (AAccount != nullptr)
				*AAccount = it.value();
			return true;
		}
	}
	return false;
}

bool Omemo::findAnchorFragment(QTextDocument *ADocument, const MessageDocumentContext &AContext,
	const QString &AHref, const IMessageDocumentContextManager *AManager, QTextFragment &AFragment)
{
	if (ADocument == nullptr || AContext.document != ADocument || AHref.isEmpty()
		|| AManager == nullptr || !AManager->isMessageContextValid(AContext))
		return false;

	const int startPos = AContext.atStart ? 0 : AContext.marker.position() + 1;
	const int endPos = AManager->nextMessagePosition(AContext);
	for (QTextBlock block = ADocument->findBlock(startPos); block.isValid(); block = block.next())
	{
		if (endPos >= 0 && block.position() >= endPos)
			break;
		for (QTextBlock::Iterator it = block.begin(); !it.atEnd(); ++it)
		{
			const QTextFragment &fragment = it.fragment();
			const int fragStart = fragment.position();
			const int fragEnd = fragStart + fragment.length();
			if (fragEnd <= startPos || fragStart < startPos)
				continue;
			if (endPos >= 0 && fragStart >= endPos)
				break;
			if (endPos >= 0 && fragEnd > endPos)
				continue;
			const QTextCharFormat format = fragment.charFormat();
			if (format.isAnchor() && format.anchorHref() == AHref)
			{
				AFragment = fragment;
				return true;
			}
		}
	}
	return false;
}

bool Omemo::insertAttachmentImage(QTextDocument *ADocument, const MessageDocumentContext &AContext,
	const QString &ACleanUrl, const QString &ALocalFile, const IMessageDocumentContextManager *AManager)
{
	QTextFragment fragment;
	if (!findAnchorFragment(ADocument,AContext,ACleanUrl,AManager,fragment))
		return false;

	QTextCursor cursor(ADocument);
	cursor.setPosition(fragment.position() + fragment.length());

	if (OmemoAttachments::isImageFilePath(ALocalFile))
	{
		// Anchored image char format, so links and the context menu work
		// the same way as for the plain text links.
		QTextImageFormat format;
		format.setName(QUrl::fromLocalFile(ALocalFile).toString());
		format.setToolTip(OmemoAttachments::attachmentFileName(ACleanUrl));
		format.setAnchor(true);
		format.setAnchorHref(ACleanUrl);
		format.setProperty(TMI_IMAGE_LINK_URL,ACleanUrl);
		cursor.insertImage(format);
	}
	else
	{
		// A non-image file is shown under its own name: the anchor text (the
		// sanitized url) is replaced by "name (size)".  The anchor format is
		// preserved so the link stays clickable.
		const QString label = OmemoAttachments::attachmentFileName(ACleanUrl)
			+ QLatin1String(" (") + OmemoAttachments::formatFileSize(QFileInfo(ALocalFile).size())
			+ QLatin1String(")");
		cursor.setPosition(fragment.position());
		cursor.setPosition(fragment.position() + fragment.length(),QTextCursor::KeepAnchor);
		cursor.insertText(label,fragment.charFormat());
	}
	return true;
}

void Omemo::appendPendingAttachmentInsert(QTextDocument *ADocument,
	const MessageDocumentContext &AContext, const QString &ACleanUrl)
{
	for (int i = FPendingAttachmentInserts.count()-1; i >= 0; --i)
	{
		if (FPendingAttachmentInserts.at(i).document.isNull())
			FPendingAttachmentInserts.removeAt(i);
		else if (FPendingAttachmentInserts.at(i).url == ACleanUrl
			&& FPendingAttachmentInserts.at(i).context.document == ADocument)
			return;
	}

	AttachmentInsert insert;
	insert.document = ADocument;
	insert.context = AContext;
	insert.url = ACleanUrl;
	FPendingAttachmentInserts.append(insert);
}

void Omemo::onAttachmentReady(OmemoAccount *AAccount, const QString &ACleanUrl, bool ASuccess)
{
	// Failures are logged where they happen; nothing can be inserted then.
	if (AAccount != nullptr && FPendingSaves.contains(ACleanUrl))
	{
		const QString target = FPendingSaves.take(ACleanUrl);
		if (ASuccess)
			saveAttachment(AAccount,ACleanUrl,target);
	}

	if (!ASuccess)
	{
		// The waiting media consumers (image viewer) get the empty path;
		// copies are scheduled because the lambda outlives the list.
		const QList<MediaHandler> failed = FMediaRequests.take(ACleanUrl);
		for (const MediaHandler &item : failed)
		{
			const MediaHandler handler = item;
			QTimer::singleShot(0,this,[handler]() { handler(QString()); });
		}
		return;
	}

	// The waiting media consumers (image viewer galleries); copies of the
	// handlers are scheduled because the lambda outlives the list.
	const QList<MediaHandler> handlers = FMediaRequests.take(ACleanUrl);
	const QString readyPath = OmemoAttachments::attachmentFilePath(AAccount,ACleanUrl);
	for (const MediaHandler &item : handlers)
	{
		const MediaHandler handler = item;
		QTimer::singleShot(0,this,[handler,readyPath]() { handler(readyPath); });
	}

	for (int i = FPendingAttachmentInserts.count()-1; i >= 0; --i)
	{
		const AttachmentInsert &insert = FPendingAttachmentInserts.at(i);
		if (insert.document.isNull())
		{
			FPendingAttachmentInserts.removeAt(i);
			continue;
		}
		if (insert.url != ACleanUrl)
			continue;

		const QString localFile = OmemoAttachments::attachmentFilePath(AAccount,insert.url);
		if (insertAttachmentImage(insert.document,insert.context,insert.url,localFile,FMessageContextManager))
			FPendingAttachmentInserts.removeAt(i);
	}
}

bool Omemo::messageViewUrlOpen(int AOrder, IMessageViewWidget *AWidget, const QUrl &AUrl)
{
	Q_UNUSED(AWidget);
	if (AOrder != MVUHO_OMEMO)
		return false;

	// Only the sanitized attachment urls of this account are ours.  The href
	// the viewer emits is stored exactly as the anchor was created.
	const QString href = QString::fromUtf8(AUrl.toEncoded());
	OmemoAccount *account = nullptr;
	if (!isAttachmentUrl(href,&account))
		return false;

	// The remote file is encrypted and always useless for the browser or the
	// system opener: never let the default handler see it.
	if (OmemoAttachments::isFileCached(account,href))
	{
		LOG_STRM_INFO(account->streamJid(),QString("OMEMO attachment opened, url=%1").arg(href));
		return QDesktopServices::openUrl(QUrl::fromLocalFile(OmemoAttachments::attachmentFilePath(account,href)));
	}

	// Not downloaded yet: fetch and decrypt it, the user can click again.
	LOG_STRM_INFO(account->streamJid(),QString("OMEMO attachment download requested by click, url=%1").arg(href));
	FAttachments->downloadAttachment(account,href);
	return true;
}

QString Omemo::messageStyleContentSlot(int AOrder, QWidget *AWidget,
	const QString &ASlotName, bool ASlotInTemplate,
	const IMessageStyleContentOptions &AOptions)
{
	if (AOrder != MSHO_OMEMO || !ASlotInTemplate
		|| ASlotName != MSM_CONTENT_SLOT_MARKS)
		return QString();
	if (AWidget == nullptr
		|| (AOptions.kind != IMessageStyleContentOptions::KindMessage
			&& AOptions.kind != IMessageStyleContentOptions::KindMeCommand)
		|| AOptions.messageId.isEmpty())
		return QString();

	const int status = messageStatus(AOptions.messageId);
	if (status == StatusUnknown)
		return QString();

	QTextBrowser *browser = qobject_cast<QTextBrowser *>(AWidget);
	if (browser == nullptr)
		return QString();

	// The glyph goes into the same right-aligned marks slot as delivery
	// receipts and the edited pencil.  A deferred pass adds a right tab stop
	// when no other plugin has already created one for this line.
	QPointer<QTextBrowser> guard(browser);
	const QString messageId = AOptions.messageId;
	QTimer::singleShot(0,this,[this,guard,messageId]() {
		if (guard != nullptr)
			alignOmemoMark(guard,messageId);
	});

	return omemoMarkToken(messageId) + omemoMarkHtml(status);
}

void Omemo::alignOmemoMark(QTextBrowser *ABrowser, const QString &AMessageId) const
{
	if (ABrowser == nullptr || ABrowser->document() == nullptr)
		return;

	QTextDocument *document = ABrowser->document();
	const QString token = omemoMarkToken(AMessageId);
	QTextCursor found = document->find(token);
	if (found.isNull())
		return;

	const int tokenPos = found.selectionStart();
	QTextBlock block = document->findBlock(tokenPos);
	if (!block.isValid())
		return;

	const int relativePos = tokenPos - block.position();
	const bool hasTab = block.text().left(relativePos).contains(QChar(0x0009));
	QTextBlockFormat blockFormat = block.blockFormat();
	QList<QTextOption::Tab> tabs = blockFormat.tabPositions();
	bool hasRightTab = false;
	for (int i=0; i<tabs.count(); i++)
	{
		if (tabs.at(i).type == QTextOption::RightTab)
		{
			hasRightTab = true;
			break;
		}
	}

	const qreal usableWidth = ABrowser->viewport()->width() - 2*document->documentMargin();
	const char32_t lockCode = 0x1F512;
	const QString lockGlyph = QString::fromUcs4(&lockCode,1);
	const qreal markerWidth = QFontMetricsF(ABrowser->font()).horizontalAdvance(lockGlyph) + 8;

	if (!hasTab)
	{
		if (usableWidth <= markerWidth)
			return;
		QTextCursor tabCursor(document);
		tabCursor.setPosition(tokenPos);
		tabCursor.insertText(QChar(0x0009));
	}

	if (hasRightTab || usableWidth <= markerWidth)
		return;

	QTextCursor blockCursor(document);
	blockCursor.setPosition(tokenPos);
	blockFormat = blockCursor.block().blockFormat();
	tabs = blockFormat.tabPositions();
	QTextOption::Tab tab;
	tab.type = QTextOption::RightTab;
	tab.position = usableWidth - markerWidth;
	int rightTab = -1;
	for (int i=0; i<tabs.count(); i++)
	{
		if (tabs.at(i).type == QTextOption::RightTab)
		{
			rightTab = i;
			break;
		}
	}
	if (rightTab >= 0)
		tabs[rightTab] = tab;
	else
		tabs.append(tab);
	blockFormat.setTabPositions(tabs);
	blockCursor.setBlockFormat(blockFormat);
}

QString Omemo::omemoMarkHtml(int AStatus) const
{
	QString cssClass;
	QString color;
	QString title;
	QString glyph;
	switch (AStatus)
	{
	case StatusEncrypted:
		cssClass = MSMC_MARK_OMEMO;
		color = QStringLiteral("#1e8e3e");
		title = tr("OMEMO encrypted");
		// U+FE0E asks the font for the text (monochrome) presentation, so the
		// CSS color applies instead of the fixed yellow emoji color.
		glyph = QStringLiteral("&#128274;&#65038;"); // closed lock
		break;
	case StatusPending:
		cssClass = MSMC_MARK_OMEMO;
		color = QStringLiteral("#c58a00");
		title = tr("OMEMO encryption pending");
		glyph = QStringLiteral("&#8987;&#65038;"); // hourglass
		break;
	case StatusFailed:
		cssClass = MSMC_MARK_OMEMO_FAILED;
		color = QStringLiteral("#c62828");
		title = tr("OMEMO encryption/decryption failed");
		glyph = QStringLiteral("&#9888;&#65038;"); // warning sign
		break;
	case StatusNotEncrypted:
		cssClass = MSMC_MARK_OMEMO_OPEN;
		color = QStringLiteral("#9e9e9e");
		title = tr("Not encrypted");
		glyph = QStringLiteral("&#128275;&#65038;"); // open lock
		break;
	case StatusUnencryptedExpected:
		cssClass = MSMC_MARK_OMEMO_UNENCRYPTED;
		color = QStringLiteral("#c62828");
		title = tr("OMEMO is available for this contact, but the message is not encrypted");
		glyph = QStringLiteral("&#128275;&#65038;"); // open lock
		break;
	default:
		return QString();
	}

	return QStringLiteral("&nbsp;<span class=\"%1\" style=\"color:%2;font-size:90%;\" title=\"%3\">%4</span>")
		.arg(cssClass).arg(color).arg(title.toHtmlEscaped()).arg(glyph);
}

void Omemo::replaceBody(Stanza &AStanza, const QString &ABody)
{
	QDomNodeList bodies = AStanza.element().elementsByTagName(QStringLiteral("body"));
	for (int i=bodies.count()-1; i>=0; --i)
		AStanza.element().removeChild(bodies.at(i));

	QString body = ABody;
	Stanza::replaceInvalidXmlChars(body);
	if (!body.isEmpty())
		AStanza.element().appendChild(AStanza.createElement(QStringLiteral("body")))
			.appendChild(AStanza.createTextNode(body));
}
