#include "omemocrypto.h"

#include "omemostore.h"

#include <QDebug>
#include <QRandomGenerator>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <ctime>
#include <cstring>

#include <signal_protocol.h>
#include <session_builder.h>
#include <session_cipher.h>
#include <session_pre_key.h>
#include <session_record.h>
#include <ratchet.h>
#include <curve.h>
#include <protocol.h>

namespace
{
	const uint32_t OmemoProtocolVersion = 3;
	const uint32_t OmemoRegistrationIdMax = 16380;
	const int OmemoPreKeyCount = 100;

	struct HmacContext
	{
		EVP_PKEY *key;
		EVP_MD_CTX *ctx;
	};

	struct DigestContext
	{
		EVP_MD_CTX *ctx;
	};

	QByteArray bufferToArray(const signal_buffer *ABuffer)
	{
		if (ABuffer == nullptr)
			return QByteArray();
		return QByteArray((const char *)signal_buffer_data(const_cast<signal_buffer *>(ABuffer)),
			(int)signal_buffer_len(ABuffer));
	}

	signal_buffer *arrayToBuffer(const QByteArray &AArray)
	{
		if (AArray.isEmpty())
			return nullptr;
		return signal_buffer_create((const uint8_t *)AArray.constData(),AArray.size());
	}

	QString jidFromAddress(const signal_protocol_address *AAddress)
	{
		return QString::fromUtf8(AAddress->name,(int)AAddress->name_len);
	}

	OmemoStore::Device *findDevice(OmemoStore *AStore, const QString &AJid, quint32 ADeviceId, bool ACreate)
	{
		QHash<quint32, OmemoStore::Device> &devices = AStore->data().devices[AJid];
		QHash<quint32, OmemoStore::Device>::iterator it = devices.find(ADeviceId);
		if (it == devices.end() && ACreate)
			it = devices.insert(ADeviceId,OmemoStore::Device());
		return it == devices.end() ? nullptr : &it.value();
	}

	void logCallback(int ALevel, const char *AMessage, size_t ALen, void *AUserData)
	{
		Q_UNUSED(ALevel)
		Q_UNUSED(AMessage)
		Q_UNUSED(ALen)
		Q_UNUSED(AUserData)
	}

	int randomCallback(uint8_t *AData, size_t ALen, void *AUserData)
	{
		Q_UNUSED(AUserData)
		return RAND_bytes(AData,(int)ALen) == 1 ? 0 : SG_ERR_UNKNOWN;
	}

