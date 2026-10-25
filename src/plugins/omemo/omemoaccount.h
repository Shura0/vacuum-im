#ifndef OMEMOACCOUNT_H
#define OMEMOACCOUNT_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QUuid>

#include <interfaces/iaccountmanager.h>
#include <interfaces/ipepmanager.h>
#include <interfaces/istanzaprocessor.h>
#include <utils/jid.h>
#include <utils/stanza.h>

#include "omemocrypto.h"

class Omemo;
class OmemoStore;

// Per-account OMEMO state: encrypted store, Signal sessions, PEP device list
// and bundle cache.  One object exists for every Vacuum account, but it only
// starts on an opened XMPP stream.
class OmemoAccount :
	public QObject,
	public IStanzaRequestOwner,
	public IPEPHandler
{
	Q_OBJECT
	Q_INTERFACES(IStanzaRequestOwner IPEPHandler)
public:
	OmemoAccount(Omemo *APlugin, IAccount *AAccount,
		IStanzaProcessor *AStanzaProcessor, IPEPManager *APepManager);
	virtual ~OmemoAccount();

	// IStanzaRequestOwner
	virtual QObject *instance() { return this; }
	virtual void stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza);

	// IPEPHandler
	virtual bool processPEPEvent(const Jid &AStreamJid, const Stanza &AStanza);

	bool start(bool AInitializeTransport = true);
	void stop();
	void streamOpened(const Jid &AStreamJid);
	void streamClosed();
	bool isStarted() const { return FStarted; }

	QUuid accountId() const;
	Jid streamJid() const { return FStreamJid; }
	QString bareJid() const { return FStreamJid.bare(); }
	OmemoCrypto *crypto() const { return FCrypto; }
	OmemoStore *store() const { return FStore; }

	QList<quint32> devices(const QString &AJid) const;
	QList<quint32> encryptableDevices(const QString &AJid) const;
	bool isDeviceListKnown(const QString &AJid) const;
	bool hasDevices(const QString &AJid) const;
	bool hasEncryptableDevices(const QString &AJid) const;
	bool sessionsReady(const QString &AJid) const;
	bool isChatEnabled(const QString &AJid) const;
	void setChatEnabled(const QString &AJid, bool AEnabled);
	int deviceTrust(const QString &AJid, quint32 ADeviceId) const;
	void setDeviceTrust(const QString &AJid, quint32 ADeviceId, int ATrustLevel);
	QString deviceFingerprint(const QString &AJid, quint32 ADeviceId) const;
	QString ownFingerprint() const;

	// Returns true when the device list and every needed bundle are cached and
	// sessions have been established.  Missing data is requested in the
	// background; stateChanged() is emitted when it arrives.
	bool ensureSessions(const QString &AJid);
	bool ensureDeviceList(const QString &AJid);
	void refreshDeviceList(const QString &AJid);
	void fetchDeviceBundles(const QString &AJid);

	bool ensureSelfSession(QString *AError = nullptr);
	void cacheMessage(const QString &AMessageId, const QByteArray &APlain);
	bool cachedMessage(const QString &AMessageId, QByteArray &APlain) const;
	bool encryptMessage(const QString &ATargetJid, const QByteArray &APlain,
		QDomElement &AEncrypted, QString *AError = nullptr);
	bool decryptMessage(const QString &ASenderJid, const QDomElement &AEncrypted,
		QByteArray &APlain, QString *AError = nullptr);

	void publishOwnDevice();
	void refreshOwnBundle();

signals:
	void stateChanged(const QString &AJid);
	void error(const QString &AMessage);

private:
	enum RequestType
	{
		RequestDeviceList,
		RequestBundle,
		RequestNodeConfig,
		RequestNodeConfigSubmit
	};

	struct RequestInfo
	{
		RequestType type = RequestDeviceList;
		QString jid;
		quint32 deviceId = 0;
		QString node;
	};

	void parseDeviceList(const QDomElement &AItems, const QString &AJid);
	void parseBundle(const QDomElement &AItems, const QString &AJid, quint32 ADeviceId);
	bool parseNodeConfigAndMaybeSubmit(const QDomElement &AConfigure, const QString &ANode);

	void registerDevices(const QString &AJid, const QList<quint32> &ADevices);
	void publishDeviceList(const QList<quint32> &ADevices);
	bool publishPepItem(const QString &ANode, const QDomElement &AItem);
	bool publishBundle();
	void subscribeDeviceList(const QString &AJid);
	void configureNodeOpen(const QString &ANode);

	bool requestDeviceList(const QString &AJid);
	bool requestBundle(const QString &AJid, quint32 ADeviceId);
	bool requestNodeConfig(const QString &ANode);
	bool submitNodeConfig(const QString &ANode, const QDomElement &AForm);
	bool sendPubsubItemsRequest(const Jid &ATo, const QString &ANode,
		RequestType AType, const QString &AJid, quint32 ADeviceId);
	bool sendPubsubRequest(const Jid &ATo, const QDomElement &APayload,
		RequestType AType, const QString &AJid, quint32 ADeviceId, const QString &ANode);

	QString bundleNode(quint32 ADeviceId) const;
	QString bundleCacheKey(const QString &AJid, quint32 ADeviceId) const;
	bool isOwnJid(const QString &AJid) const;

private:
	Omemo *FPlugin;
	IAccount *FAccount;
	IStanzaProcessor *FStanzaProcessor;
	IPEPManager *FPepManager;
	Jid FStreamJid;
	QString FStorePath;
	OmemoStore *FStore;
	OmemoCrypto *FCrypto;
	int FPepHandler;
	bool FStarted;

	QHash<QString, QList<quint32> > FDeviceLists;
	QHash<QString, qint64> FDeviceListTimes;
	QSet<QString> FKnownDeviceLists;
	QHash<QString, QHash<quint32, OmemoCrypto::PreKeyBundle> > FBundles;
	QHash<QString, RequestInfo> FRequests;
	QSet<QString> FPendingDeviceLists;
	QSet<QString> FFailedDeviceLists;
	QSet<QString> FPendingBundles;
	QSet<QString> FFailedBundles;
	QSet<QString> FSubscriptions;
	QSet<QString> FConfiguringNodes;
	bool FPublishingDeviceList;
	bool FPublishingBundle;
};

#endif // OMEMOACCOUNT_H
