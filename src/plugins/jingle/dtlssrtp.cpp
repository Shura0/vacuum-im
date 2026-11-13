#include "dtlssrtp.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QStringList>
#include <QTimer>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <srtp2/srtp.h>

//Accept any certificate chain in the verify callback; peer identity is checked
//strictly against the fingerprint from the session description afterwards.
static int dtlsVerifyCallback(int APreverify, X509_STORE_CTX *AStoreCtx)
{
	Q_UNUSED(AStoreCtx);
	return APreverify ? 1 : 1;
}

static bool srtpInitialized()
{
	static bool done = (srtp_init() == srtp_err_status_ok);
	return done;
}

DtlsSrtp::DtlsSrtp(QObject *AParent)
	: QObject(AParent)
{
	FsslCtx = nullptr;
	Fssl = nullptr;
	FreadBio = nullptr;
	FwriteBio = nullptr;
	FsrtpOut = nullptr;
	FsrtpIn = nullptr;
	FState = StateNew;
	FAsClient = false;

	FdtlsTimer = new QTimer(this);
	FdtlsTimer->setSingleShot(true);
	connect(FdtlsTimer, &QTimer::timeout, this, [this]() {
		if (!Fssl)
			return;
		DTLSv1_handle_timeout(static_cast<SSL *>(Fssl));
		pumpHandshake();
	});
}

DtlsSrtp::~DtlsSrtp()
{
	stop();
}

bool DtlsSrtp::createIdentity()
{
	if (!srtpInitialized())
		return false;

	EVP_PKEY *pkey = nullptr;
	EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
	if (!pctx)
		return false;
	if (EVP_PKEY_keygen_init(pctx) <= 0 ||
		EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0 ||
		EVP_PKEY_keygen(pctx, &pkey) <= 0)
	{
		EVP_PKEY_CTX_free(pctx);
		return false;
	}
	EVP_PKEY_CTX_free(pctx);

	X509 *x509 = X509_new();
	if (!x509)
	{
		EVP_PKEY_free(pkey);
		return false;
	}

	X509_set_version(x509, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(x509), (long)QRandomGenerator::global()->bounded(1000000000));
	X509_gmtime_adj(X509_getm_notBefore(x509), 0);
	X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60 * 24 * 30);
	X509_set_pubkey(x509, pkey);

	X509_NAME *name = X509_get_subject_name(x509);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"vacuum-im", -1, -1, 0);
	X509_set_issuer_name(x509, name);
	X509_sign(x509, pkey, EVP_sha256());

	unsigned char *der = nullptr;
	const int derLength = i2d_X509(x509, &der);
	if (derLength > 0 && der)
	{
		FLocalFingerprint = certificateFingerprint(der, derLength);
		OPENSSL_free(der);
	}
	else
	{
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return false;
	}

	FsslCtx = SSL_CTX_new(DTLS_method());
	if (!FsslCtx)
	{
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return false;
	}

	SSL_CTX *ctx = static_cast<SSL_CTX *>(FsslCtx);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, dtlsVerifyCallback);
	SSL_CTX_set_tlsext_use_srtp(ctx, "SRTP_AES128_CM_SHA1_80");
	if (SSL_CTX_use_certificate(ctx, x509) != 1 || SSL_CTX_use_PrivateKey(ctx, pkey) != 1 ||
		SSL_CTX_check_private_key(ctx) != 1)
	{
		X509_free(x509);
		EVP_PKEY_free(pkey);
		stop();
		return false;
	}
	X509_free(x509);
	EVP_PKEY_free(pkey);

	Fssl = SSL_new(ctx);
	FreadBio = BIO_new(BIO_s_mem());
	FwriteBio = BIO_new(BIO_s_mem());
	SSL_set_bio(static_cast<SSL *>(Fssl), static_cast<BIO *>(FreadBio), static_cast<BIO *>(FwriteBio));
	return true;
}

QString DtlsSrtp::localFingerprint() const
{
	return FLocalFingerprint;
}

void DtlsSrtp::setPeerFingerprint(const QString &AFingerprint)
{
	FPeerFingerprint = normalizeFingerprint(AFingerprint);
}

bool DtlsSrtp::start(bool AAsClient)
{
	if (!Fssl || FState == StateConnecting || FState == StateConnected)
		return false;

	FAsClient = AAsClient;
	if (FAsClient)
		SSL_set_connect_state(static_cast<SSL *>(Fssl));
	else
		SSL_set_accept_state(static_cast<SSL *>(Fssl));

	setState(StateConnecting);
	pumpHandshake();
	return FState == StateConnecting || FState == StateConnected;
}