	int hmacInitCallback(void **AHmacContext, const uint8_t *AKey, size_t AKeyLen, void *AUserData)
	{
		Q_UNUSED(AUserData)
		HmacContext *context = new HmacContext;
		context->key = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC,nullptr,AKey,(int)AKeyLen);
		context->ctx = EVP_MD_CTX_new();
		if (context->key == nullptr || context->ctx == nullptr
			|| EVP_DigestSignInit(context->ctx,nullptr,EVP_sha256(),nullptr,context->key) != 1)
		{
			EVP_MD_CTX_free(context->ctx);
			EVP_PKEY_free(context->key);
			delete context;
			return SG_ERR_UNKNOWN;
		}
		*AHmacContext = context;
		return 0;
	}

	int hmacUpdateCallback(void *AHmacContext, const uint8_t *AData, size_t ADataLen, void *AUserData)
	{
		Q_UNUSED(AUserData)
		HmacContext *context = static_cast<HmacContext *>(AHmacContext);
		return EVP_DigestSignUpdate(context->ctx,AData,ADataLen) == 1 ? 0 : SG_ERR_UNKNOWN;
	}

	int hmacFinalCallback(void *AHmacContext, signal_buffer **AOutput, void *AUserData)
	{
		Q_UNUSED(AUserData)
		HmacContext *context = static_cast<HmacContext *>(AHmacContext);
		size_t length = 0;
		if (EVP_DigestSignFinal(context->ctx,nullptr,&length) != 1)
			return SG_ERR_UNKNOWN;

		signal_buffer *output = signal_buffer_alloc(length);
		if (output == nullptr)
			return SG_ERR_NOMEM;

		if (EVP_DigestSignFinal(context->ctx,signal_buffer_data(output),&length) != 1)
		{
			signal_buffer_free(output);
			return SG_ERR_UNKNOWN;
		}
		*AOutput = output;
		return 0;
	}

	void hmacCleanupCallback(void *AHmacContext, void *AUserData)
	{
		Q_UNUSED(AUserData)
		HmacContext *context = static_cast<HmacContext *>(AHmacContext);
		EVP_MD_CTX_free(context->ctx);
		EVP_PKEY_free(context->key);
		delete context;
	}

	int sha512InitCallback(void **ADigestContext, void *AUserData)
	{
		Q_UNUSED(AUserData)
		DigestContext *context = new DigestContext;
		context->ctx = EVP_MD_CTX_new();
		if (context->ctx == nullptr || EVP_DigestInit_ex(context->ctx,EVP_sha512(),nullptr) != 1)
		{
			EVP_MD_CTX_free(context->ctx);
			delete context;
			return SG_ERR_UNKNOWN;
		}
		*ADigestContext = context;
		return 0;
	}

	int sha512UpdateCallback(void *ADigestContext, const uint8_t *AData, size_t ADataLen, void *AUserData)
	{
		Q_UNUSED(AUserData)
		DigestContext *context = static_cast<DigestContext *>(ADigestContext);
		return EVP_DigestUpdate(context->ctx,AData,ADataLen) == 1 ? 0 : SG_ERR_UNKNOWN;
	}

	int sha512FinalCallback(void *ADigestContext, signal_buffer **AOutput, void *AUserData)
	{
		Q_UNUSED(AUserData)
		DigestContext *context = static_cast<DigestContext *>(ADigestContext);
		unsigned char digest[EVP_MAX_MD_SIZE];
		unsigned int length = 0;
		if (EVP_DigestFinal_ex(context->ctx,digest,&length) != 1)
			return SG_ERR_UNKNOWN;

		signal_buffer *output = signal_buffer_create(digest,length);
		if (output == nullptr)
			return SG_ERR_NOMEM;

		// Prepare the context for the next digest.
		if (EVP_DigestInit_ex(context->ctx,EVP_sha512(),nullptr) != 1)
		{
			signal_buffer_free(output);
			return SG_ERR_UNKNOWN;
		}

		*AOutput = output;
		return 0;
	}

	void sha512CleanupCallback(void *ADigestContext, void *AUserData)
	{
		Q_UNUSED(AUserData)
		DigestContext *context = static_cast<DigestContext *>(ADigestContext);
		EVP_MD_CTX_free(context->ctx);
		delete context;
	}

	const EVP_CIPHER *cipherForId(int ACipher)
	{
		return ACipher == SG_CIPHER_AES_CTR_NOPADDING ? EVP_aes_256_ctr() : EVP_aes_256_cbc();
	}

	int encryptCallback(signal_buffer **AOutput, int ACipher, const uint8_t *AKey, size_t AKeyLen,
		const uint8_t *AIv, size_t AIvLen, const uint8_t *APlaintext, size_t APlaintextLen,
		void *AUserData)
	{
		Q_UNUSED(AKeyLen)
		Q_UNUSED(AIvLen)
		Q_UNUSED(AUserData)
		if (ACipher != SG_CIPHER_AES_CTR_NOPADDING && ACipher != SG_CIPHER_AES_CBC_PKCS5)
			return SG_ERR_UNKNOWN;

		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		if (ctx == nullptr)
			return SG_ERR_NOMEM;

		QByteArray output((int)APlaintextLen + EVP_MAX_BLOCK_LENGTH,0);
		int written = 0;
		int finalWritten = 0;
		bool ok = EVP_EncryptInit_ex(ctx,cipherForId(ACipher),nullptr,AKey,AIv) == 1
			&& EVP_CIPHER_CTX_set_padding(ctx,ACipher == SG_CIPHER_AES_CBC_PKCS5 ? 1 : 0) == 1
			&& EVP_EncryptUpdate(ctx,(unsigned char *)output.data(),&written,APlaintext,(int)APlaintextLen) == 1
			&& EVP_EncryptFinal_ex(ctx,(unsigned char *)output.data() + written,&finalWritten) == 1;
		EVP_CIPHER_CTX_free(ctx);

		if (!ok)
			return SG_ERR_UNKNOWN;

		signal_buffer *buffer = signal_buffer_create((const uint8_t *)output.constData(),written + finalWritten);
		if (buffer == nullptr)
			return SG_ERR_NOMEM;
		*AOutput = buffer;
		return 0;
	}

	int decryptCallback(signal_buffer **AOutput, int ACipher, const uint8_t *AKey, size_t AKeyLen,
		const uint8_t *AIv, size_t AIvLen, const uint8_t *ACiphertext, size_t ACiphertextLen,
		void *AUserData)
	{
		Q_UNUSED(AKeyLen)
		Q_UNUSED(AIvLen)
		Q_UNUSED(AUserData)
		if (ACipher != SG_CIPHER_AES_CTR_NOPADDING && ACipher != SG_CIPHER_AES_CBC_PKCS5)
			return SG_ERR_UNKNOWN;

		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		if (ctx == nullptr)
			return SG_ERR_NOMEM;

		QByteArray output((int)ACiphertextLen + EVP_MAX_BLOCK_LENGTH,0);
		int written = 0;
		int finalWritten = 0;
		bool ok = EVP_DecryptInit_ex(ctx,cipherForId(ACipher),nullptr,AKey,AIv) == 1
			&& EVP_CIPHER_CTX_set_padding(ctx,ACipher == SG_CIPHER_AES_CBC_PKCS5 ? 1 : 0) == 1
			&& EVP_DecryptUpdate(ctx,(unsigned char *)output.data(),&written,ACiphertext,(int)ACiphertextLen) == 1
			&& EVP_DecryptFinal_ex(ctx,(unsigned char *)output.data() + written,&finalWritten) == 1;
		EVP_CIPHER_CTX_free(ctx);

		if (!ok)
			return SG_ERR_UNKNOWN;

		signal_buffer *buffer = signal_buffer_create((const uint8_t *)output.constData(),written + finalWritten);
		if (buffer == nullptr)
			return SG_ERR_NOMEM;
		*AOutput = buffer;
		return 0;
	}

	int getIdentityKeyPairCallback(signal_buffer **APublicData, signal_buffer **APrivateData, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		const QByteArray &serialized = self->store()->data().identityKeyPair;
		if (serialized.isEmpty())
			return SG_ERR_INVALID_KEY;

		ratchet_identity_key_pair *pair = nullptr;
		if (ratchet_identity_key_pair_deserialize(&pair,(const uint8_t *)serialized.constData(),
			serialized.size(),self->context()) < 0)
			return SG_ERR_INVALID_KEY;

		signal_buffer *publicData = nullptr;
		signal_buffer *privateData = nullptr;
		int result = ec_public_key_serialize(&publicData,ratchet_identity_key_pair_get_public(pair));
		if (result >= 0)
			result = ec_private_key_serialize(&privateData,ratchet_identity_key_pair_get_private(pair));
		SIGNAL_UNREF(pair);

		if (result < 0)
		{
			signal_buffer_free(publicData);
			signal_buffer_free(privateData);
			return SG_ERR_UNKNOWN;
		}

		*APublicData = publicData;
		*APrivateData = privateData;
		return 0;
	}

	int getLocalRegistrationIdCallback(void *AUserData, uint32_t *ARegistrationId)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		*ARegistrationId = self->store()->data().registrationId;
		return *ARegistrationId != 0 ? 0 : SG_ERR_INVALID_KEY;
	}

	int saveIdentityCallback(const signal_protocol_address *AAddress, uint8_t *AKeyData,
		size_t AKeyLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,true);
		if (device == nullptr)
			return SG_ERR_NOMEM;
		device->keyId = AKeyData != nullptr ? QByteArray((const char *)AKeyData,(int)AKeyLen) : QByteArray();
		self->store()->markDirty();
		return 0;
	}

	int isTrustedIdentityCallback(const signal_protocol_address *AAddress, uint8_t *AKeyData,
		size_t AKeyLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,false);
		if (device == nullptr || device->keyId.isEmpty())
			return 1;
		if (device->keyId.size() != (int)AKeyLen)
			return 0;
		return CRYPTO_memcmp(device->keyId.constData(),AKeyData,AKeyLen) == 0 ? 1 : 0;
	}

	int loadPreKeyCallback(signal_buffer **ARecord, uint32_t APreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		const QByteArray &record = self->store()->data().preKeys.value(APreKeyId);
		if (record.isEmpty())
			return SG_ERR_INVALID_KEY_ID;
		*ARecord = arrayToBuffer(record);
		return *ARecord != nullptr ? 0 : SG_ERR_NOMEM;
	}

	int storePreKeyCallback(uint32_t APreKeyId, uint8_t *ARecord, size_t ARecordLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		self->store()->data().preKeys.insert(APreKeyId,QByteArray((const char *)ARecord,(int)ARecordLen));
		self->store()->markDirty();
		return 0;
	}

	int containsPreKeyCallback(uint32_t APreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		return self->store()->data().preKeys.contains(APreKeyId) ? 1 : 0;
	}

	int removePreKeyCallback(uint32_t APreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		self->store()->data().preKeys.remove(APreKeyId);
		self->store()->markDirty();
		return 0;
	}

	int loadSignedPreKeyCallback(signal_buffer **ARecord, uint32_t ASignedPreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		const OmemoStore::SignedPreKey &key = self->store()->data().signedPreKeys.value(ASignedPreKeyId);
		if (key.data.isEmpty())
			return SG_ERR_INVALID_KEY_ID;
		*ARecord = arrayToBuffer(key.data);
		return *ARecord != nullptr ? 0 : SG_ERR_NOMEM;
	}

	int storeSignedPreKeyCallback(uint32_t ASignedPreKeyId, uint8_t *ARecord, size_t ARecordLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::SignedPreKey &key = self->store()->data().signedPreKeys[ASignedPreKeyId];
		key.data = QByteArray((const char *)ARecord,(int)ARecordLen);
		if (key.created == 0)
			key.created = time(nullptr);
		self->store()->markDirty();
		return 0;
	}

	int containsSignedPreKeyCallback(uint32_t ASignedPreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		return self->store()->data().signedPreKeys.contains(ASignedPreKeyId) ? 1 : 0;
	}

	int removeSignedPreKeyCallback(uint32_t ASignedPreKeyId, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		self->store()->data().signedPreKeys.remove(ASignedPreKeyId);
		self->store()->markDirty();
		return 0;
	}

	int loadSessionCallback(signal_buffer **ARecord, signal_buffer **AUserRecord,
		const signal_protocol_address *AAddress, void *AUserData)
	{
		Q_UNUSED(AUserRecord)
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,false);
		if (device == nullptr || device->session.isEmpty())
			return 0;
		*ARecord = arrayToBuffer(device->session);
		return *ARecord != nullptr ? 1 : SG_ERR_NOMEM;
	}

	int getSubDeviceSessionsCallback(signal_int_list **ASessions, const char *AName,
		size_t ANameLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		QString jid = QString::fromUtf8(AName,(int)ANameLen);
		QHash<quint32, OmemoStore::Device> devices = self->store()->data().devices.value(jid);
		signal_int_list *sessions = signal_int_list_alloc();
		if (sessions == nullptr)
			return SG_ERR_NOMEM;
		int count = 0;
		for (QHash<quint32, OmemoStore::Device>::const_iterator it=devices.constBegin(); it!=devices.constEnd(); ++it)
		{
			if (!it->session.isEmpty())
			{
				signal_int_list_push_back(sessions,(int)it.key());
				++count;
			}
		}
		*ASessions = sessions;
		return count;
	}

	int storeSessionCallback(const signal_protocol_address *AAddress, uint8_t *ARecord,
		size_t ARecordLen, uint8_t *AUserRecord, size_t AUserRecordLen, void *AUserData)
	{
		Q_UNUSED(AUserRecord)
		Q_UNUSED(AUserRecordLen)
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,true);
		if (device == nullptr)
			return SG_ERR_NOMEM;
		device->session = QByteArray((const char *)ARecord,(int)ARecordLen);
		self->store()->markDirty();
		return 0;
	}

	int containsSessionCallback(const signal_protocol_address *AAddress, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,false);
		return device != nullptr && !device->session.isEmpty() ? 1 : 0;
	}

	int deleteSessionCallback(const signal_protocol_address *AAddress, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		OmemoStore::Device *device = findDevice(self->store(),jidFromAddress(AAddress),AAddress->device_id,false);
		if (device == nullptr || device->session.isEmpty())
			return 0;
		device->session.clear();
		self->store()->markDirty();
		return 1;
	}

	int deleteAllSessionsCallback(const char *AName, size_t ANameLen, void *AUserData)
	{
		OmemoCrypto *self = static_cast<OmemoCrypto *>(AUserData);
		QString jid = QString::fromUtf8(AName,(int)ANameLen);
		QHash<quint32, OmemoStore::Device> &devices = self->store()->data().devices[jid];
		int count = 0;
		for (QHash<quint32, OmemoStore::Device>::iterator it=devices.begin(); it!=devices.end(); ++it)
		{
			if (!it->session.isEmpty())
			{
				it->session.clear();
				++count;
			}
		}
		if (count > 0)
			self->store()->markDirty();
		return count;
	}

	int loadSenderKeyCallback(signal_buffer **ARecord, signal_buffer **AUserRecord,
		const signal_protocol_sender_key_name *ASenderKeyName, void *AUserData)
	{
		Q_UNUSED(ARecord)
		Q_UNUSED(AUserRecord)
		Q_UNUSED(ASenderKeyName)
		Q_UNUSED(AUserData)
		return 0;
	}

	int storeSenderKeyCallback(const signal_protocol_sender_key_name *ASenderKeyName,
		uint8_t *ARecord, size_t ARecordLen, uint8_t *AUserRecord, size_t AUserRecordLen,
		void *AUserData)
	{
		Q_UNUSED(ASenderKeyName)
		Q_UNUSED(ARecord)
		Q_UNUSED(ARecordLen)
		Q_UNUSED(AUserRecord)
		Q_UNUSED(AUserRecordLen)
		Q_UNUSED(AUserData)
		return 0;
	}
}

