#ifndef OMEMOCRYPT_H
#define OMEMOCRYPT_H

#include <QByteArray>
#include <QList>
#include <QString>

class OmemoStore;

struct signal_context;
struct signal_protocol_store_context;

// C++ wrapper around the libomemo-c protocol v3 (legacy OMEMO 0.3) API.
//
// The class owns the signal_context and the signal_protocol_store_context.
// All persistent data is kept in OmemoStore; the store callbacks mark it
// dirty and flush() writes it atomically.
class OmemoCrypto
{
public:
	struct PreKeyBundle
	{
		quint32 registrationId = 0;
		quint32 deviceId = 0;
		quint32 preKeyId = 0;
		QByteArray preKey;
		quint32 signedPreKeyId = 0;
		QByteArray signedPreKey;
		QByteArray signedPreKeySignature;
		QByteArray identityKey;
	};

	struct PreKeyInfo
	{
		quint32 id = 0;
		QByteArray publicKey;
	};

	explicit OmemoCrypto(OmemoStore *AStore);
	~OmemoCrypto();

	bool init(QString *AError = nullptr);
	bool ensureIdentity(QString *AError = nullptr);
	bool ensurePreKeys(int ACount, QString *AError = nullptr);
	bool flush();

	bool isReady() const { return FReady; }
	QString lastError() const { return FLastError; }

	quint32 deviceId() const;
	quint32 registrationId() const;
	QByteArray identityPublicKey() const;
	QByteArray identityKeySerialized() const;
	int preKeyCount() const;
	QList<PreKeyInfo> preKeys() const;
	bool signedPreKeyInfo(quint32 &AId, QByteArray &APublicKey,
		QByteArray &ASignature) const;

	OmemoStore *store() const { return FStore; }
	signal_context *context() const { return FContext; }

	bool createBundle(PreKeyBundle &ABundle, QString *AError = nullptr);
	bool processBundle(const QString &AJid, quint32 ADeviceId, const PreKeyBundle &ABundle,
		QString *AError = nullptr);

	bool hasSession(const QString &AJid, quint32 ADeviceId) const;

	bool encrypt(const QString &AJid, quint32 ADeviceId, const QByteArray &APlain,
		QByteArray &ACipher, bool &APreKey, QString *AError = nullptr);
	bool decryptPreKey(const QString &AJid, quint32 ADeviceId, const QByteArray &ACipher,
		QByteArray &APlain, QString *AError = nullptr);
	bool decryptSignal(const QString &AJid, quint32 ADeviceId, const QByteArray &ACipher,
		QByteArray &APlain, QString *AError = nullptr);

private:
	bool setError(const QString &AError, QString *AErrorOut);
	bool ensureStoreLoaded(QString *AErrorOut);

private:
	OmemoStore *FStore;
	signal_context *FContext;
	signal_protocol_store_context *FStoreContext;
	QString FLastError;
	bool FReady;
};

#endif // OMEMOCRYPT_H
