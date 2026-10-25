#include "omemoaccount.h"

#include "omemodefinitions.h"
#include "omemoenvelope.h"
#include "omemostore.h"

#include <definitions/namespaces.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <utils/xmpperror.h>

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>

OmemoAccount::OmemoAccount(Omemo *APlugin, IAccount *AAccount,
	IStanzaProcessor *AStanzaProcessor, IPEPManager *APepManager) :
	QObject(nullptr),
	FPlugin(APlugin),
	FAccount(AAccount),
	FStanzaProcessor(AStanzaProcessor),
	FPepManager(APepManager),
	FStore(nullptr),
	FCrypto(nullptr),
	FPepHandler(-1),
	FStarted(false),
	FPublishingDeviceList(false),
	FPublishingBundle(false)
{
}

OmemoAccount::~OmemoAccount()
{
	stop();
}

QUuid OmemoAccount::accountId() const
{
	return FAccount != nullptr ? FAccount->accountId() : QUuid();
}

bool OmemoAccount::start(bool AInitializeTransport)
{
	if (FStarted)
		return true;
	if (FAccount == nullptr || FStanzaProcessor == nullptr || FPepManager == nullptr)
		return false;

	FStreamJid = FAccount->streamJid();
	if (!FStreamJid.isValid())
		FStreamJid = FAccount->accountJid();
	if (!FStreamJid.isValid())
		return false;

	const QByteArray cryptKey = Options::cryptKey();
	if (cryptKey.isEmpty())
	{
		emit error(tr("The profile encryption key is not available"));
		return false;
	}

	QDir directory(Options::filesPath() + QStringLiteral("/omemo"));
	if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
	{
		emit error(tr("Cannot create the OMEMO data directory"));
		return false;
	}

	FStorePath = directory.filePath(accountId().toString(QUuid::WithoutBraces) + QStringLiteral(".dat"));
	FStore = new OmemoStore(FStorePath,cryptKey);
	FCrypto = new OmemoCrypto(FStore);

	QString error;
	if (!FCrypto->init(&error) || !FCrypto->ensurePreKeys(OMEMO_PREKEYS_MAX,&error))
	{
		emit this->error(tr("Cannot initialize OMEMO: %1").arg(error));
		stop();
		return false;
	}

	FStarted = true;
	FPepHandler = FPepManager->insertNodeHandler(QString::fromLatin1(OMEMO_NODE_DEVICELIST),this);

	if (!AInitializeTransport && FAccount->xmppStream() != nullptr
		&& FAccount->xmppStream()->isConnected())
		AInitializeTransport = true;

	if (AInitializeTransport)
	{
		subscribeDeviceList(bareJid());
		requestDeviceList(bareJid());
		publishBundle();

		// PEP service discovery is often not ready at stream-open time, so retry
		// the initial publication a couple of times.
		QTimer::singleShot(5000,this,[this]() {
			if (FStarted)
			{
				publishBundle();
				publishOwnDevice();
			}
		});
		QTimer::singleShot(15000,this,[this]() {
			if (FStarted)
			{
				publishBundle();
				publishOwnDevice();
			}
		});
	}

	LOG_STRM_INFO(FStreamJid,QString("OMEMO started, device=%1").arg(FCrypto->deviceId()));
	return true;
}

void OmemoAccount::streamOpened(const Jid &AStreamJid)
{
	if (!FStarted || !AStreamJid.isValid())
		return;

	FStreamJid = AStreamJid;
	const QString ownJid = bareJid();
	FFailedDeviceLists.clear();
	FFailedBundles.clear();

	// The account may have been initialized before a connection was ready so
	// that local archive history could be decrypted.  Re-announce and refresh
	// the PEP state now that stanzas can actually be sent.
	FSubscriptions.remove(ownJid);
	FPendingDeviceLists.remove(ownJid);
	subscribeDeviceList(ownJid);
	requestDeviceList(ownJid);
	publishBundle();
	publishOwnDevice();

	// Discovery may not be ready immediately after reconnect.
	QTimer::singleShot(5000,this,[this]() {
		if (FStarted)
		{
			publishBundle();
			publishOwnDevice();
		}
	});
}

void OmemoAccount::streamClosed()
{
	// Keep the crypto store loaded: the user may open local archive history
	// while the account is disconnected.  Only transport-related state is
	// reset; streamOpened() re-subscribes and republishes later.
	FSubscriptions.clear();
	FPendingDeviceLists.clear();
	FPendingBundles.clear();
	FConfiguringNodes.clear();
}

void OmemoAccount::stop()
{
	if (FPepHandler >= 0 && FPepManager != nullptr)
		FPepManager->removeNodeHandler(FPepHandler);
	FPepHandler = -1;

	if (FCrypto != nullptr)
	{
		FCrypto->flush();
		delete FCrypto;
		FCrypto = nullptr;
	}
	delete FStore;
	FStore = nullptr;

	FDeviceLists.clear();
	FDeviceListTimes.clear();
	FKnownDeviceLists.clear();
	FBundles.clear();
	FRequests.clear();
	FPendingDeviceLists.clear();
	FFailedDeviceLists.clear();
	FPendingBundles.clear();
	FFailedBundles.clear();
	FSubscriptions.clear();
	FConfiguringNodes.clear();
	FStarted = false;
}