OmemoCrypto::OmemoCrypto(OmemoStore *AStore) : FStore(AStore), FContext(nullptr), FStoreContext(nullptr), FReady(false)
{
}

OmemoCrypto::~OmemoCrypto()
{
	if (FStoreContext != nullptr)
		signal_protocol_store_context_destroy(FStoreContext);
	if (FContext != nullptr)
		signal_context_destroy(FContext);
}

bool OmemoCrypto::setError(const QString &AError, QString *AErrorOut)
{
	FLastError = AError;
	if (AErrorOut != nullptr)
		*AErrorOut = AError;
	return false;
}

bool OmemoCrypto::ensureStoreLoaded(QString *AErrorOut)
{
	if (FStore == nullptr)
		return setError(QStringLiteral("OMEMO store is not set"),AErrorOut);
	if (FStore->isLoaded())
		return true;
	if (!FStore->load())
		return setError(FStore->lastError(),AErrorOut);
	return true;
}

bool OmemoCrypto::init(QString *AErrorOut)
{
	if (FReady)
		return true;
	if (!ensureStoreLoaded(AErrorOut))
		return false;

	if (signal_context_create(&FContext,this) < 0)
		return setError(QStringLiteral("cannot create libomemo context"),AErrorOut);
	signal_context_set_log_function(FContext,logCallback);

	signal_crypto_provider provider;
	memset(&provider,0,sizeof(provider));
	provider.random_func = randomCallback;
	provider.hmac_sha256_init_func = hmacInitCallback;
	provider.hmac_sha256_update_func = hmacUpdateCallback;
	provider.hmac_sha256_final_func = hmacFinalCallback;
	provider.hmac_sha256_cleanup_func = hmacCleanupCallback;
	provider.sha512_digest_init_func = sha512InitCallback;
	provider.sha512_digest_update_func = sha512UpdateCallback;
	provider.sha512_digest_final_func = sha512FinalCallback;
	provider.sha512_digest_cleanup_func = sha512CleanupCallback;
	provider.encrypt_func = encryptCallback;
	provider.decrypt_func = decryptCallback;
	if (signal_context_set_crypto_provider(FContext,&provider) < 0)
		return setError(QStringLiteral("cannot set libomemo crypto provider"),AErrorOut);

	if (signal_protocol_store_context_create(&FStoreContext,FContext) < 0)
		return setError(QStringLiteral("cannot create libomemo store context"),AErrorOut);

	signal_protocol_session_store sessionStore;
	memset(&sessionStore,0,sizeof(sessionStore));
	sessionStore.load_session_func = loadSessionCallback;
	sessionStore.get_sub_device_sessions_func = getSubDeviceSessionsCallback;
	sessionStore.store_session_func = storeSessionCallback;
	sessionStore.contains_session_func = containsSessionCallback;
	sessionStore.delete_session_func = deleteSessionCallback;
	sessionStore.delete_all_sessions_func = deleteAllSessionsCallback;
	sessionStore.user_data = this;
	signal_protocol_store_context_set_session_store(FStoreContext,&sessionStore);

	signal_protocol_pre_key_store preKeyStore;
	memset(&preKeyStore,0,sizeof(preKeyStore));
	preKeyStore.load_pre_key = loadPreKeyCallback;
	preKeyStore.store_pre_key = storePreKeyCallback;
	preKeyStore.contains_pre_key = containsPreKeyCallback;
	preKeyStore.remove_pre_key = removePreKeyCallback;
	preKeyStore.user_data = this;
	signal_protocol_store_context_set_pre_key_store(FStoreContext,&preKeyStore);

	signal_protocol_signed_pre_key_store signedPreKeyStore;
	memset(&signedPreKeyStore,0,sizeof(signedPreKeyStore));
	signedPreKeyStore.load_signed_pre_key = loadSignedPreKeyCallback;
	signedPreKeyStore.store_signed_pre_key = storeSignedPreKeyCallback;
	signedPreKeyStore.contains_signed_pre_key = containsSignedPreKeyCallback;
	signedPreKeyStore.remove_signed_pre_key = removeSignedPreKeyCallback;
	signedPreKeyStore.user_data = this;
	signal_protocol_store_context_set_signed_pre_key_store(FStoreContext,&signedPreKeyStore);

	signal_protocol_identity_key_store identityStore;
	memset(&identityStore,0,sizeof(identityStore));
	identityStore.get_identity_key_pair = getIdentityKeyPairCallback;
	identityStore.get_local_registration_id = getLocalRegistrationIdCallback;
	identityStore.save_identity = saveIdentityCallback;
	identityStore.is_trusted_identity = isTrustedIdentityCallback;
	identityStore.user_data = this;
	signal_protocol_store_context_set_identity_key_store(FStoreContext,&identityStore);

	signal_protocol_sender_key_store senderKeyStore;
	memset(&senderKeyStore,0,sizeof(senderKeyStore));
	senderKeyStore.store_sender_key = storeSenderKeyCallback;
	senderKeyStore.load_sender_key = loadSenderKeyCallback;
	senderKeyStore.user_data = this;
	signal_protocol_store_context_set_sender_key_store(FStoreContext,&senderKeyStore);

	FReady = true;
	if (!ensureIdentity(AErrorOut))
	{
		FReady = false;
		return false;
	}
	return true;
}

