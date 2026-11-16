#ifndef MEDIAENGINE_H
#define MEDIAENGINE_H

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QUuid>

#include "audioconvert.h"
#include "iceagent.h"
#include "jitterbuffer.h"
#include "rtpdefs.h"

class DtlsSrtp;
class QAudioSink;
class QAudioSource;
class QIODevice;
class QLibrary;
class QTimer;
class RtcpSession;

//The subset of the libopus API we use; loaded at runtime to keep the plugin
//buildable on systems without the codec package.
typedef void *(*OpusEncoderCreateFunc)(int ASampleRate, int AChannels, int AApplication, int *AError);
typedef int (*OpusEncoderCtlFunc)(void *AEncoder, int ARequest, ...);
typedef int (*OpusEncodeFunc)(void *AEncoder, const short *APcm, int AFrameSize, unsigned char *AData, int AMaxBytes);
typedef void *(*OpusDecoderCreateFunc)(int ASampleRate, int AChannels, int *AError);
typedef int (*OpusDecodeFunc)(void *ADecoder, const unsigned char *AData, int ALen, short *AOut, int AMaxFrameSize, int ADecodeFec);

/*
 * The audio media path of a call: an IceAgent moves the datagrams, a DtlsSrtp object
 * secures them (muxed with DTLS on the RTP socket), RTP carries one Opus stream in
 * each direction and Qt Multimedia feeds the local capture/playback.
 */
class MediaEngine :
	public QObject
{
	Q_OBJECT;
public:
	explicit MediaEngine(QObject *AParent = nullptr);
	virtual ~MediaEngine();

	bool initialize(); //Creates the ICE agent, the DTLS identity and the Opus library

	IceAgent *iceAgent() const;
	DtlsSrtp *dtlsSrtp() const;

	quint32 localSsrc() const;
	//The negotiated payload type/clockrate for the local->remote Opus stream
	void setRemoteCodec(int APayloadType, int AClockrate);

	void startIce(bool AControlling);
	void stop();

	bool isAudioActive() const;
	//DTLS role: we are the client when our advertised/adopted setup attribute is "active"
	void setDtlsClientRole(bool AAsClient);
	//Peer DTLS parameters from the session-accept: the handshake cannot start without
	//them, and as the offerer they may arrive after ICE has already connected.
	void applyDtlsParameters(bool AAsClient, const QString &APeerFingerprint);

signals:
	void iceConnected();
	void dtlsConnected();
	void failed(const QString &AReason);

private slots:
	void onIceDisconnected(const QString &AReason);
	void onDtlsStateChanged(int AState);
	void onMediaDatagram(const QByteArray &AData, int AComponent);
	void onMediaTick();

private:
	bool loadOpus();
	bool startAudio();
	void stopAudio();
	void emitFailed(const QString &AReason);
	QByteArray buildRtpPacket(const QByteArray &AOpusPayload, bool AMarker);
	qint64 clockMs() const;
	void drainPlayback();
	void captureAndSend();
	int decodeOpus(const char *AData, int ALength);
	void scheduleRtcpReport(int AMs);
	void sendRtcpReport();
	void sendRtcp(const QByteArray &APacket);

private:
	IceAgent *FIceAgent;
	DtlsSrtp *FDtlsSrtp;

	QLibrary *FOpusLib;
	void *FOpusEncoder;   //OpusEncoder *
	void *FOpusDecoder;   //OpusDecoder *
	OpusEncodeFunc FOpusEncode;
	OpusDecodeFunc FOpusDecode;

	quint32 FLocalSsrc;
	int FRemotePayloadType;
	int FRemoteClockrate;
	quint32 FSeq;
	quint32 FTimestamp;
	bool FFirstPacket;

	JitterBuffer FJitter;
	RtpSeqTracker FSeqTracker;
	RtcpSession *FRtcpSession;
	QTimer *FRtcpTimer;
	bool FPeerReportLogged;
	QElapsedTimer FClock;

	qint64 FPlayoutDue;    //Samples the audio clock still owes the sink
	qint64 FLastPlayoutMs;
	QByteArray FCapturePending;
	QByteArray FPcmBuffer;

	QAudioSource *FSource;
	QAudioSink *FSink;
	QIODevice *FCaptureIo;
	QIODevice *FPlaybackIo;
	QTimer *FMediaTimer;
	QAudioFormat FSourceFormat;   //The format the capture device actually accepted
	QAudioFormat FSinkFormat;     //The format the playback device actually accepted
	AudioConverter FCaptureConverter;  //Device format -> Opus (48 kHz, stereo)
	AudioConverter FPlaybackConverter; //Opus (48 kHz, stereo) -> device format

	bool FAsDtlsClient;
	bool FRemoteParamsReady;
	bool FIceConnected;
	bool FAudioActive;
	bool FFailPending;
};

#endif // MEDIAENGINE_H