QList<quint32> OmemoAccount::devices(const QString &AJid) const
{
	return FDeviceLists.value(Jid(AJid).bare());
}

QList<quint32> OmemoAccount::encryptableDevices(const QString &AJid) const
{
	const QString jid = Jid(AJid).bare();
	QList<quint32> result;
	for (quint32 deviceId : FDeviceLists.value(jid))
	{
		if (deviceTrust(jid,deviceId) == OmemoStore::TrustTrusted)
			result.append(deviceId);
	}
	return result;
}

bool OmemoAccount::isDeviceListKnown(const QString &AJid) const
{
	return FKnownDeviceLists.contains(Jid(AJid).bare());
}

bool OmemoAccount::hasDevices(const QString &AJid) const
{
	return !FDeviceLists.value(Jid(AJid).bare()).isEmpty();
}

bool OmemoAccount::hasEncryptableDevices(const QString &AJid) const
{
	return !encryptableDevices(AJid).isEmpty();
}

bool OmemoAccount::sessionsReady(const QString &AJid) const
{
	const QString jid = Jid(AJid).bare();
	if (!FKnownDeviceLists.contains(jid) || FCrypto == nullptr)
		return false;
	for (quint32 deviceId : FDeviceLists.value(jid))
	{
		if (deviceTrust(jid,deviceId) == OmemoStore::TrustTrusted
			&& !FCrypto->hasSession(jid,deviceId))
			return false;
	}
	return true;
}

bool OmemoAccount::isChatEnabled(const QString &AJid) const
{
	const QString jid = Jid(AJid).bare();
	if (!FStarted || FStore == nullptr || jid.isEmpty())
		return false;
	return !FStore->data().disabledChats.contains(jid);
}

void OmemoAccount::setChatEnabled(const QString &AJid, bool AEnabled)
{
	const QString jid = Jid(AJid).bare();
	if (FStore == nullptr || jid.isEmpty())
		return;
	if (AEnabled)
		FStore->data().disabledChats.remove(jid);
	else
		FStore->data().disabledChats.insert(jid);
	FStore->markDirty();
	FStore->save();
	emit stateChanged(jid);
}

int OmemoAccount::deviceTrust(const QString &AJid, quint32 ADeviceId) const
{
	if (FStore == nullptr)
		return OmemoStore::TrustUndecided;
	const QHash<quint32, OmemoStore::Device> devices = FStore->data().devices.value(Jid(AJid).bare());
	return devices.contains(ADeviceId) ? devices.value(ADeviceId).trustLevel : OmemoStore::TrustUndecided;
}

void OmemoAccount::setDeviceTrust(const QString &AJid, quint32 ADeviceId, int ATrustLevel)
{
	if (FStore == nullptr || ADeviceId == 0)
		return;
	const QString jid = Jid(AJid).bare();
	OmemoStore::Device &device = FStore->data().devices[jid][ADeviceId];
	device.trustLevel = ATrustLevel;
	FFailedBundles.remove(bundleCacheKey(jid,ADeviceId));
	FStore->markDirty();
	FStore->save();
	emit stateChanged(jid);
}

QString OmemoAccount::deviceFingerprint(const QString &AJid, quint32 ADeviceId) const
{
	if (FStore == nullptr)
		return QString();
	QByteArray key = FStore->data().devices.value(Jid(AJid).bare()).value(ADeviceId).keyId;
	if (key.size() == 33 && (unsigned char)key.at(0) == 0x05)
		key.remove(0,1);
	if (key.isEmpty())
		return QString();

	const QString hex = QString::fromLatin1(key.toHex()).toUpper();
	QStringList groups;
	for (int i=0; i<hex.size(); i+=8)
		groups.append(hex.mid(i,8));
	return groups.join(QLatin1Char(' '));
}

QString OmemoAccount::ownFingerprint() const
{
	if (FCrypto == nullptr)
		return QString();
	QByteArray key = FCrypto->identityPublicKey();
	if (key.size() != 32)
		return QString();
	const QString hex = QString::fromLatin1(key.toHex()).toUpper();
	QStringList groups;
	for (int i=0; i<hex.size(); i+=8)
		groups.append(hex.mid(i,8));
	return groups.join(QLatin1Char(' '));
}

bool OmemoAccount::ensureDeviceList(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	if (jid.isEmpty())
		return false;
	if (FKnownDeviceLists.contains(jid))
	{
		const qint64 now = QDateTime::currentSecsSinceEpoch();
		if (now - FDeviceListTimes.value(jid,0) > OMEMO_DEVICELIST_TTL
			&& !FPendingDeviceLists.contains(jid)
			&& !FFailedDeviceLists.contains(jid))
			requestDeviceList(jid);
		return true;
	}
	if (FFailedDeviceLists.contains(jid) || FPendingDeviceLists.contains(jid))
		return false;
	requestDeviceList(jid);
	return false;
}

void OmemoAccount::refreshDeviceList(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	if (jid.isEmpty())
		return;
	FFailedDeviceLists.remove(jid);
	for (auto it=FFailedBundles.begin(); it!=FFailedBundles.end(); )
	{
		if (it->startsWith(jid + QLatin1Char('/')))
			it = FFailedBundles.erase(it);
		else
			++it;
	}
	FKnownDeviceLists.remove(jid);
	requestDeviceList(jid);
}