bool OmemoCrypto::ensureIdentity(QString *AErrorOut)
{
	if (!ensureStoreLoaded(AErrorOut))
		return false;

	OmemoStore::Data &data = FStore->data();
	bool changed = false;

	if (data.identityKeyPair.isEmpty())
	{
		ec_key_pair *pair = nullptr;
		if (curve_generate_key_pair(FContext,&pair) < 0)
			return setError(QStringLiteral("cannot generate identity key"),AErrorOut);

		ratchet_identity_key_pair *identity = nullptr;
		if (ratchet_identity_key_pair_create(&identity,ec_key_pair_get_public(pair),
			ec_key_pair_get_private(pair)) < 0)
		{
			SIGNAL_UNREF(pair);
			return setError(QStringLiteral("cannot create identity key pair"),AErrorOut);
		}

		signal_buffer *serialized = nullptr;
		signal_buffer *publicKey = nullptr;
		int result = ratchet_identity_key_pair_serialize(&serialized,identity);
		if (result >= 0)
			result = ec_public_key_serialize_omemo(&publicKey,ec_key_pair_get_public(pair));
		SIGNAL_UNREF(identity);
		SIGNAL_UNREF(pair);

		if (result < 0)
		{
			signal_buffer_free(serialized);
			signal_buffer_free(publicKey);
			return setError(QStringLiteral("cannot serialize identity key"),AErrorOut);
		}

		data.identityKeyPair = bufferToArray(serialized);
		data.identityPublic = bufferToArray(publicKey);
		signal_buffer_free(serialized);
		signal_buffer_free(publicKey);
		changed = true;
	}

	if (data.registrationId == 0)
	{
		data.registrationId = QRandomGenerator::global()->bounded(1,(int)OmemoRegistrationIdMax + 1);
		changed = true;
	}

	if (data.deviceId == 0)
	{
		data.deviceId = QRandomGenerator::global()->bounded(1,0x7FFFFFFF);
		changed = true;
	}

	if (changed)
	{
		FStore->markDirty();
		if (!flush())
			return setError(FLastError,AErrorOut);
	}
	return true;
}