void DtlsSrtp::stop()
{
	if (FsrtpIn)
		srtp_dealloc(static_cast<srtp_t>(FsrtpIn));
	if (FsrtpOut)
		srtp_dealloc(static_cast<srtp_t>(FsrtpOut));
	FsrtpIn = nullptr;
	FsrtpOut = nullptr;

	//SSL_shutdown() on a socket whose handshake never finished raises
	//SSL_R_UNINITIALIZED into the per-thread OpenSSL error queue. That queue is shared
	//with Qt's QSslSocket in the same process, and it drains the whole queue when it
	//collects TLS errors - so a leftover from here aborts the XMPP stream with a bogus
	//"SSL routines::uninitialized". Only close_notify a completed session, and always
	//leave the queue as clean as we found it.
	if (Fssl)
	{
		if (FState == StateConnected)
			SSL_shutdown(static_cast<SSL *>(Fssl));
		//SSL_free also frees the attached BIOs
		SSL_free(static_cast<SSL *>(Fssl));
	}
	Fssl = nullptr;
	FreadBio = nullptr;
	FwriteBio = nullptr;

	if (FsslCtx)
		SSL_CTX_free(static_cast<SSL_CTX *>(FsslCtx));
	FsslCtx = nullptr;

	//Drain whatever the handshake attempt left behind; see the note above about the
	//error queue being shared process-wide with Qt's TLS sockets.
	ERR_clear_error();

	FdtlsTimer->stop();
	FState = StateNew;
}

DtlsSrtp::State DtlsSrtp::state() const
{
	return FState;
}

void DtlsSrtp::writeDatagram(const QByteArray &ADatagram)
{
	if (!Fssl || ADatagram.isEmpty())
		return;

	BIO_write(static_cast<BIO *>(FreadBio), ADatagram.constData(), ADatagram.size());
	if (FState == StateConnecting)
		pumpHandshake();
}

QByteArray DtlsSrtp::protectRtp(const QByteArray &APacket) const
{
	if (!FsrtpOut)
		return QByteArray();
	QByteArray out = APacket;
	out.resize(APacket.size() + SRTP_MAX_TRAILER_LEN);
	//srtp_protect() reads the input packet length from len_ptr and writes the protected length back
	int length = APacket.size();
	if (srtp_protect(static_cast<srtp_t>(FsrtpOut), out.data(), &length) != srtp_err_status_ok)
		return QByteArray();
	out.resize(length);
	return out;
}

QByteArray DtlsSrtp::protectRtcp(const QByteArray &APacket) const
{
	if (!FsrtpOut)
		return QByteArray();
	QByteArray out = APacket;
	out.resize(APacket.size() + SRTP_MAX_TRAILER_LEN);
	int length = APacket.size();
	if (srtp_protect_rtcp(static_cast<srtp_t>(FsrtpOut), out.data(), &length) != srtp_err_status_ok)
		return QByteArray();
	out.resize(length);
	return out;
}

QByteArray DtlsSrtp::unprotectRtp(const QByteArray &APacket) const
{
	if (!FsrtpIn)
		return QByteArray();
	QByteArray out = APacket;
	int length = out.size();
	if (srtp_unprotect(static_cast<srtp_t>(FsrtpIn), out.data(), &length) != srtp_err_status_ok)
		return QByteArray();
	out.resize(length);
	return out;
}

QByteArray DtlsSrtp::unprotectRtcp(const QByteArray &APacket) const
{
	if (!FsrtpIn)
		return QByteArray();
	QByteArray out = APacket;
	int length = out.size();
	if (srtp_unprotect_rtcp(static_cast<srtp_t>(FsrtpIn), out.data(), &length) != srtp_err_status_ok)
		return QByteArray();
	out.resize(length);
	return out;
}

void DtlsSrtp::setState(State AState)
{
	if (FState == AState)
		return;
	FState = AState;
	emit stateChanged((int)AState);
}

void DtlsSrtp::pumpHandshake()
{
	if (!Fssl || FState != StateConnecting)
		return;

	SSL *ssl = static_cast<SSL *>(Fssl);
	const int result = SSL_do_handshake(ssl);
	flushOutgoing();

	//flushOutgoing() can be re-entrant: datagramOut feeds the peer directly in tests and
	//in the media engine, so the handshake may have already finished deeper in the stack.
	if (FState != StateConnecting)
		return;

	if (result == 1)
	{
		onHandshakeDone();
		return;
	}

	const int error = SSL_get_error(ssl, result);
	if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
	{
		ERR_clear_error(); //do not leak our handshake failure into the shared queue
		setState(StateFailed);
		return;
	}

	struct timeval timeout;
	if (DTLSv1_get_timeout(ssl, &timeout))
		FdtlsTimer->start((qint64)timeout.tv_sec * 1000 + timeout.tv_usec / 1000);
	else
		FdtlsTimer->stop();
}