void OmemoAccount::fetchDeviceBundles(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	for (quint32 deviceId : FDeviceLists.value(jid))
	{
		const QString cacheKey = bundleCacheKey(jid,deviceId);
		if (!FBundles.value(jid).contains(deviceId)
			&& !FPendingBundles.contains(cacheKey)
			&& !FFailedBundles.contains(cacheKey))
			requestBundle(jid,deviceId);
	}
}

bool OmemoAccount::ensureSessions(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	if (FCrypto == nullptr || !FCrypto->isReady())
		return false;
	if (!ensureDeviceList(jid))
		return false;

	bool ready = true;
	for (quint32 deviceId : FDeviceLists.value(jid))
	{
		if (deviceTrust(jid,deviceId) != OmemoStore::TrustTrusted)
			continue;
		if (FCrypto->hasSession(jid,deviceId))
			continue;

		if (FBundles.value(jid).contains(deviceId))
		{
			QString error;
			if (!FCrypto->processBundle(jid,deviceId,FBundles.value(jid).value(deviceId),&error))
			{
				LOG_STRM_WARNING(FStreamJid,QString("Cannot process OMEMO bundle for %1/%2: %3")
					.arg(jid).arg(deviceId).arg(error));
				ready = false;
			}
		}
		else
		{
			const QString cacheKey = bundleCacheKey(jid,deviceId);
			if (!FPendingBundles.contains(cacheKey) && !FFailedBundles.contains(cacheKey))
				requestBundle(jid,deviceId);
			ready = false;
		}
	}

	if (ready)
		FCrypto->flush();
	return ready;
}

void OmemoAccount::cacheMessage(const QString &AMessageId, const QByteArray &APlain)
{
	if (FStore == nullptr || AMessageId.isEmpty() || APlain.isEmpty())
		return;
	OmemoStore::Data &data = FStore->data();
	data.messageCache.insert(AMessageId,APlain);
	while (data.messageCache.size() > 6000)
		data.messageCache.erase(data.messageCache.begin());
	FStore->markDirty();
	if (FCrypto != nullptr)
		FCrypto->flush();
}

bool OmemoAccount::cachedMessage(const QString &AMessageId, QByteArray &APlain) const
{
	if (FStore == nullptr || AMessageId.isEmpty())
		return false;
	QHash<QString, QByteArray>::const_iterator it = FStore->data().messageCache.constFind(AMessageId);
	if (it == FStore->data().messageCache.constEnd())
		return false;
	APlain = it.value();
	return !APlain.isEmpty();
}

bool OmemoAccount::ensureSelfSession(QString *AError)
{
	if (FCrypto == nullptr || !FCrypto->isReady())
	{
		if (AError != nullptr)
			*AError = tr("OMEMO is not initialized");
		return false;
	}
	const QString ownJid = bareJid();
	const quint32 ownDevice = FCrypto->deviceId();
	if (ownJid.isEmpty() || ownDevice == 0)
	{
		if (AError != nullptr)
			*AError = tr("OMEMO device is not initialized");
		return false;
	}
	if (FCrypto->hasSession(ownJid,ownDevice))
		return true;

	OmemoCrypto::PreKeyBundle bundle;
	if (!FCrypto->createBundle(bundle,AError))
		return false;
	return FCrypto->processBundle(ownJid,ownDevice,bundle,AError);
}

bool OmemoAccount::encryptMessage(const QString &ATargetJid, const QByteArray &APlain,
	QDomElement &AEncrypted, QString *AError)
{
	AEncrypted = QDomElement();
	if (FCrypto == nullptr || !FCrypto->isReady())
	{
		if (AError != nullptr)
			*AError = tr("OMEMO is not initialized");
		return false;
	}

	const QString targetJid = Jid(ATargetJid).bare();
	const QString ownJid = bareJid();
	QList<OmemoEnvelope::Target> targets;
	QSet<QString> used;

	// A self-session is needed so copies of our own outgoing messages can be
	// decrypted later from MAM/local history, even if the PEP device list is
	// unavailable.  Failure here must not block delivery to the contact.
	if (!ensureSelfSession())
		LOG_STRM_WARNING(FStreamJid,QStringLiteral("Cannot create OMEMO self-session"));

	const auto appendTargets = [&](const QString &AJid, bool AOnlyReady)
	{
		for (quint32 deviceId : encryptableDevices(AJid))
		{
			if (AOnlyReady && !FCrypto->hasSession(AJid,deviceId))
				continue;
			const QString key = AJid + QLatin1Char('/') + QString::number(deviceId);
			if (used.contains(key))
				continue;
			used.insert(key);
			OmemoEnvelope::Target target;
			target.jid = AJid;
			target.deviceId = deviceId;
			targets.append(target);
		}
	};
	appendTargets(targetJid,false);

	// Always include the current device when its self-session exists.  It is
	// intentionally independent of the own device list: that list is often
	// unavailable while offline or before the first PEP fetch.
	const quint32 ownDevice = FCrypto->deviceId();
	if (ownDevice != 0 && FCrypto->hasSession(ownJid,ownDevice))
	{
		const QString key = ownJid + QLatin1Char('/') + QString::number(ownDevice);
		if (!used.contains(key))
		{
			used.insert(key);
			OmemoEnvelope::Target target;
			target.jid = ownJid;
			target.deviceId = ownDevice;
			targets.append(target);
		}
	}

	if (targetJid != ownJid)
	{
		// Other own devices are useful for carbons, but a missing session for
		// one of them must not block delivery to the actual recipient.
		appendTargets(ownJid,true);
	}

	if (targets.isEmpty())
	{
		if (AError != nullptr)
			*AError = tr("No OMEMO devices are available for encryption");
		return false;
	}

	QStringList targetInfo;
	for (const OmemoEnvelope::Target &target : targets)
		targetInfo.append(QStringLiteral("%1/%2").arg(target.jid).arg(target.deviceId));
	LOG_STRM_INFO(FStreamJid,QString("OMEMO encrypt targets=%1").arg(targetInfo.join(QLatin1Char(','))));

	OmemoEnvelope::Data data;
	if (!OmemoEnvelope::encrypt(FCrypto,targets,APlain,data,AError))
		return false;

	QDomDocument document;
	AEncrypted = OmemoEnvelope::toXml(document,data);
	return !AEncrypted.isNull();
}