bool OmemoCrypto::ensurePreKeys(int ACount, QString *AErrorOut)
{
	if (!FReady || !ensureStoreLoaded(AErrorOut))
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);

	OmemoStore::Data &data = FStore->data();
	bool changed = false;

	if (data.signedPreKeys.isEmpty())
	{
		ratchet_identity_key_pair *identity = nullptr;
		if (ratchet_identity_key_pair_deserialize(&identity,
			(const uint8_t *)data.identityKeyPair.constData(),data.identityKeyPair.size(),FContext) < 0)
			return setError(QStringLiteral("cannot load identity key"),AErrorOut);

		ec_key_pair *pair = nullptr;
		if (curve_generate_key_pair(FContext,&pair) < 0)
		{
			SIGNAL_UNREF(identity);
			return setError(QStringLiteral("cannot generate signed pre-key"),AErrorOut);
		}

		signal_buffer *publicKey = nullptr;
		signal_buffer *signature = nullptr;
		int result = ec_public_key_serialize(&publicKey,ec_key_pair_get_public(pair));
		if (result >= 0)
			result = curve_calculate_signature(FContext,&signature,
				ratchet_identity_key_pair_get_private(identity),
				signal_buffer_data(publicKey),signal_buffer_len(publicKey));

		quint32 id = data.latestSignedPreKeyId != 0 ? data.latestSignedPreKeyId : 1;
		session_signed_pre_key *record = nullptr;
		if (result >= 0)
			result = session_signed_pre_key_create(&record,id,time(nullptr),pair,
				signal_buffer_data(signature),signal_buffer_len(signature),nullptr,0);

		signal_buffer *serialized = nullptr;
		if (result >= 0)
			result = session_signed_pre_key_serialize(&serialized,record);

		SIGNAL_UNREF(record);
		SIGNAL_UNREF(pair);
		SIGNAL_UNREF(identity);
		signal_buffer_free(publicKey);
		signal_buffer_free(signature);

		if (result < 0 || serialized == nullptr)
		{
			signal_buffer_free(serialized);
			return setError(QStringLiteral("cannot create signed pre-key"),AErrorOut);
		}

		OmemoStore::SignedPreKey &key = data.signedPreKeys[id];
		key.created = time(nullptr);
		key.data = bufferToArray(serialized);
		signal_buffer_free(serialized);
		data.latestSignedPreKeyId = id;
		changed = true;
	}

	if (ACount < 1)
		ACount = 1;

	while (data.preKeys.size() < ACount)
	{
		quint32 id = data.latestPreKeyId + 1;
		if (id == 0)
			id = 1;

		ec_key_pair *pair = nullptr;
		if (curve_generate_key_pair(FContext,&pair) < 0)
			return setError(QStringLiteral("cannot generate pre-key"),AErrorOut);

		session_pre_key *record = nullptr;
		int result = session_pre_key_create(&record,id,pair);
		signal_buffer *serialized = nullptr;
		if (result >= 0)
			result = session_pre_key_serialize(&serialized,record);

		SIGNAL_UNREF(record);
		SIGNAL_UNREF(pair);

		if (result < 0 || serialized == nullptr)
		{
			signal_buffer_free(serialized);
			return setError(QStringLiteral("cannot create pre-key"),AErrorOut);
		}

		data.preKeys.insert(id,bufferToArray(serialized));
		signal_buffer_free(serialized);
		data.latestPreKeyId = id;
		changed = true;
	}

	if (changed)
	{
		FStore->markDirty();
		if (!flush())
			return setError(FLastError,AErrorOut);
	}
	return true;
}