void DtlsSrtp::flushOutgoing()
{
	if (!FwriteBio)
		return;

	BIO *bio = static_cast<BIO *>(FwriteBio);
	for (int pending = BIO_pending(bio); pending > 0; pending = BIO_pending(bio))
	{
		QByteArray datagram(pending, '\0');
		const int read = BIO_read(bio, datagram.data(), pending);
		if (read <= 0)
			break;
		datagram.resize(read);
		emit datagramOut(datagram);
	}
}

void DtlsSrtp::onHandshakeDone()
{
	if (FState != StateConnecting)
		return;

	SSL *ssl = static_cast<SSL *>(Fssl);

	//Strict fingerprint pinning (XEP-0359)
	X509 *peer = SSL_get1_peer_certificate(ssl);
	if (!peer)
	{
		setState(StateFailed);
		return;
	}
	unsigned char *der = nullptr;
	const int derLength = i2d_X509(peer, &der);
	X509_free(peer);
	if (derLength <= 0 || !der)
	{
		setState(StateFailed);
		return;
	}
	const QString peerFingerprint = certificateFingerprint(der, derLength);
	OPENSSL_free(der);

	if (FPeerFingerprint.isEmpty() || normalizeFingerprint(peerFingerprint) != FPeerFingerprint)
	{
		setState(StateFailed);
		return;
	}

	const SRTP_PROTECTION_PROFILE *profile = SSL_get_selected_srtp_profile(ssl);
	if (!profile || strcmp(profile->name, "SRTP_AES128_CM_SHA1_80") != 0)
	{
		setState(StateFailed);
		return;
	}

	if (!deriveSrtpKeys())
	{
		setState(StateFailed);
		return;
	}

	FdtlsTimer->stop();
	setState(StateConnected);
}

bool DtlsSrtp::deriveSrtpKeys()
{
	//RFC 5764 section 4.2: client_write_key | server_write_key | client_salt | server_salt
	unsigned char material[60];
	//The label "EXTRACTOR-dtls_srtp" is 19 characters long.
	const int result = SSL_export_keying_material(static_cast<SSL *>(Fssl), material, sizeof(material),
	                                              "EXTRACTOR-dtls_srtp", 19, nullptr, 0, 0);
	if (result != 1)
		return false;

	unsigned char outKey[30]; //16 key + 14 salt
	unsigned char inKey[30];
	if (FAsClient)
	{
		memcpy(outKey, material, 16);
		memcpy(outKey + 16, material + 32, 14);
		memcpy(inKey, material + 16, 16);
		memcpy(inKey + 16, material + 46, 14);
	}
	else
	{
		memcpy(outKey, material + 16, 16);
		memcpy(outKey + 16, material + 46, 14);
		memcpy(inKey, material, 16);
		memcpy(inKey + 16, material + 32, 14);
	}

	auto create = [](void **AContext, unsigned char *AKey, bool AOutbound) {
		srtp_policy_t policy;
		memset(&policy, 0, sizeof(policy));
		srtp_crypto_policy_set_rtp_default(&policy.rtp);
		srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
		policy.ssrc.type = AOutbound ? ssrc_any_outbound : ssrc_any_inbound;
		policy.ssrc.value = 0;
		policy.key = AKey;
		policy.keys = nullptr;
		policy.num_master_keys = 0;
		policy.window_size = 64;
		policy.allow_repeat_tx = 1;
		policy.next = nullptr;
		return srtp_create(reinterpret_cast<srtp_t *>(AContext), &policy) == srtp_err_status_ok;
	};

	//srtp_create() copies the master key and salt into the session contexts
	return create(&FsrtpOut, outKey, true) && create(&FsrtpIn, inKey, false);
}

QString DtlsSrtp::certificateFingerprint(const unsigned char *ADer, int ALength)
{
	const QByteArray hash = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(ADer), ALength),
	                                                QCryptographicHash::Sha256);
	QStringList parts;
	for (const char ch : hash)
		parts << QString("%1").arg((quint8)ch, 2, 16, QChar('0')).toUpper();
	return parts.join(':');
}

QString DtlsSrtp::normalizeFingerprint(const QString &AFingerprint)
{
	QString result = AFingerprint;
	result.remove(':');
	return result.toLower();
}
