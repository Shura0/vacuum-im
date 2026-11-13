#include "mediaengine.h"
#include "dtlssrtp.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QLibrary>
#include <QMediaDevices>
#include <QRandomGenerator>
#include <QTimer>
#include <QtEndian>

#include <utils/logger.h>

//The subset of the libopus API we use; loaded at runtime to keep the plugin
//buildable on systems without the codec package.
typedef void *(*OpusEncoderCreateFunc)(int FS, int Channels, int Application, int *Error);
typedef int (*OpusEncoderCtlFunc)(void *Encoder, int Request, ...);
typedef int (*OpusEncodeFunc)(void *Encoder, const short *Pcm, int FrameSize, unsigned char *Data, int MaxBytes);
typedef void *(*OpusDecoderCreateFunc)(int FS, int Channels, int *Error);
typedef int (*OpusDecodeFunc)(void *Decoder, const unsigned char *Data, int Len, short *Out, int MaxFrameSize, int DecodeFec);

#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002

static const int OpusSampleRate = 48000;
static const int OpusChannels = 2;
static const int OpusFrameMs = 20;
static const int OpusFrameSamples = OpusSampleRate * OpusFrameMs / 1000; //960
static const int OpusFrameBytes = OpusFrameSamples * OpusChannels * int(sizeof(short)); //3840
static const int OpusMaxPacket = 1500;

//RTP static header size with no CSRCs/extensions is 12 bytes
static const int RtpHeaderSize = 12;

MediaEngine::MediaEngine(QObject *AParent)
	: QObject(AParent)
{
	FIceAgent = nullptr;
	FDtlsSrtp = nullptr;
	FOpusLib = nullptr;
	FOpusEncoder = nullptr;
	FOpusDecoder = nullptr;
	FSource = nullptr;
	FSink = nullptr;
	FCaptureIo = nullptr;
	FPlaybackIo = nullptr;
	FMediaTimer = nullptr;

	FLocalSsrc = QRandomGenerator::global()->generate();
	FRemotePayloadType = -1;
	FRemoteClockrate = OpusSampleRate;
	FSeq = QRandomGenerator::global()->bounded(0xFFFF);
	FTimestamp = QRandomGenerator::global()->generate();
	FFirstPacket = true;
	FAudioActive = false;
	FAsDtlsClient = true;
}

MediaEngine::~MediaEngine()
{
	stop();
}

bool MediaEngine::initialize()
{
	FIceAgent = new IceAgent(this);
	FDtlsSrtp = new DtlsSrtp(this);

	connect(FIceAgent, &IceAgent::connected, this, [this]() {
		LOG_INFO(QString("MediaEngine: ICE connected, DTLS starting as %1").arg(FAsDtlsClient ? "client" : "server"));
		emit iceConnected();
		FDtlsSrtp->start(FAsDtlsClient);
	});
	connect(FIceAgent, &IceAgent::disconnected, this, &MediaEngine::onIceDisconnected);
	connect(FIceAgent, &IceAgent::mediaReceived, this, &MediaEngine::onMediaDatagram);
	connect(FDtlsSrtp, &DtlsSrtp::stateChanged, this, &MediaEngine::onDtlsStateChanged);
	connect(FDtlsSrtp, &DtlsSrtp::datagramOut, this, [this](const QByteArray &ADatagram) {
		FIceAgent->writeMedia(ADatagram, 1);
	});

	if (!FDtlsSrtp->createIdentity())
	{
		LOG_WARNING("MediaEngine: DTLS identity failed (check OpenSSL)");
		return false;
	}

	FOpusLib = new QLibrary(this);
	if (!loadOpus())
	{
		LOG_WARNING("MediaEngine: libopus could not be loaded at runtime - install the Opus library to enable calls");
		return false;
	}

	FFormat = QAudioFormat();
	FFormat.setSampleRate(OpusSampleRate);
	FFormat.setChannelCount(OpusChannels);
	FFormat.setSampleFormat(QAudioFormat::Int16);

	FMediaTimer = new QTimer(this);
	FMediaTimer->setInterval(OpusFrameMs);
	connect(FMediaTimer, &QTimer::timeout, this, &MediaEngine::onMediaTick);

	return true;
}