bool OmemoCrypto::flush()
{
	if (FStore == nullptr)
		return setError(QStringLiteral("OMEMO store is not set"),nullptr);
	if (!FStore->isDirty())
		return true;
	if (!FStore->save())
		return setError(FStore->lastError(),nullptr);
	return true;
}

quint32 OmemoCrypto::deviceId() const
{
	return FStore != nullptr ? FStore->data().deviceId : 0;
}

quint32 OmemoCrypto::registrationId() const
{
	return FStore != nullptr ? FStore->data().registrationId : 0;
}

QByteArray OmemoCrypto::identityPublicKey() const
{
	return FStore != nullptr ? FStore->data().identityPublic : QByteArray();
}

QByteArray OmemoCrypto::identityKeySerialized() const
{
	const QByteArray publicKey = identityPublicKey();
	if (publicKey.size() != 32)
		return QByteArray();
	return QByteArray(1,(char)0x05) + publicKey;
}

int OmemoCrypto::preKeyCount() const
{
	return FStore != nullptr ? FStore->data().preKeys.size() : 0;
}

QList<OmemoCrypto::PreKeyInfo> OmemoCrypto::preKeys() const
{
	QList<PreKeyInfo> result;
	if (FStore == nullptr)
		return result;

	QList<quint32> ids = FStore->data().preKeys.keys();
	std::sort(ids.begin(),ids.end());
	for (quint32 id : ids)
	{
		const QByteArray &serialized = FStore->data().preKeys.value(id);
		session_pre_key *record = nullptr;
		if (session_pre_key_deserialize(&record,
			(const uint8_t *)serialized.constData(),serialized.size(),FContext) < 0)
			continue;

		signal_buffer *publicKey = nullptr;
		int status = ec_public_key_serialize(&publicKey,
			ec_key_pair_get_public(session_pre_key_get_key_pair(record)));
		SIGNAL_UNREF(record);
		if (status < 0 || publicKey == nullptr)
		{
			signal_buffer_free(publicKey);
			continue;
		}

		PreKeyInfo info;
		info.id = id;
		info.publicKey = bufferToArray(publicKey);
		signal_buffer_free(publicKey);
		result.append(info);
	}
	return result;
}

bool OmemoCrypto::signedPreKeyInfo(quint32 &AId, QByteArray &APublicKey,
	QByteArray &ASignature) const
{
	AId = 0;
	APublicKey.clear();
	ASignature.clear();
	if (FStore == nullptr)
		return false;

	const OmemoStore::Data &data = FStore->data();
	for (QHash<quint32, OmemoStore::SignedPreKey>::const_iterator it=data.signedPreKeys.constBegin();
		it!=data.signedPreKeys.constEnd(); ++it)
	{
		if (it.key() > AId)
			AId = it.key();
	}
	if (AId == 0)
		return false;

	const OmemoStore::SignedPreKey &signedPreKey = data.signedPreKeys.value(AId);
	session_signed_pre_key *record = nullptr;
	if (session_signed_pre_key_deserialize(&record,
		(const uint8_t *)signedPreKey.data.constData(),signedPreKey.data.size(),FContext) < 0)
		return false;

	signal_buffer *publicKey = nullptr;
	int status = ec_public_key_serialize(&publicKey,
		ec_key_pair_get_public(session_signed_pre_key_get_key_pair(record)));
	if (status >= 0 && publicKey != nullptr)
	{
		APublicKey = bufferToArray(publicKey);
		ASignature = QByteArray((const char *)session_signed_pre_key_get_signature(record),
			(int)session_signed_pre_key_get_signature_len(record));
	}
	signal_buffer_free(publicKey);
	SIGNAL_UNREF(record);
	return status >= 0 && !APublicKey.isEmpty() && !ASignature.isEmpty();
}