bool OmemoAccount::decryptMessage(const QString &ASenderJid, const QDomElement &AEncrypted,
	QByteArray &APlain, QString *AError)
{
	APlain.clear();
	if (FCrypto == nullptr || !FCrypto->isReady())
	{
		if (AError != nullptr)
			*AError = tr("OMEMO is not initialized");
		return false;
	}

	OmemoEnvelope::Data data;
	if (!OmemoEnvelope::fromXml(AEncrypted,data,AError))
		return false;
	if (!OmemoEnvelope::decrypt(FCrypto,data,ASenderJid,APlain,AError))
		return false;

	const QString senderJid = Jid(ASenderJid).bare();
	if (!senderJid.isEmpty() && data.sid != 0)
	{
		if (!isDeviceListKnown(senderJid))
		{
			if (!FPendingDeviceLists.contains(senderJid))
				requestDeviceList(senderJid);
		}
		else if (!FDeviceLists.value(senderJid).contains(data.sid))
		{
			FDeviceLists[senderJid].append(data.sid);
			registerDevices(senderJid,QList<quint32>() << data.sid);
			emit stateChanged(senderJid);
		}
	}

	if (FCrypto->preKeyCount() < OMEMO_PREKEYS_MIN)
		refreshOwnBundle();
	return true;
}

void OmemoAccount::publishOwnDevice()
{
	if (!FStarted || FCrypto == nullptr || FPublishingDeviceList)
		return;

	const QString ownJid = bareJid();
	if (!FKnownDeviceLists.contains(ownJid))
	{
		if (!FPendingDeviceLists.contains(ownJid))
			requestDeviceList(ownJid);
		return;
	}

	// Only devices whose private keys and bundles are actually present in this
	// store may be advertised.  Old device ids left in the PEP list have no
	// bundle here; remote clients then either skip the missing bundle or fail
	// to build a session for the current device.
	QList<quint32> list;
	if (FCrypto->deviceId() != 0)
		list.append(FCrypto->deviceId());
	FDeviceLists[ownJid] = list;
	registerDevices(ownJid,list);

	LOG_STRM_INFO(FStreamJid,QString("OMEMO publishing own device list: %1")
		.arg(QStringList([&]() { QStringList ids; for (quint32 id : list) ids.append(QString::number(id)); return ids; }()).join(QLatin1Char(','))));

	FPublishingDeviceList = true;
	publishDeviceList(list);
	FPublishingDeviceList = false;
	publishBundle();
}

void OmemoAccount::refreshOwnBundle()
{
	if (!FStarted || FCrypto == nullptr)
		return;
	QString error;
	if (!FCrypto->ensurePreKeys(OMEMO_PREKEYS_MAX,&error))
		LOG_STRM_WARNING(FStreamJid,QString("Cannot refresh OMEMO pre-keys: %1").arg(error));
	publishBundle();
}

bool OmemoAccount::publishPepItem(const QString &ANode, const QDomElement &AItem)
{
	if (FStanzaProcessor == nullptr || FStreamJid.isEmpty())
		return false;

	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET).setUniqueId();
	QDomElement pubsub = iq.addElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
	QDomElement publish = pubsub.appendChild(iq.createElement(QStringLiteral("publish"))).toElement();
	publish.setAttribute(QStringLiteral("node"),ANode);
	publish.appendChild(AItem.cloneNode(true));
	if (FStanzaProcessor->sendStanzaOut(FStreamJid,iq))
	{
		LOG_STRM_INFO(FStreamJid,QString("OMEMO PEP item published, node=%1").arg(ANode));
		return true;
	}
	LOG_STRM_WARNING(FStreamJid,QString("Failed to send OMEMO PEP publish, node=%1").arg(ANode));
	return false;
}

