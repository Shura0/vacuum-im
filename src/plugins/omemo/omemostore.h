#ifndef OMEMOSTORE_H
#define OMEMOSTORE_H

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

// Encrypted per-account OMEMO state.
//
// The file format is:
//   "VOMEMO1" | salt[16] | nonce[12] | tag[16] | AES-256-GCM(payload)
//
// The payload is compact JSON.  The AES key is derived from the profile key
// (Options::cryptKey) with HKDF-SHA256 and the random per-file salt.  All
// binary values in the JSON payload are base64-encoded.
class OmemoStore
{
public:
	enum TrustLevel
	{
		TrustUndecided = 0,
		TrustTrusted = 1,
		TrustDistrusted = 2
	};

	struct SignedPreKey
	{
		qint64 created = 0;
		QByteArray data;
	};

	struct Device
	{
		QByteArray keyId;
		QByteArray session;
		int trustLevel = TrustUndecided;
		int unrespondedSent = 0;
		int unrespondedReceived = 0;
		qint64 removedAt = 0;
	};

	struct Data
	{
		quint32 deviceId = 0;
		QString deviceLabel;
		quint32 registrationId = 0;
		QByteArray identityKeyPair;
		QByteArray identityPublic;
		quint32 latestSignedPreKeyId = 0;
		quint32 latestPreKeyId = 0;
		QHash<quint32, SignedPreKey> signedPreKeys;
		QHash<quint32, QByteArray> preKeys;
		// bare JID -> device id -> device
		QHash<QString, QHash<quint32, Device> > devices;
		// Chats where the user explicitly disabled OMEMO.
		QSet<QString> disabledChats;
		// Decrypted plaintext of processed OMEMO messages, keyed by stanza id.
		// This keeps local/MAM history readable even when the ratchet has
		// advanced or the copy was encrypted to an older device.
		QHash<QString, QByteArray> messageCache;
	};

	explicit OmemoStore(const QString &AFilePath, const QByteArray &AKey = QByteArray());

	bool load();
	bool save();
	bool exists() const;
	void reset();

	bool isLoaded() const { return FLoaded; }
	bool isDirty() const { return FDirty; }
	void markDirty() { FDirty = true; }
	const Data &data() const { return FData; }
	Data &data() { return FData; }

	QString filePath() const { return FFilePath; }
	QString lastError() const { return FLastError; }

private:
	QByteArray serialize() const;
	bool deserialize(const QByteArray &APayload);

private:
	QString FFilePath;
	QByteArray FKey;
	Data FData;
	bool FLoaded;
	bool FDirty;
	mutable QString FLastError;
};

#endif // OMEMOSTORE_H
