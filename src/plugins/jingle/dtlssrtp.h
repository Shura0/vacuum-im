#ifndef DTLSSRTP_H
#define DTLSSRTP_H

#include <QObject>
#include <QString>

class QTimer;

/*
 * DTLS-SRTP per XEP-0359/0360: a self-signed EC P-256 certificate whose SHA-256
 * fingerprint is exchanged in the session description, a DTLS handshake pumped over
 * memory BIOs (datagrams in, datagrams out), key material export for SRTP, and the
 * protect/unprotect calls for the RTP and RTCP streams.
 */
class DtlsSrtp :
	public QObject
{
	Q_OBJECT;
public:
	enum State {
		StateNew = 0,
		StateConnecting = 1,
		StateConnected = 2,
		StateFailed = 3
	};

	explicit DtlsSrtp(QObject *AParent = nullptr);
	virtual ~DtlsSrtp();

	//Generates the identity (key pair + certificate) and the local fingerprint
	bool createIdentity();
	QString localFingerprint() const;

	//The fingerprint from the peer's <fingerprint> element, "AA:BB:.." hex
	void setPeerFingerprint(const QString &AFingerprint);

	bool start(bool AAsClient);
	void stop();
	State state() const;

	//Feed a datagram received from the media socket (handshake traffic)
	void writeDatagram(const QByteArray &ADatagram);

	QByteArray protectRtp(const QByteArray &APacket) const;
	QByteArray protectRtcp(const QByteArray &APacket) const;
	QByteArray unprotectRtp(const QByteArray &APacket) const;
	QByteArray unprotectRtcp(const QByteArray &APacket) const;

signals:
	void stateChanged(int AState);
	void datagramOut(const QByteArray &ADatagram);

private:
	void setState(State AState);
	void pumpHandshake();
	void flushOutgoing();
	void onHandshakeDone();
	bool deriveSrtpKeys();
	static QString certificateFingerprint(const unsigned char *ADer, int ALength);
	static QString normalizeFingerprint(const QString &AFingerprint);

private:
	void *FsslCtx;   //SSL_CTX *
	void *Fssl;      //SSL *
	void *FreadBio;  //BIO *
	void *FwriteBio; //BIO *
	QTimer *FdtlsTimer;

	QString FLocalFingerprint;
	QString FPeerFingerprint;
	State FState;
	bool FAsClient;

	mutable void *FsrtpOut; //srtp_t, protect side
	mutable void *FsrtpIn;  //srtp_t, unprotect side
};

#endif // DTLSSRTP_H