void OmemoAccount::publishDeviceList(const QList<quint32> &ADevices)
{
	if (FPepManager == nullptr || FStreamJid.isEmpty())
		return;

	QDomDocument document;
	QDomElement item = document.createElement(QStringLiteral("item"));
	item.setAttribute(QStringLiteral("id"),QStringLiteral("current"));
	QDomElement list = document.createElement(QStringLiteral("list"));
	list.setAttribute(QStringLiteral("xmlns"),QString::fromLatin1(OMEMO_NS));

	QSet<quint32> unique;
	for (quint32 deviceId : ADevices)
	{
		if (deviceId == 0 || unique.contains(deviceId))
			continue;
		unique.insert(deviceId);
		QDomElement device = document.createElement(QStringLiteral("device"));
		device.setAttribute(QStringLiteral("id"),QString::number(deviceId));
		list.appendChild(device);
	}
	item.appendChild(list);

	if (!publishPepItem(QString::fromLatin1(OMEMO_NODE_DEVICELIST),item))
	{
		LOG_STRM_WARNING(FStreamJid,QString("Cannot publish OMEMO device list"));
		return;
	}
	configureNodeOpen(QString::fromLatin1(OMEMO_NODE_DEVICELIST));
}

bool OmemoAccount::publishBundle()
{
	if (!FStarted || FCrypto == nullptr || FPepManager == nullptr || FPublishingBundle)
		return false;

	quint32 signedPreKeyId = 0;
	QByteArray signedPreKey;
	QByteArray signature;
	if (!FCrypto->signedPreKeyInfo(signedPreKeyId,signedPreKey,signature))
	{
		QString error;
		if (!FCrypto->ensurePreKeys(OMEMO_PREKEYS_MAX,&error)
			|| !FCrypto->signedPreKeyInfo(signedPreKeyId,signedPreKey,signature))
		{
			LOG_STRM_WARNING(FStreamJid,QString("Cannot create OMEMO signed pre-key: %1").arg(error));
			return false;
		}
	}

	const QByteArray identityKey = FCrypto->identityKeySerialized();
	const QList<OmemoCrypto::PreKeyInfo> preKeys = FCrypto->preKeys();
	if (identityKey.isEmpty() || preKeys.isEmpty())
	{
		LOG_STRM_WARNING(FStreamJid,QString("Cannot publish incomplete OMEMO bundle"));
		return false;
	}

	QDomDocument document;
	QDomElement item = document.createElement(QStringLiteral("item"));
	item.setAttribute(QStringLiteral("id"),QStringLiteral("current"));
	QDomElement bundle = document.createElement(QStringLiteral("bundle"));
	bundle.setAttribute(QStringLiteral("xmlns"),QString::fromLatin1(OMEMO_NS));

	QDomElement signedPreKeyElement = document.createElement(QStringLiteral("signedPreKeyPublic"));
	signedPreKeyElement.setAttribute(QStringLiteral("signedPreKeyId"),QString::number(signedPreKeyId));
	signedPreKeyElement.appendChild(document.createTextNode(QString::fromLatin1(signedPreKey.toBase64())));
	bundle.appendChild(signedPreKeyElement);

	QDomElement signatureElement = document.createElement(QStringLiteral("signedPreKeySignature"));
	signatureElement.appendChild(document.createTextNode(QString::fromLatin1(signature.toBase64())));
	bundle.appendChild(signatureElement);

	QDomElement identityElement = document.createElement(QStringLiteral("identityKey"));
	identityElement.appendChild(document.createTextNode(QString::fromLatin1(identityKey.toBase64())));
	bundle.appendChild(identityElement);

	QDomElement preKeysElement = document.createElement(QStringLiteral("prekeys"));
	for (const OmemoCrypto::PreKeyInfo &preKey : preKeys)
	{
		QDomElement preKeyElement = document.createElement(QStringLiteral("preKeyPublic"));
		preKeyElement.setAttribute(QStringLiteral("preKeyId"),QString::number(preKey.id));
		preKeyElement.appendChild(document.createTextNode(QString::fromLatin1(preKey.publicKey.toBase64())));
		preKeysElement.appendChild(preKeyElement);
	}
	bundle.appendChild(preKeysElement);
	item.appendChild(bundle);

	FPublishingBundle = true;
	const QString node = bundleNode(FCrypto->deviceId());
	const bool published = publishPepItem(node,item);
	FPublishingBundle = false;

	if (!published)
	{
		LOG_STRM_WARNING(FStreamJid,QString("Cannot publish OMEMO bundle, node=%1").arg(node));
		return false;
	}

	configureNodeOpen(node);
	return true;
}

void OmemoAccount::subscribeDeviceList(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	if (jid.isEmpty() || FStanzaProcessor == nullptr || FSubscriptions.contains(jid))
		return;

	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET).setUniqueId();
	if (!isOwnJid(jid))
		iq.setTo(jid);
	QDomElement pubsub = iq.addElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
	QDomElement subscribe = pubsub.appendChild(iq.createElement(QStringLiteral("subscribe"))).toElement();
	subscribe.setAttribute(QStringLiteral("node"),QString::fromLatin1(OMEMO_NODE_DEVICELIST));
	subscribe.setAttribute(QStringLiteral("jid"),FStreamJid.full());
	if (FStanzaProcessor->sendStanzaOut(FStreamJid,iq))
		FSubscriptions.insert(jid);
}

