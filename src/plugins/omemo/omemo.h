#ifndef OMEMO_H
#define OMEMO_H

#include <QHash>
#include <QObject>
#include <QUuid>

#include <interfaces/iaccountmanager.h>
#include <interfaces/iendtoendencryption.h>
#include <interfaces/imessagestylemanager.h>
#include <interfaces/imessageprocessor.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/ipluginmanager.h>
#include <interfaces/ipepmanager.h>
#include <interfaces/istanzaprocessor.h>
#include <interfaces/ixmppstreammanager.h>

#include "omemoaccount.h"

#define OMEMO_UUID "8CD7A325-8F03-4C78-B632-F5AAE71979B7"

class QTextBrowser;

class Omemo :
	public QObject,
	public IPlugin,
	public IStanzaHandler,
	public IMessageEditor,
	public IEndToEndEncryption,
	public IMessageStyleHandler
{
	Q_OBJECT
	Q_INTERFACES(IPlugin IStanzaHandler IMessageEditor IEndToEndEncryption IMessageStyleHandler)
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.Omemo")
public:
	Omemo();
	virtual ~Omemo();
	// IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(OMEMO_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings() { return true; }
	virtual bool startPlugin();
	// IStanzaHandler
	virtual bool stanzaReadWrite(int AHandleId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept);
	// IMessageEditor
	virtual bool messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection);
	// IEndToEndEncryption
	virtual bool decryptMessage(const Jid &AStreamJid, Message &AMessage, int ADirection);
	// IMessageStyleHandler
	virtual bool messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml,
		QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions);
	virtual QString messageStyleContentSlot(int AOrder, QWidget *AWidget,
		const QString &ASlotName, bool ASlotInTemplate,
		const IMessageStyleContentOptions &AOptions);

private slots:
	void onAccountInserted(IAccount *AAccount);
	void onAccountRemoved(IAccount *AAccount);
	void onAccountDestroyed(const QUuid &AAccountId);
	void onStreamOpened(IXmppStream *AXmppStream);
	void onStreamClosed(IXmppStream *AXmppStream);
	void onAccountError(const QString &AMessage);
	void onAccountStateChanged(OmemoAccount *AAccount, const QString &AJid);
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onChatWindowDestroyed(IMessageChatWindow *AWindow);
	void onWindowAddressChanged();
	void onWindowContextMenuRequested(Menu *AMenu);

private:
	enum MessageStatus
	{
		StatusUnknown = 0,
		StatusEncrypted,
		StatusNotEncrypted,
		StatusUnencryptedExpected,
		StatusPending,
		StatusFailed
	};

	struct StanzaHandles
	{
		int in = -1;
		int out = -1;
	};
	struct PendingMessage
	{
		Jid streamJid;
		QString contactJid;
		Stanza stanza;
	};

	void startAccount(OmemoAccount *AAccount);
	OmemoAccount *accountForStream(const Jid &AStreamJid);
	void updateLock(IMessageChatWindow *AWindow);
	void updateLocks(OmemoAccount *AAccount);
	void showDevicesDialog(IMessageChatWindow *AWindow);
	void setMessageStatus(const QString &AMessageId, int AStatus);
	int messageStatus(const QString &AMessageId) const;
	QString omemoMarkHtml(int AStatus) const;
	void alignOmemoMark(QTextBrowser *ABrowser, const QString &AMessageId) const;
	void markOutgoingStatus(const Jid &AStreamJid, Message &AMessage);
	static void markStanzaStatus(Stanza &AStanza, int AStatus);
	static QString statusName(int AStatus);
	static int statusFromName(const QString &AName);
	bool processInbound(const Jid &AStreamJid, Stanza &AStanza, QString *APlainOut = nullptr);
	bool processOutbound(const Jid &AStreamJid, Stanza &AStanza);
	bool encryptStanza(OmemoAccount *AAccount, const QString &AContactJid, Stanza &AStanza);
	void queuePending(const Jid &AStreamJid, const QString &AContactJid, const Stanza &AStanza);
	void flushPending(OmemoAccount *AAccount, const QString &AJid);
	bool sendRaw(const Jid &AStreamJid, Stanza &AStanza);
	static void replaceBody(Stanza &AStanza, const QString &ABody);

private:
	IPluginManager *FPluginManager;
	IAccountManager *FAccountManager;
	IStanzaProcessor *FStanzaProcessor;
	IPEPManager *FPepManager;
	IXmppStreamManager *FXmppStreamManager;
	IMessageProcessor *FMessageProcessor;
	IMessageWidgets *FMessageWidgets;
	QHash<QUuid, OmemoAccount *> FAccounts;
	QHash<QString, OmemoAccount *> FStreamAccounts;
	QHash<QString, StanzaHandles> FStanzaHandles;
	QHash<QString, QList<PendingMessage> > FPendingMessages;
	QHash<IMessageChatWindow *, Action *> FLockActions;
	QHash<IMessageChatWindow *, Action *> FLockToggles;
	QHash<QString, int> FMessageStatus;
	bool FAccountRemoving;
};

#endif // OMEMO_H
