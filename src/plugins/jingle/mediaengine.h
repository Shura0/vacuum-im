#ifndef MEDIAENGINE_H
#define MEDIAENGINE_H

#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QUuid>

#include "iceagent.h"

class DtlsSrtp;
class QAudioSink;
class QAudioSource;
class QIODevice;
class QLibrary;
class QTimer;

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
	QByteArray buildRtpPacket(const QByteArray &AOpusPayload, bool AMarker);

private:
	IceAgent *FIceAgent;
	DtlsSrtp *FDtlsSrtp;

	QLibrary *FOpusLib;
	void *FOpusEncoder;   //OpusEncoder *
	void *FOpusDecoder;   //OpusDecoder *

	quint32 FLocalSsrc;
	int FRemotePayloadType;
	int FRemoteClockrate;
	quint32 FSeq;
	quint32 FTimestamp;
	bool FFirstPacket;

	QAudioSource *FSource;
	QAudioSink *FSink;
	QIODevice *FCaptureIo;
	QIODevice *FPlaybackIo;
	QTimer *FMediaTimer;
	QAudioFormat FFormat;

	bool FAsDtlsClient;
	bool FAudioActive;
};

#endif // MEDIAENGINE_H