bool OmemoAccount::requestDeviceList(const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	if (jid.isEmpty())
		return false;
	FPendingDeviceLists.insert(jid);
	const Jid target = isOwnJid(jid) ? Jid() : Jid(jid);
	if (!sendPubsubItemsRequest(target,QString::fromLatin1(OMEMO_NODE_DEVICELIST),
		RequestDeviceList,jid,0))
	{
		FPendingDeviceLists.remove(jid);
		return false;
	}
	return true;
}

bool OmemoAccount::requestBundle(const QString &AJid, quint32 ADeviceId)
{
	const QString jid = Jid(AJid).bare();
	if (jid.isEmpty() || ADeviceId == 0)
		return false;
	const QString cacheKey = bundleCacheKey(jid,ADeviceId);
	FPendingBundles.insert(cacheKey);
	const Jid target = isOwnJid(jid) ? Jid() : Jid(jid);
	if (!sendPubsubItemsRequest(target,bundleNode(ADeviceId),RequestBundle,jid,ADeviceId))
	{
		FPendingBundles.remove(cacheKey);
		return false;
	}
	return true;
}

bool OmemoAccount::requestNodeConfig(const QString &ANode)
{
	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_GET).setUniqueId();
	QDomElement pubsub = iq.addElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
	QDomElement configure = pubsub.appendChild(iq.createElement(QStringLiteral("configure"))).toElement();
	configure.setAttribute(QStringLiteral("node"),ANode);

	RequestInfo info;
	info.type = RequestNodeConfig;
	info.node = ANode;
	FRequests.insert(iq.id(),info);
	if (!FStanzaProcessor->sendStanzaRequest(this,FStreamJid,iq,OMEMO_PEP_REQUEST_TIMEOUT))
	{
		FRequests.remove(iq.id());
		return false;
	}
	return true;
}

bool OmemoAccount::submitNodeConfig(const QString &ANode, const QDomElement &AForm)
{
	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_SET).setUniqueId();
	QDomElement pubsub = iq.addElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
	QDomElement configure = pubsub.appendChild(iq.createElement(QStringLiteral("configure"))).toElement();
	configure.setAttribute(QStringLiteral("node"),ANode);
	configure.appendChild(AForm.cloneNode(true));

	RequestInfo info;
	info.type = RequestNodeConfigSubmit;
	info.node = ANode;
	FRequests.insert(iq.id(),info);
	FConfiguringNodes.insert(ANode);
	if (!FStanzaProcessor->sendStanzaRequest(this,FStreamJid,iq,OMEMO_PEP_REQUEST_TIMEOUT))
	{
		FRequests.remove(iq.id());
		FConfiguringNodes.remove(ANode);
		return false;
	}
	return true;
}

void OmemoAccount::configureNodeOpen(const QString &ANode)
{
	if (ANode.isEmpty() || FConfiguringNodes.contains(ANode))
		return;
	requestNodeConfig(ANode);
}

bool OmemoAccount::sendPubsubItemsRequest(const Jid &ATo, const QString &ANode,
	RequestType AType, const QString &AJid, quint32 ADeviceId)
{
	Stanza iq(STANZA_KIND_IQ);
	iq.setType(STANZA_TYPE_GET).setUniqueId();
	if (ATo.isValid())
		iq.setTo(ATo.full());
	QDomElement pubsub = iq.addElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
	QDomElement items = pubsub.appendChild(iq.createElement(QStringLiteral("items"))).toElement();
	items.setAttribute(QStringLiteral("node"),ANode);

	RequestInfo info;
	info.type = AType;
	info.jid = AJid;
	info.deviceId = ADeviceId;
	info.node = ANode;
	FRequests.insert(iq.id(),info);
	if (!FStanzaProcessor->sendStanzaRequest(this,FStreamJid,iq,OMEMO_PEP_REQUEST_TIMEOUT))
	{
		FRequests.remove(iq.id());
		return false;
	}
	return true;
}

void OmemoAccount::parseDeviceList(const QDomElement &AItems, const QString &AJid)
{
	const QString jid = Jid(AJid).bare();
	FFailedDeviceLists.remove(jid);
	QList<quint32> devices;

	QDomElement item = AItems.firstChildElement(QStringLiteral("item"));
	if (!item.isNull())
	{
		QDomElement list = item.firstChildElement(QStringLiteral("list"));
		if (list.isNull())
			list = item.firstChildElement(QStringLiteral("devices"));
		if (list.isNull())
			list = item.firstChildElement();
		for (QDomElement device = list.firstChildElement(QStringLiteral("device"));
			!device.isNull(); device = device.nextSiblingElement(QStringLiteral("device")))
		{
			bool ok = false;
			quint32 id = device.attribute(QStringLiteral("id")).toUInt(&ok);
			if (ok && id != 0 && !devices.contains(id))
				devices.append(id);
		}
	}

	FDeviceLists[jid] = devices;
	FDeviceListTimes[jid] = QDateTime::currentSecsSinceEpoch();
	FKnownDeviceLists.insert(jid);
	registerDevices(jid,devices);

	if (isOwnJid(jid) && FCrypto != nullptr)
	{
		if (!devices.contains(FCrypto->deviceId()))
			publishOwnDevice();
		else
			publishBundle();
	}
}