bool OmemoCrypto::createBundle(PreKeyBundle &ABundle, QString *AErrorOut)
{
	if (!FReady)
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);
	if (!ensurePreKeys(10,AErrorOut))
		return false;

	const OmemoStore::Data &data = FStore->data();
	if (data.signedPreKeys.isEmpty() || data.preKeys.isEmpty() || data.identityPublic.isEmpty())
		return setError(QStringLiteral("OMEMO keys are incomplete"),AErrorOut);

	quint32 signedPreKeyId = 0;
	for (QHash<quint32, OmemoStore::SignedPreKey>::const_iterator it=data.signedPreKeys.constBegin();
		it!=data.signedPreKeys.constEnd(); ++it)
	{
		if (it.key() > signedPreKeyId)
			signedPreKeyId = it.key();
	}

	const OmemoStore::SignedPreKey &signedPreKey = data.signedPreKeys.value(signedPreKeyId);
	session_signed_pre_key *signedRecord = nullptr;
	if (session_signed_pre_key_deserialize(&signedRecord,
		(const uint8_t *)signedPreKey.data.constData(),signedPreKey.data.size(),FContext) < 0)
		return setError(QStringLiteral("cannot decode signed pre-key"),AErrorOut);

	quint32 preKeyId = data.preKeys.constBegin().key();
	session_pre_key *preKeyRecord = nullptr;
	if (session_pre_key_deserialize(&preKeyRecord,
		(const uint8_t *)data.preKeys.value(preKeyId).constData(),
		data.preKeys.value(preKeyId).size(),FContext) < 0)
	{
		SIGNAL_UNREF(signedRecord);
		return setError(QStringLiteral("cannot decode pre-key"),AErrorOut);
	}

	signal_buffer *signedPublic = nullptr;
	signal_buffer *preKeyPublic = nullptr;
	int result = ec_public_key_serialize(&signedPublic,
		ec_key_pair_get_public(session_signed_pre_key_get_key_pair(signedRecord)));
	if (result >= 0)
		result = ec_public_key_serialize(&preKeyPublic,
			ec_key_pair_get_public(session_pre_key_get_key_pair(preKeyRecord)));

	const QByteArray signature((const char *)session_signed_pre_key_get_signature(signedRecord),
		(int)session_signed_pre_key_get_signature_len(signedRecord));

	SIGNAL_UNREF(preKeyRecord);
	SIGNAL_UNREF(signedRecord);

	if (result < 0)
	{
		signal_buffer_free(signedPublic);
		signal_buffer_free(preKeyPublic);
		return setError(QStringLiteral("cannot serialize pre-key bundle"),AErrorOut);
	}

	ABundle.registrationId = data.registrationId;
	ABundle.deviceId = data.deviceId;
	ABundle.preKeyId = preKeyId;
	ABundle.preKey = bufferToArray(preKeyPublic);
	ABundle.signedPreKeyId = signedPreKeyId;
	ABundle.signedPreKey = bufferToArray(signedPublic);
	ABundle.signedPreKeySignature = signature;
	// Legacy OMEMO 0.3 uses libsignal's serialized public key (0x05 + 32
	// bytes) in bundles.  identityPublic is stored without the prefix.
	ABundle.identityKey = QByteArray(1,(char)0x05) + data.identityPublic;

	signal_buffer_free(signedPublic);
	signal_buffer_free(preKeyPublic);
	return true;
}

bool OmemoCrypto::processBundle(const QString &AJid, quint32 ADeviceId,
	const PreKeyBundle &ABundle, QString *AErrorOut)
{
	if (!FReady)
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);

	ec_public_key *preKey = nullptr;
	ec_public_key *signedPreKey = nullptr;
	ec_public_key *identityKey = nullptr;
	// OMEMO 0.3 bundles carry libsignal-serialized (0x05 + 32 bytes) or
	// raw 32-byte Montgomery public keys.  Decode explicitly as Montgomery:
	// the generic curve_decode_point() treats 32 bytes as Ed25519.
	int result = 0;
	if (!ABundle.preKey.isEmpty())
		result = curve_decode_point_mont(&preKey,(const uint8_t *)ABundle.preKey.constData(),
			ABundle.preKey.size(),FContext);
	if (result >= 0)
		result = curve_decode_point_mont(&signedPreKey,(const uint8_t *)ABundle.signedPreKey.constData(),
			ABundle.signedPreKey.size(),FContext);
	if (result >= 0)
		result = curve_decode_point_mont(&identityKey,(const uint8_t *)ABundle.identityKey.constData(),
			ABundle.identityKey.size(),FContext);

	session_pre_key_bundle *bundle = nullptr;
	if (result >= 0)
		result = session_pre_key_bundle_create(&bundle,ABundle.registrationId,(int)ADeviceId,
			ABundle.preKeyId,preKey,ABundle.signedPreKeyId,signedPreKey,
			(const uint8_t *)ABundle.signedPreKeySignature.constData(),
			ABundle.signedPreKeySignature.size(),identityKey);

	if (result < 0 || bundle == nullptr)
	{
		SIGNAL_UNREF(identityKey);
		SIGNAL_UNREF(signedPreKey);
		SIGNAL_UNREF(preKey);
		SIGNAL_UNREF(bundle);
		return setError(QStringLiteral("cannot create pre-key bundle"),AErrorOut);
	}

	SIGNAL_UNREF(identityKey);
	SIGNAL_UNREF(signedPreKey);
	SIGNAL_UNREF(preKey);

	QByteArray name = AJid.toUtf8();
	signal_protocol_address address = { name.constData(),(size_t)name.size(),(int)ADeviceId };
	session_builder *builder = nullptr;
	result = session_builder_create(&builder,FStoreContext,&address,FContext);
	if (result >= 0)
	{
		session_builder_set_version(builder,OmemoProtocolVersion);
		result = session_builder_process_pre_key_bundle(builder,bundle);
	}
	session_builder_free(builder);
	SIGNAL_UNREF(bundle);

	if (result < 0)
		return setError(QStringLiteral("cannot process pre-key bundle: %1").arg(result),AErrorOut);

	if (!flush())
		return setError(FLastError,AErrorOut);
	return true;
}