bool MediaEngine::loadOpus()
{
	FOpusLib->setFileName("libopus.so.0");
	if (!FOpusLib->load())
	{
		//Fall back to plain soname resolution on other platforms
		FOpusLib->setFileName("opus");
		if (!FOpusLib->load())
			return false;
	}

	OpusEncoderCreateFunc encoderCreate = reinterpret_cast<OpusEncoderCreateFunc>(FOpusLib->resolve("opus_encoder_create"));
	OpusDecoderCreateFunc decoderCreate = reinterpret_cast<OpusDecoderCreateFunc>(FOpusLib->resolve("opus_decoder_create"));
	if (!encoderCreate || !decoderCreate)
		return false;

	int error = 0;
	FOpusEncoder = encoderCreate(OpusSampleRate, OpusChannels, OPUS_APPLICATION_VOIP, &error);
	if (!FOpusEncoder || error != 0)
		return false;
	FOpusDecoder = decoderCreate(OpusSampleRate, OpusChannels, &error);
	if (!FOpusDecoder || error != 0)
		return false;

	OpusEncoderCtlFunc encoderCtl = reinterpret_cast<OpusEncoderCtlFunc>(FOpusLib->resolve("opus_encoder_ctl"));
	if (encoderCtl)
		encoderCtl(FOpusEncoder, OPUS_SET_BITRATE_REQUEST, 48000);
	return true;
}

IceAgent *MediaEngine::iceAgent() const
{
	return FIceAgent;
}

DtlsSrtp *MediaEngine::dtlsSrtp() const
{
	return FDtlsSrtp;
}

quint32 MediaEngine::localSsrc() const
{
	return FLocalSsrc;
}

void MediaEngine::setRemoteCodec(int APayloadType, int AClockrate)
{
	FRemotePayloadType = APayloadType;
	FRemoteClockrate = AClockrate;
}

void MediaEngine::setDtlsClientRole(bool AAsClient)
{
	FAsDtlsClient = AAsClient;
}

void MediaEngine::startIce(bool AControlling)
{
	FIceAgent->setControlling(AControlling);
	FIceAgent->start();
}

void MediaEngine::stop()
{
	stopAudio();
	if (FDtlsSrtp)
		FDtlsSrtp->stop();
	if (FIceAgent)
		FIceAgent->stop();
}

bool MediaEngine::isAudioActive() const
{
	return FAudioActive;
}

void MediaEngine::onIceDisconnected(const QString &AReason)
{
	FAudioActive = false;
	emit failed(AReason);
}

void MediaEngine::onDtlsStateChanged(int AState)
{
	switch (AState)
	{
	case DtlsSrtp::StateConnected:
		emit dtlsConnected();
		if (!startAudio())
			emit failed(tr("Failed to initialize the audio devices"));
		break;
	case DtlsSrtp::StateFailed:
		emit failed(tr("The encrypted media handshake failed"));
		break;
	default:
		break;
	}
}

void MediaEngine::onMediaDatagram(const QByteArray &AData, int AComponent)
{
	if (AData.size() < 2)
		return;

	const quint8 firstByte = quint8(AData.at(0));

	//Datagram mux demultiplex (RFC 7983): DTLS records carry first byte 20..63
	if (firstByte >= 20 && firstByte <= 63)
	{
		FDtlsSrtp->writeDatagram(AData);
		return;
	}

	if (firstByte < 128 || firstByte > 223)
		return;

	//RTCP compound packets live in 192..223 (PT range of the RTCP messages)
	const bool isRtcp = (AData.size() >= 4) && (quint8(AData.at(1)) >= 192 && quint8(AData.at(1)) <= 192 + 8);
	const QByteArray plain = isRtcp ? FDtlsSrtp->unprotectRtcp(AData) : FDtlsSrtp->unprotectRtp(AData);
	if (plain.isEmpty() || plain.size() < RtpHeaderSize)
		return;

	if (isRtcp)
		return; //RTCP stats arrive with the call polish phase

	//Parse the fixed RTP header
	const quint8 secondByte = quint8(plain.at(1));
	const int payloadType = secondByte & 0x7F;
	const int csrcCount = quint8(plain.at(0)) & 0x0F;
	int offset = RtpHeaderSize + csrcCount * 4;
	if (offset > plain.size())
		return;
	const bool hasExtension = quint8(plain.at(0)) & 0x10;
	if (hasExtension)
	{
		if (offset + 4 > plain.size())
			return;
		const quint16 extensionLength = qFromBigEndian<quint16>(plain.constData() + offset + 2);
		offset += 4 + extensionLength * 4;
		if (offset > plain.size())
			return;
	}
	const QByteArray payload = plain.mid(offset);
	if (payload.isEmpty())
		return;

	if (payloadType != FRemotePayloadType && FRemotePayloadType >= 0)
		return;

	static short pcm[OpusFrameSamples * OpusChannels];
	OpusDecodeFunc decoderDecode = reinterpret_cast<OpusDecodeFunc>(FOpusLib->resolve("opus_decode"));
	if (!decoderDecode)
		return;

	const int samples = decoderDecode(FOpusDecoder, reinterpret_cast<const unsigned char *>(payload.constData()),
	                                  payload.size(), pcm, OpusFrameSamples, 0);
	if (samples > 0 && FPlaybackIo)
		FPlaybackIo->write(reinterpret_cast<const char *>(pcm), samples * OpusChannels * qint64(sizeof(short)));
}