void OmemoAccount::parseBundle(const QDomElement &AItems, const QString &AJid, quint32 ADeviceId)
{
	const QString jid = Jid(AJid).bare();
	FFailedBundles.remove(bundleCacheKey(jid,ADeviceId));
	QDomElement item = AItems.firstChildElement(QStringLiteral("item"));
	QDomElement bundle = item.firstChildElement(QStringLiteral("bundle"));
	if (bundle.isNull())
	{
		LOG_STRM_WARNING(FStreamJid,QString("OMEMO bundle item is missing for %1/%2").arg(jid).arg(ADeviceId));
		return;
	}

	QDomElement signedPreKeyElement = bundle.firstChildElement(QStringLiteral("signedPreKeyPublic"));
	QDomElement signatureElement = bundle.firstChildElement(QStringLiteral("signedPreKeySignature"));
	QDomElement identityElement = bundle.firstChildElement(QStringLiteral("identityKey"));
	QDomElement preKeysElement = bundle.firstChildElement(QStringLiteral("prekeys"));

	QList<QDomElement> preKeyElements;
	for (QDomElement element = preKeysElement.firstChildElement(QStringLiteral("preKeyPublic"));
		!element.isNull(); element = element.nextSiblingElement(QStringLiteral("preKeyPublic")))
		preKeyElements.append(element);
	QDomElement preKeyElement;
	if (!preKeyElements.isEmpty())
		preKeyElement = preKeyElements.at(QRandomGenerator::global()->bounded(preKeyElements.size()));

	bool signedIdOk = false;
	bool preKeyIdOk = false;
	OmemoCrypto::PreKeyBundle preKeyBundle;
	preKeyBundle.registrationId = ADeviceId;
	preKeyBundle.deviceId = ADeviceId;
	preKeyBundle.signedPreKeyId = signedPreKeyElement.attribute(QStringLiteral("signedPreKeyId")).toUInt(&signedIdOk);
	preKeyBundle.signedPreKey = QByteArray::fromBase64(signedPreKeyElement.text().trimmed().toLatin1());
	preKeyBundle.signedPreKeySignature = QByteArray::fromBase64(signatureElement.text().trimmed().toLatin1());
	preKeyBundle.identityKey = QByteArray::fromBase64(identityElement.text().trimmed().toLatin1());
	preKeyBundle.preKeyId = preKeyElement.attribute(QStringLiteral("preKeyId")).toUInt(&preKeyIdOk);
	preKeyBundle.preKey = QByteArray::fromBase64(preKeyElement.text().trimmed().toLatin1());

	if (!signedIdOk || !preKeyIdOk || preKeyBundle.preKey.isEmpty()
		|| preKeyBundle.signedPreKey.isEmpty() || preKeyBundle.signedPreKeySignature.isEmpty()
		|| preKeyBundle.identityKey.isEmpty())
	{
		LOG_STRM_WARNING(FStreamJid,QString("Invalid OMEMO bundle for %1/%2").arg(jid).arg(ADeviceId));
		return;
	}

	FBundles[jid].insert(ADeviceId,preKeyBundle);
	registerDevices(jid,QList<quint32>() << ADeviceId);
	if (FStore != nullptr)
	{
		OmemoStore::Device &device = FStore->data().devices[jid][ADeviceId];
		if (device.keyId.isEmpty())
		{
			device.keyId = preKeyBundle.identityKey;
			FStore->markDirty();
			FStore->save();
		}
	}
}

void OmemoAccount::registerDevices(const QString &AJid, const QList<quint32> &ADevices)
{
	if (FStore == nullptr)
		return;

	const QString jid = Jid(AJid).bare();
	QHash<quint32, OmemoStore::Device> &devices = FStore->data().devices[jid];
	const bool hadDevices = !devices.isEmpty();
	bool changed = false;

	for (quint32 deviceId : ADevices)
	{
		if (deviceId == 0 || devices.contains(deviceId))
			continue;
		OmemoStore::Device device;
		// Devices of our own account are under our control and are trusted
		// automatically; devices of contacts use TOFU/BTBV.
		device.trustLevel = isOwnJid(jid) ? OmemoStore::TrustTrusted
			: (hadDevices ? OmemoStore::TrustUndecided : OmemoStore::TrustTrusted);
		devices.insert(deviceId,device);
		changed = true;
	}

	if (changed)
	{
		FStore->markDirty();
		FStore->save();
	}
}

bool OmemoAccount::processPEPEvent(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (!FStarted || AStreamJid != FStreamJid)
		return false;

	QDomElement event = AStanza.firstElement(QStringLiteral("event"),QString::fromLatin1(NS_PUBSUB_EVENT));
	QDomElement items = event.firstChildElement(QStringLiteral("items"));
	if (items.isNull() || items.attribute(QStringLiteral("node")) != QString::fromLatin1(OMEMO_NODE_DEVICELIST))
		return false;

	const QString from = AStanza.fromJid().isValid() ? AStanza.fromJid().bare() : bareJid();
	parseDeviceList(items,from);
	emit stateChanged(from);
	return true;
}