bool OmemoCrypto::hasSession(const QString &AJid, quint32 ADeviceId) const
{
	if (FStore == nullptr)
		return false;
	QHash<quint32, OmemoStore::Device> devices = FStore->data().devices.value(AJid);
	return devices.value(ADeviceId).session.size() > 0;
}

bool OmemoCrypto::encrypt(const QString &AJid, quint32 ADeviceId, const QByteArray &APlain,
	QByteArray &ACipher, bool &APreKey, QString *AErrorOut)
{
	ACipher.clear();
	APreKey = false;
	if (!FReady)
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);

	QByteArray name = AJid.toUtf8();
	signal_protocol_address address = { name.constData(),(size_t)name.size(),(int)ADeviceId };
	session_cipher *cipher = nullptr;
	int result = session_cipher_create(&cipher,FStoreContext,&address,FContext);
	ciphertext_message *message = nullptr;
	if (result >= 0)
		result = session_cipher_encrypt(cipher,(const uint8_t *)APlain.constData(),APlain.size(),&message);

	if (result >= 0 && message != nullptr)
	{
		APreKey = ciphertext_message_get_type(message) == CIPHERTEXT_PREKEY_TYPE;
		ACipher = bufferToArray(ciphertext_message_get_serialized(message));
	}

	SIGNAL_UNREF(message);
	session_cipher_free(cipher);

	if (result < 0 || ACipher.isEmpty())
		return setError(QStringLiteral("cannot encrypt OMEMO message: %1").arg(result),AErrorOut);

	if (!flush())
		return setError(FLastError,AErrorOut);
	return true;
}

bool OmemoCrypto::decryptPreKey(const QString &AJid, quint32 ADeviceId, const QByteArray &ACipher,
	QByteArray &APlain, QString *AErrorOut)
{
	APlain.clear();
	if (!FReady)
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);

	pre_key_signal_message *message = nullptr;
	int parseResult = pre_key_signal_message_deserialize(&message,
		(const uint8_t *)ACipher.constData(),ACipher.size(),FContext);
	if (parseResult < 0)
	{
		// libomemo-c also accepts the OMEMO 0.4 protobuf key exchange.  Some
		// clients emit it even for the legacy namespace; accepting both keeps
		// the history path compatible without changing our outgoing format.
		parseResult = pre_key_signal_message_deserialize_omemo(&message,
			(const uint8_t *)ACipher.constData(),ACipher.size(),ADeviceId,FContext);
	}
	if (parseResult < 0)
		return setError(QStringLiteral("cannot parse pre-key message"),AErrorOut);

	QByteArray name = AJid.toUtf8();
	signal_protocol_address address = { name.constData(),(size_t)name.size(),(int)ADeviceId };
	session_cipher *cipher = nullptr;
	int result = session_cipher_create(&cipher,FStoreContext,&address,FContext);
	signal_buffer *plaintext = nullptr;
	if (result >= 0)
		result = session_cipher_decrypt_pre_key_signal_message(cipher,message,nullptr,&plaintext);

	if (result >= 0 && plaintext != nullptr)
		APlain = bufferToArray(plaintext);

	signal_buffer_free(plaintext);
	session_cipher_free(cipher);
	SIGNAL_UNREF(message);

	if (result < 0)
		return setError(QStringLiteral("cannot decrypt OMEMO pre-key message: %1").arg(result),AErrorOut);

	if (!flush())
		return setError(FLastError,AErrorOut);
	return true;
}

bool OmemoCrypto::decryptSignal(const QString &AJid, quint32 ADeviceId, const QByteArray &ACipher,
	QByteArray &APlain, QString *AErrorOut)
{
	APlain.clear();
	if (!FReady)
		return setError(QStringLiteral("OMEMO crypto is not initialized"),AErrorOut);

	signal_message *message = nullptr;
	int parseResult = signal_message_deserialize(&message,
		(const uint8_t *)ACipher.constData(),ACipher.size(),FContext);
	if (parseResult < 0)
	{
		parseResult = signal_message_deserialize_omemo(&message,
			(const uint8_t *)ACipher.constData(),ACipher.size(),FContext);
	}
	if (parseResult < 0)
		return setError(QStringLiteral("cannot parse signal message"),AErrorOut);

	QByteArray name = AJid.toUtf8();
	signal_protocol_address address = { name.constData(),(size_t)name.size(),(int)ADeviceId };
	session_cipher *cipher = nullptr;
	int result = session_cipher_create(&cipher,FStoreContext,&address,FContext);
	signal_buffer *plaintext = nullptr;
	if (result >= 0)
		result = session_cipher_decrypt_signal_message(cipher,message,nullptr,&plaintext);

	if (result >= 0 && plaintext != nullptr)
		APlain = bufferToArray(plaintext);

	signal_buffer_free(plaintext);
	session_cipher_free(cipher);
	SIGNAL_UNREF(message);

	if (result < 0)
		return setError(QStringLiteral("cannot decrypt OMEMO message: %1").arg(result),AErrorOut);

	if (!flush())
		return setError(FLastError,AErrorOut);
	return true;
}