void MediaEngine::onMediaTick()
{
	if (!FAudioActive || !FCaptureIo || !FPlaybackIo)
		return;
	if (FRemotePayloadType < 0)
		return;

	if (FCaptureIo->bytesAvailable() < OpusFrameBytes)
		return;

	const QByteArray pcm = FCaptureIo->read(OpusFrameBytes);
	static unsigned char rtpPayload[OpusMaxPacket];
	OpusEncodeFunc encoderEncode = reinterpret_cast<OpusEncodeFunc>(FOpusLib->resolve("opus_encode"));
	if (!encoderEncode || pcm.size() < OpusFrameBytes)
		return;

	const int payloadSize = encoderEncode(FOpusEncoder, reinterpret_cast<const short *>(pcm.constData()),
	                                      OpusFrameSamples, rtpPayload, sizeof(rtpPayload));
	if (payloadSize <= 0)
		return;

	const QByteArray packet = buildRtpPacket(QByteArray(reinterpret_cast<const char *>(rtpPayload), payloadSize), FFirstPacket);
	FFirstPacket = false;

	const QByteArray protectedPacket = FDtlsSrtp->protectRtp(packet);
	if (!protectedPacket.isEmpty())
		FIceAgent->writeMedia(protectedPacket, 1);
}

QByteArray MediaEngine::buildRtpPacket(const QByteArray &AOpusPayload, bool AMarker)
{
	quint8 header[RtpHeaderSize];
	header[0] = 0x80; //V=2, no padding, no extension, no CSRC
	header[1] = quint8(FRemotePayloadType & 0x7F) | (AMarker ? 0x80 : 0x00);

	quint16 seqBE = qToBigEndian<quint16>(quint16(FSeq++));
	memcpy(header + 2, &seqBE, 2);
	quint32 tsBE = qToBigEndian<quint32>(FTimestamp);
	memcpy(header + 4, &tsBE, 4);
	quint32 ssrcBE = qToBigEndian<quint32>(FLocalSsrc);
	memcpy(header + 8, &ssrcBE, 4);

	FTimestamp += OpusFrameSamples;

	QByteArray packet(reinterpret_cast<const char *>(header), RtpHeaderSize);
	packet.append(AOpusPayload);
	return packet;
}

bool MediaEngine::startAudio()
{
	if (FAudioActive)
		return true;

	const QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
	const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
	if (inputDevice.isNull() || outputDevice.isNull())
		return false;

	FSource = new QAudioSource(inputDevice, FFormat, this);
	FSink = new QAudioSink(outputDevice, FFormat, this);
	FSource->setBufferSize(OpusFrameBytes * 4);
	FSink->setBufferSize(OpusFrameBytes * 4);

	FCaptureIo = FSource->start();
	FPlaybackIo = FSink->start();
	if (!FCaptureIo || !FPlaybackIo)
	{
		stopAudio();
		return false;
	}

	FAudioActive = true;
	FMediaTimer->start();
	return true;
}

void MediaEngine::stopAudio()
{
	FMediaTimer->stop();
	FAudioActive = false;

	if (FSource)
		FSource->stop();
	if (FSink)
		FSink->stop();
	FCaptureIo = nullptr;
	FPlaybackIo = nullptr;
	delete FSource;
	delete FSink;
	FSource = nullptr;
	FSink = nullptr;
}