void OmemoAccount::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	Q_UNUSED(AStreamJid);
	const QString id = AStanza.id();
	if (!FRequests.contains(id))
		return;

	const RequestInfo info = FRequests.take(id);
	const bool result = AStanza.type() != STANZA_TYPE_ERROR;

	switch (info.type)
	{
	case RequestDeviceList:
	{
		FPendingDeviceLists.remove(info.jid);
		if (result)
		{
			QDomElement pubsub = AStanza.firstElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
			QDomElement items = pubsub.firstChildElement(QStringLiteral("items"));
			parseDeviceList(items,info.jid);
		}
		else
		{
			XmppStanzaError error(AStanza);
			if (error.condition() == QStringLiteral("item-not-found")
				|| error.condition() == QStringLiteral("feature-not-implemented"))
			{
				// The contact simply has no OMEMO node; this is not a
				// transport failure and a plaintext chat is expected.
				FDeviceLists[info.jid] = QList<quint32>();
				FDeviceListTimes[info.jid] = QDateTime::currentSecsSinceEpoch();
				FKnownDeviceLists.insert(info.jid);
				FFailedDeviceLists.remove(info.jid);
				if (isOwnJid(info.jid))
					publishOwnDevice();
			}
			else
			{
				// Do not treat a failed PEP request as an empty device list:
				// a network error must not downgrade the chat to plaintext.
				FFailedDeviceLists.insert(info.jid);
			}
		}
		emit stateChanged(info.jid);
		break;
	}
	case RequestBundle:
	{
		const QString cacheKey = bundleCacheKey(info.jid,info.deviceId);
		FPendingBundles.remove(cacheKey);
		if (result)
		{
			QDomElement pubsub = AStanza.firstElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
			QDomElement items = pubsub.firstChildElement(QStringLiteral("items"));
			parseBundle(items,info.jid,info.deviceId);
		}
		else
		{
			FFailedBundles.insert(cacheKey);
		}
		emit stateChanged(info.jid);
		break;
	}
	case RequestNodeConfig:
		if (result)
		{
			QDomElement pubsub = AStanza.firstElement(QStringLiteral("pubsub"),QString::fromLatin1(NS_PUBSUB));
			QDomElement configure = pubsub.firstChildElement(QStringLiteral("configure"));
			if (!configure.isNull())
				parseNodeConfigAndMaybeSubmit(configure,info.node);
		}
		break;
	case RequestNodeConfigSubmit:
		FConfiguringNodes.remove(info.node);
		if (!result)
			LOG_STRM_WARNING(FStreamJid,QString("Cannot make OMEMO PEP node public: %1").arg(info.node));
		break;
	}
}

bool OmemoAccount::parseNodeConfigAndMaybeSubmit(const QDomElement &AConfigure, const QString &ANode)
{
	QDomElement form = AConfigure.firstChildElement(QStringLiteral("x"));
	if (form.isNull())
		return false;

	QDomElement accessField;
	bool isOpen = false;
	for (QDomElement field = form.firstChildElement(QStringLiteral("field"));
		!field.isNull(); field = field.nextSiblingElement(QStringLiteral("field")))
	{
		if (field.attribute(QStringLiteral("var")) == QStringLiteral("pubsub#access_model"))
		{
			accessField = field;
			isOpen = field.firstChildElement(QStringLiteral("value")).text() == QStringLiteral("open");
			break;
		}
	}

	if (isOpen)
		return true;

	QDomElement newForm = form.cloneNode(true).toElement();
	newForm.setAttribute(QStringLiteral("type"),QStringLiteral("submit"));
	QDomElement newField;
	for (QDomElement field = newForm.firstChildElement(QStringLiteral("field"));
		!field.isNull(); field = field.nextSiblingElement(QStringLiteral("field")))
	{
		if (field.attribute(QStringLiteral("var")) == QStringLiteral("pubsub#access_model"))
		{
			newField = field;
			break;
		}
	}
	if (newField.isNull())
	{
		newField = newForm.ownerDocument().createElement(QStringLiteral("field"));
		newField.setAttribute(QStringLiteral("var"),QStringLiteral("pubsub#access_model"));
		newForm.appendChild(newField);
	}

	QDomElement value = newField.firstChildElement(QStringLiteral("value"));
	if (value.isNull())
	{
		value = newField.ownerDocument().createElement(QStringLiteral("value"));
		newField.appendChild(value);
	}
	while (!value.firstChild().isNull())
		value.removeChild(value.firstChild());
	value.appendChild(newField.ownerDocument().createTextNode(QStringLiteral("open")));

	return submitNodeConfig(ANode,newForm);
}

QString OmemoAccount::bundleNode(quint32 ADeviceId) const
{
	return QString::fromLatin1(OMEMO_NODE_BUNDLES) + QLatin1Char(':') + QString::number(ADeviceId);
}

QString OmemoAccount::bundleCacheKey(const QString &AJid, quint32 ADeviceId) const
{
	return Jid(AJid).bare() + QLatin1Char('/') + QString::number(ADeviceId);
}

bool OmemoAccount::isOwnJid(const QString &AJid) const
{
	return Jid(AJid).bare() == bareJid();
}
