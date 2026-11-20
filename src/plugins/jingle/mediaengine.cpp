#include "mediaengine.h"
#include "dtlssrtp.h"
#include "rtcpsession.h"
#include "videoreceiver.h"

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
#include <utils/options.h>

#include <definitions/optionvalues.h>

static double pcmRms(const char *AData, int ABytes);

typedef void (*OpusEncoderDestroyFunc)(void *AEncoder);
typedef void (*OpusDecoderDestroyFunc)(void *ADecoder);

#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_BITRATE_REQUEST 4002
#define OPUS_SET_INBAND_FEC_REQUEST 4012
#define OPUS_SET_PACKET_LOSS_PERC_REQUEST 4014

static const int OpusSampleRate = 48000;
static const int OpusChannels = 2;
static const int OpusFrameMs = 20;
static const int OpusFrameSamples = OpusSampleRate * OpusFrameMs / 1000; //960
static const int OpusMaxFrameSamples = OpusSampleRate * 120 / 1000;      //120 ms, the largest Opus frame
static const int OpusFrameBytes = OpusFrameSamples * OpusChannels * int(sizeof(short)); //3840
static const int OpusMaxPacket = 1500;

//The RTCP report interval and the playout budget are described in RFC 3550 6.3
static const int RtcpMinIntervalMs = 5000;

MediaEngine::MediaEngine(QObject *AParent) : QObject(AParent)
{
	FIceAgent = nullptr;
	FDtlsSrtp = nullptr;
	FOpusLib = nullptr;
	FOpusEncoder = nullptr;
	FOpusDecoder = nullptr;
	FOpusEncode = nullptr;
	FOpusDecode = nullptr;
	FSource = nullptr;
	FSink = nullptr;
	FCaptureIo = nullptr;
	FPlaybackIo = nullptr;
	FMediaTimer = nullptr;
	FRtcpSession = nullptr;
	FRtcpTimer = nullptr;
	FEchoCanceller = nullptr;

	FStatCaptureFrames = 0;
	FStatCaptureInSum = 0;
	FStatCaptureOutSum = 0;
	FStatEncodedPackets = 0;
	FStatEncodedBytes = 0;
	FStatPlaybackFrames = 0;
	FStatPlaybackSum = 0;
	FStatPlaybackBytes = 0;
	FStatNextLogMs = 0;

	FLastDecodedSamples = OpusFrameSamples;

	FLocalSsrc = QRandomGenerator::global()->generate();
	FLocalVideoSsrc = QRandomGenerator::global()->generate();
	FRemotePayloadType = -1;
	FRemoteClockrate = OpusSampleRate;
	FRemoteVideoPayloadType = -1;
	FVideoReceiver = nullptr;
	FVideoRtpCount = 0;
	FVideoDecodedCount = 0;
	FSeq = QRandomGenerator::global()->bounded(0xFFFF);
	FTimestamp = QRandomGenerator::global()->generate();
	FFirstPacket = true;
	FAudioActive = false;
	FAsDtlsClient = true;
	FRemoteParamsReady = false;
	FIceConnected = false;
	FFailPending = false;

	FPeerReportLogged = false;
	FPlayoutDue = 0;
	FLastPlayoutMs = -1;
}

MediaEngine::~MediaEngine()
{
	stop();

	//The codec contexts are allocated by libopus, not by the plugin
	OpusEncoderDestroyFunc encoderDestroy = FOpusLib ?
		reinterpret_cast<OpusEncoderDestroyFunc>(FOpusLib->resolve("opus_encoder_destroy")) : nullptr;
	OpusDecoderDestroyFunc decoderDestroy = FOpusLib ?
		reinterpret_cast<OpusDecoderDestroyFunc>(FOpusLib->resolve("opus_decoder_destroy")) : nullptr;
	if (encoderDestroy && FOpusEncoder)
		encoderDestroy(FOpusEncoder);
	if (decoderDestroy && FOpusDecoder)
		decoderDestroy(FOpusDecoder);
}

bool MediaEngine::initialize()
{
	FClock.start();

	FIceAgent = new IceAgent(this);
	FDtlsSrtp = new DtlsSrtp(this);

	connect(FIceAgent, &IceAgent::connected, this, [this]() {
		FIceConnected = true;
		emit iceConnected();
		//As the offerer ICE can finish before the session-accept arrives. The answer
		//carries the peer's fingerprint and the final setup role, and guessing either
		//one makes both sides pick the same DTLS role and the handshake die instantly.
		if (!FRemoteParamsReady)
		{
			LOG_INFO("MediaEngine: ICE connected, waiting for the peer's DTLS parameters before the handshake");
			return;
		}
		LOG_INFO(QString("MediaEngine: ICE connected, DTLS starting as %1").arg(FAsDtlsClient ? "client" : "server"));
		FDtlsSrtp->start(FAsDtlsClient);
	});
	connect(FIceAgent, &IceAgent::disconnected, this, &MediaEngine::onIceDisconnected);
	connect(FIceAgent, &IceAgent::mediaReceived, this, &MediaEngine::onMediaDatagram);
	connect(FDtlsSrtp, &DtlsSrtp::stateChanged, this, &MediaEngine::onDtlsStateChanged);
	connect(FDtlsSrtp, &DtlsSrtp::datagramOut, this, [this](const QByteArray &ADatagram) {
		FIceAgent->writeMedia(ADatagram, 1);
	});

	FRtcpSession = new RtcpSession(this);
	FRtcpSession->setLocalSsrc(FLocalSsrc);
	FRtcpSession->setClockRate(OpusSampleRate);
	connect(FRtcpSession, &RtcpSession::reportSent, this, [this](const QString &ASummary) {
		LOG_DEBUG(QString("MediaEngine: %1, jitter buffer depth=%2 target=%3 plc=%4 late=%5").
			arg(ASummary).arg(FJitter.depth()).arg(FJitter.targetFrames()).
			arg(FJitter.concealedFrames()).arg(FJitter.latePackets()));
		if (FRtcpSession->fractionLost() >= 26) //More than 10% of the peer's stream lost
			LOG_WARNING(QString("MediaEngine: poor reception, %1").arg(ASummary));
	});
	connect(FRtcpSession, &RtcpSession::peerReportReceived, this, [this](const QString &ASummary) {
		if (!FPeerReportLogged)
		{
			FPeerReportLogged = true;
			LOG_INFO(QString("MediaEngine: RTCP reports are flowing, %1").arg(ASummary));
		}
		else
			LOG_DEBUG(QString("MediaEngine: %1").arg(ASummary));
	});
	connect(FRtcpSession, &RtcpSession::peerBye, this, []() {
		LOG_INFO("MediaEngine: peer sent RTCP BYE");
	});

	FRtcpTimer = new QTimer(this);
	FRtcpTimer->setSingleShot(true);
	connect(FRtcpTimer, &QTimer::timeout, this, &MediaEngine::sendRtcpReport);

	if (!FDtlsSrtp->createIdentity())
	{
		LOG_WARNING("MediaEngine: DTLS identity failed (check OpenSSL)");
		return false;
	}

	FVideoReceiver = new VideoReceiver(this);
	FVideoReceiver->initialize();

	FOpusLib = new QLibrary(this);
	if (!loadOpus())
	{
		LOG_WARNING("MediaEngine: libopus could not be loaded at runtime - install the Opus library to enable calls");
		return false;
	}

	FPcmBuffer.resize(OpusMaxFrameSamples * OpusChannels * int(sizeof(short)));

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
	FOpusEncode = reinterpret_cast<OpusEncodeFunc>(FOpusLib->resolve("opus_encode"));
	FOpusDecode = reinterpret_cast<OpusDecodeFunc>(FOpusLib->resolve("opus_decode"));
	if (!encoderCreate || !decoderCreate || !FOpusEncode || !FOpusDecode)
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
	{
		encoderCtl(FOpusEncoder, OPUS_SET_BITRATE_REQUEST, 48000);
		//In-band FEC and an expected loss hint let the peer recover single lost
		//packets instead of concealing them (the cost is a few kbit/s).
		encoderCtl(FOpusEncoder, OPUS_SET_INBAND_FEC_REQUEST, 1);
		encoderCtl(FOpusEncoder, OPUS_SET_PACKET_LOSS_PERC_REQUEST, 10);
	}
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

quint32 MediaEngine::localVideoSsrc() const
{
	return FLocalVideoSsrc;
}

void MediaEngine::setRemoteCodec(int APayloadType, int AClockrate)
{
	FRemotePayloadType = APayloadType;
	FRemoteClockrate = AClockrate;
	FRtcpSession->setClockRate(FRemoteClockrate);
}

void MediaEngine::setRemoteVideoPayloadType(int APayloadType)
{
	FRemoteVideoPayloadType = APayloadType;
}

void MediaEngine::setDtlsClientRole(bool AAsClient)
{
	FAsDtlsClient = AAsClient;
}

void MediaEngine::applyDtlsParameters(bool AAsClient, const QString &APeerFingerprint)
{
	FAsDtlsClient = AAsClient;
	FDtlsSrtp->setPeerFingerprint(APeerFingerprint);
	FRemoteParamsReady = true;

	//ICE may already be connected when the answer finally arrives
	if (FIceConnected && FDtlsSrtp->state() != DtlsSrtp::StateConnected && FDtlsSrtp->state() != DtlsSrtp::StateConnecting)
	{
		LOG_INFO(QString("MediaEngine: DTLS parameters applied, starting handshake as %1").arg(FAsDtlsClient ? "client" : "server"));
		FDtlsSrtp->start(FAsDtlsClient);
	}
}

void MediaEngine::startIce(bool AControlling)
{
	FIceAgent->setControlling(AControlling);
	FIceAgent->start();
}

void MediaEngine::stop()
{
	if (FDtlsSrtp && FRtcpSession && FRtcpSession->isActive() &&
		FDtlsSrtp->state() == DtlsSrtp::StateConnected)
	{
		//Tell the peer the stream is over; RFC 3550 requires a report before BYE
		sendRtcp(FRtcpSession->buildReport(clockMs()) + FRtcpSession->buildBye());
	}

	if (FRtcpSession && FRtcpSession->isActive())
	{
		LOG_INFO(QString("MediaEngine: call media summary rx=%1 expected=%2 lost=%3 jitter=%4 ms rtt=%5 ms plc=%6 late=%7").
			arg(FRtcpSession->receivedPackets()).arg(FRtcpSession->expectedPackets()).
			arg(FRtcpSession->cumulativeLost()).arg(FRtcpSession->jitterMs(), 0, 'f', 1).
			arg(FRtcpSession->peerReport().roundTripMs < 0 ? QString("n/a") :
				QString::number(FRtcpSession->peerReport().roundTripMs, 'f', 1)).
			arg(FJitter.concealedFrames()).arg(FJitter.latePackets()));
	}

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

qint64 MediaEngine::clockMs() const
{
	return FClock.isValid() ? FClock.elapsed() : 0;
}

void MediaEngine::onIceDisconnected(const QString &AReason)
{
	//The media path is gone: close the microphone and the speaker right away
	stopAudio();
	emitFailed(AReason);
}

void MediaEngine::emitFailed(const QString &AReason)
{
	//A failure is often detected in the middle of a datagram: the ICE socket,
	//the DTLS handshake or the audio callback all end up here. Emitting the
	//signal synchronously lets the session tear the call (and this object) down
	//while the media path still holds pointers into it, so the signal is queued
	//and delivered once the current stack has unwound.
	if (FFailPending)
		return;
	FFailPending = true;
	QMetaObject::invokeMethod(this, [this, AReason]() {
		FFailPending = false;
		emit failed(AReason);
	}, Qt::QueuedConnection);
}

void MediaEngine::onDtlsStateChanged(int AState)
{
	switch (AState)
	{
	case DtlsSrtp::StateConnected:
		emit dtlsConnected();
		if (!startAudio())
			emitFailed(tr("Failed to initialize the audio devices"));
		break;
	case DtlsSrtp::StateFailed:
		emitFailed(tr("The encrypted media handshake failed"));
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

	//RTP and RTCP share the socket (RFC 5761, rtcp-mux). The second octet of an
	//RTCP packet is its type (192..223); an RTP payload type never uses that
	//range because SDP has to avoid the 64..95 payload types that would collide.
	const bool isRtcp = (AData.size() >= 4) && (quint8(AData.at(1)) >= 192 && quint8(AData.at(1)) <= 223);

	if (isRtcp)
	{
		const QByteArray plain = FDtlsSrtp->unprotectRtcp(AData);
		if (!plain.isEmpty())
			FRtcpSession->onRtcpReceived(plain, clockMs());
		return;
	}

	const QByteArray plain = FDtlsSrtp->unprotectRtp(AData);
	const RtpHeader header = parseRtpHeader(plain);
	if (!header.valid)
		return;

	if (FRemoteVideoPayloadType >= 0 && header.payloadType == FRemoteVideoPayloadType)
	{
		++FVideoRtpCount;
		if (FVideoReceiver && FVideoReceiver->isReady() && header.payloadLength > 1)
		{
			const unsigned char *payload = reinterpret_cast<const unsigned char *>(plain.constData() + header.payloadOffset);
			const quint8 descriptor = payload[0];
			const bool startOfFrame = (descriptor & 0x10) != 0; //RFC 7741 S bit

			//Skip the payload descriptor: mandatory octet plus X extension fields
			int offset = 1;
			if (descriptor & 0x80)
			{
				const quint8 ext = payload[1];
				offset += 1;
				if (ext & 0x80) //I (PictureID)
					offset += (payload[offset] & 0x80) ? 2 : 1;
				if (ext & 0x40) //L (TL0PICIDX)
					offset += 1;
				if (ext & 0x20 || ext & 0x10) //T or K (TID/Y/KEYIDX)
					offset += 1;
			}
			if (offset > 0 && offset < header.payloadLength)
			{
				//A frame may be split over several RTP packets: S=1 starts a
				//new frame, S=0 continues it, the RTP marker ends it.
				auto decodeFrame = [this]() {
					QImage frame;
					if (FVideoReceiver->decode(FVideoFrame, frame))
					{
						++FVideoDecodedCount;
						if (FVideoDecodedCount == 1)
							LOG_INFO(QString("MediaEngine: first VP8 frame decoded, %1x%2").arg(frame.width()).arg(frame.height()));
						emit videoFrameReceived(frame);
					}
					else if (FVideoDecodedCount == 0 && FVideoRtpCount <= 5)
						LOG_WARNING(QString("MediaEngine: VP8 decode failed, frame=%1 bytes").arg(FVideoFrame.size()));
					FVideoFrame.clear();
				};

				if (startOfFrame && !FVideoFrame.isEmpty())
					decodeFrame(); //new frame arrived without a marker on the previous one
				if (startOfFrame)
					FVideoFrame.clear();
				FVideoFrame.append(reinterpret_cast<const char *>(payload + offset), header.payloadLength - offset);
				if (header.marker)
					decodeFrame();
			}
		}
		if (FVideoRtpCount == 1 || FVideoRtpCount % 100 == 0)
			LOG_INFO(QString("MediaEngine: video RTP packets=%1 decoded=%2 ssrc=0x%3 receiver=%4").
				arg(FVideoRtpCount).arg(FVideoDecodedCount).arg(header.ssrc, 8, 16, QChar('0')).
				arg(FVideoReceiver && FVideoReceiver->isReady()));
		return;
	}

	//Every stream packet feeds the reception statistics, including the ones we
	//do not decode (comfort noise, a payload type that is not the negotiated one)
	const quint64 extendedSeq = FSeqTracker.extend(header.sequence);
	const qint64 now = clockMs();
	FRtcpSession->onRtpReceived(extendedSeq, header.timestamp, now);

	if (FRtcpSession->remoteSsrc() != header.ssrc)
	{
		FRtcpSession->setRemoteSsrc(header.ssrc);
		LOG_INFO(QString("MediaEngine: remote RTP stream ssrc=0x%1").arg(header.ssrc, 8, 16, QChar('0')));
	}

	if (FRemotePayloadType >= 0 && header.payloadType != FRemotePayloadType)
		return;

	const QByteArray payload = plain.mid(header.payloadOffset, header.payloadLength);

	FJitter.push(extendedSeq, payload, now);
}

void MediaEngine::onMediaTick()
{
	if (!FAudioActive || !FCaptureIo || !FPlaybackIo)
		return;
	if (FRemotePayloadType < 0)
		return;

	drainPlayback();
	captureAndSend();

	const qint64 now = clockMs();
	if (FStatNextLogMs == 0)
		FStatNextLogMs = now + 1000;
	else if (now >= FStatNextLogMs)
	{
		FStatNextLogMs = now + 1000;
		const double inRms = FStatCaptureFrames>0 ? FStatCaptureInSum/FStatCaptureFrames : -1;
		const double outRms = FStatCaptureFrames>0 ? FStatCaptureOutSum/FStatCaptureFrames : -1;
		const double playRms = FStatPlaybackFrames>0 ? FStatPlaybackSum/FStatPlaybackFrames : -1;
		LOG_DEBUG(QString("MediaEngine: audio stats, capture frames=%1 in-rms=%2 out-rms=%3, sent pkts=%4 bytes=%5, "
		                 "playback frames=%6 rms=%7 written=%8 B, aec=%9").
			arg(FStatCaptureFrames).arg(inRms, 0, 'f', 0).arg(outRms, 0, 'f', 0).
			arg(FStatEncodedPackets).arg(FStatEncodedBytes).
			arg(FStatPlaybackFrames).arg(playRms, 0, 'f', 0).arg(FStatPlaybackBytes).
			arg(FEchoCanceller != nullptr ? "on" : "off"));
		FStatCaptureFrames = 0;
		FStatCaptureInSum = 0;
		FStatCaptureOutSum = 0;
		FStatEncodedPackets = 0;
		FStatEncodedBytes = 0;
		FStatPlaybackFrames = 0;
		FStatPlaybackSum = 0;
		FStatPlaybackBytes = 0;
	}
}

void MediaEngine::drainPlayback()
{
	const qint64 now = clockMs();
	if (FLastPlayoutMs < 0)
		FLastPlayoutMs = now - OpusFrameMs;

	qint64 elapsed = now - FLastPlayoutMs;
	FLastPlayoutMs = now;
	if (elapsed < 0)
		elapsed = 0;
	//After a stall (a suspended process, a long glitch) do not burst the sink
	if (elapsed > 200)
		elapsed = 200;

	FPlayoutDue += elapsed * OpusSampleRate / 1000;

	//Play as many frames as the audio clock owes, so that a peer using a 40 or
	//60 ms packetization stays in step and the sink does not accumulate delay
	int written = 0;
	while (FPlayoutDue > 0 && written < OpusSampleRate)
	{
		QByteArray payload;
		const JitterBuffer::Result result = FJitter.pop(payload, now);
		if (result == JitterBuffer::Empty)
		{
			//Nothing due: drop the remaining budget instead of bursting later
			FPlayoutDue = 0;
			break;
		}

		const int samples = (result == JitterBuffer::Data) ?
			decodeOpus(payload.constData(), payload.size(), OpusMaxFrameSamples) :
			decodeOpus(nullptr, 0, FLastDecodedSamples);
		if (samples <= 0)
		{
			FPlayoutDue = 0;
			break;
		}

		//The echo canceller needs the decoded far-end audio before it is played
		const int pcmBytes = samples * OpusChannels * int(sizeof(short));
		FStatPlaybackFrames++;
		FStatPlaybackSum += pcmRms(FPcmBuffer.constData(), pcmBytes);
		if (FEchoCanceller)
			FEchoCanceller->processFarEnd(FPcmBuffer.constData(), pcmBytes);

		const QByteArray pcm = FPlaybackConverter.convert(
			QByteArray(FPcmBuffer.constData(), samples * OpusChannels * int(sizeof(short))));
		if (!pcm.isEmpty())
		{
			FPlaybackIo->write(pcm);
			FStatPlaybackBytes += pcm.size();
		}
		FPlayoutDue -= samples;
		written += samples;
	}
}

int MediaEngine::decodeOpus(const char *AData, int ALength, int AMaxSamples)
{
	if (!FOpusDecode || !FOpusDecoder)
		return 0;

	//A null payload with a zero length asks libopus for packet loss concealment
	const int samples = FOpusDecode(FOpusDecoder, reinterpret_cast<const unsigned char *>(AData), ALength,
		reinterpret_cast<short *>(FPcmBuffer.data()), AMaxSamples, 0);
	if (samples <= 0)
		return 0;

	FLastDecodedSamples = samples;
	return samples;
}

void MediaEngine::captureAndSend()
{
	if (!FCaptureIo || !FOpusEncode || !FOpusEncoder)
		return;

	FCapturePending.append(FCaptureConverter.convert(FCaptureIo->readAll()));
	//Keep at most 200 ms of captured audio; the timer normally drains every frame
	if (FCapturePending.size() > OpusFrameBytes * 10)
		FCapturePending.remove(0, FCapturePending.size() - OpusFrameBytes * 10);

	static unsigned char payload[OpusMaxPacket];
	while (FCapturePending.size() >= OpusFrameBytes)
	{
		QByteArray frame = FCapturePending.left(OpusFrameBytes);
		FCapturePending.remove(0, OpusFrameBytes);

		//Remove the echo of the played audio from the microphone signal before
		//it reaches the encoder; the frame is 48 kHz stereo in both cases
		FStatCaptureFrames++;
		FStatCaptureInSum += pcmRms(frame.constData(), frame.size());
		if (FEchoCanceller)
			FEchoCanceller->processNearEnd(frame.data(), frame.size());
		FStatCaptureOutSum += pcmRms(frame.constData(), frame.size());

		const int payloadSize = FOpusEncode(FOpusEncoder, reinterpret_cast<const short *>(frame.constData()),
			OpusFrameSamples, payload, sizeof(payload));
		if (payloadSize <= 0)
			continue;

		FStatEncodedPackets++;
		FStatEncodedBytes += payloadSize;

		const quint32 timestamp = FTimestamp;
		const QByteArray packet = buildRtpPacket(QByteArray(reinterpret_cast<const char *>(payload), payloadSize), FFirstPacket);
		FFirstPacket = false;

		const qint64 now = clockMs();
		FRtcpSession->onRtpSent(timestamp, payloadSize, now);

		const QByteArray protectedPacket = FDtlsSrtp->protectRtp(packet);
		if (!protectedPacket.isEmpty())
			FIceAgent->writeMedia(protectedPacket, 1);
	}
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

void MediaEngine::scheduleRtcpReport(int AMs)
{
	FRtcpTimer->start(AMs);
}

void MediaEngine::sendRtcpReport()
{
	if (!FAudioActive || !FRtcpSession->isActive())
		return;

	sendRtcp(FRtcpSession->buildReport(clockMs()));

	//RFC 3550 6.3: a random multiplier keeps participant reports apart
	scheduleRtcpReport(RtcpMinIntervalMs + int(QRandomGenerator::global()->bounded(500)));
}

void MediaEngine::sendRtcp(const QByteArray &APacket)
{
	const QByteArray protectedPacket = FDtlsSrtp->protectRtcp(APacket);
	if (!protectedPacket.isEmpty())
		FIceAgent->writeMedia(protectedPacket, 1);
}

//Picks a format the device accepts, preferring the natural codec format. The
//sample format is always 16-bit: every conversion below assumes it.
static double pcmRms(const char *AData, int ABytes)
{
	const qint16 *samples = reinterpret_cast<const qint16 *>(AData);
	const int count = ABytes / int(sizeof(qint16));
	if (count <= 0)
		return 0.0;
	double sum = 0;
	for (int i = 0; i < count; ++i)
		sum += double(samples[i]) * samples[i];
	return qSqrt(sum / count);
}

static QAudioFormat negotiateFormat(const QAudioDevice &ADevice)
{
	QList<int> rates;
	rates.append(48000);
	const QAudioFormat preferred = ADevice.preferredFormat();
	if (preferred.sampleRate() > 0 && !rates.contains(preferred.sampleRate()))
		rates.append(preferred.sampleRate());
	foreach (int rate, QList<int>() << 44100 << 32000 << 16000 << 8000)
		if (!rates.contains(rate))
			rates.append(rate);

	foreach (int rate, rates)
	{
		foreach (int channels, QList<int>() << 2 << 1)
		{
			QAudioFormat format;
			format.setSampleRate(rate);
			format.setChannelCount(channels);
			format.setSampleFormat(QAudioFormat::Int16);
			if (ADevice.isFormatSupported(format))
				return format;
		}
	}
	return QAudioFormat();
}

//A saved device is identified by its id; the options page stores the hex of
//QAudioDevice::id(). An empty id means "follow the system default", and a
//missing device (unplugged since the option was saved) falls back to it too.
static QAudioDevice selectAudioDevice(const QList<QAudioDevice> &ADevices, const QString &AId, const QAudioDevice &ADefault)
{
	if (!AId.isEmpty())
	{
		foreach (const QAudioDevice &device, ADevices)
		{
			if (QString::fromLatin1(device.id().toHex()) == AId)
				return device;
		}
	}
	return ADefault;
}

bool MediaEngine::startAudio()
{
	if (FAudioActive)
		return true;

	const QString savedInputId = Options::node(OPV_JINGLE_INPUT_DEVICE).value().toString();
	const QString savedOutputId = Options::node(OPV_JINGLE_OUTPUT_DEVICE).value().toString();
	const QAudioDevice inputDevice = selectAudioDevice(QMediaDevices::audioInputs(), savedInputId, QMediaDevices::defaultAudioInput());
	const QAudioDevice outputDevice = selectAudioDevice(QMediaDevices::audioOutputs(), savedOutputId, QMediaDevices::defaultAudioOutput());
	if (!savedInputId.isEmpty() && QString::fromLatin1(inputDevice.id().toHex()) != savedInputId)
		LOG_WARNING(QString("MediaEngine: saved input device is not available, using '%1'").arg(inputDevice.description()));
	if (!savedOutputId.isEmpty() && QString::fromLatin1(outputDevice.id().toHex()) != savedOutputId)
		LOG_WARNING(QString("MediaEngine: saved output device is not available, using '%1'").arg(outputDevice.description()));
	if (inputDevice.isNull() || outputDevice.isNull())
		return false;

	//The device may refuse 48 kHz stereo: a headset microphone is often mono and
	//a Bluetooth link prefers its own rate. The codec side stays at 48 kHz, the
	//converter at the device boundary adapts rate and channel count.
	FSourceFormat = negotiateFormat(inputDevice);
	FSinkFormat = negotiateFormat(outputDevice);
	if (!FSourceFormat.isValid() || !FSinkFormat.isValid())
	{
		LOG_WARNING(QString("MediaEngine: the selected audio devices do not accept any 16-bit format, "
		                    "input '%1', output '%2'").arg(inputDevice.description(), outputDevice.description()));
		return false;
	}
	if (FSourceFormat.sampleRate() != OpusSampleRate || FSourceFormat.channelCount() != OpusChannels ||
		FSinkFormat.sampleRate() != OpusSampleRate || FSinkFormat.channelCount() != OpusChannels)
	{
		LOG_INFO(QString("MediaEngine: audio format adapted, capture %1 Hz/%2 ch, playback %3 Hz/%4 ch, codec %5 Hz/%6 ch").
			arg(FSourceFormat.sampleRate()).arg(FSourceFormat.channelCount()).
			arg(FSinkFormat.sampleRate()).arg(FSinkFormat.channelCount()).
			arg(OpusSampleRate).arg(OpusChannels));
	}
	LOG_INFO(QString("MediaEngine: audio devices, input '%1' %2 Hz/%3 ch, output '%4' %5 Hz/%6 ch").
		arg(inputDevice.description()).arg(FSourceFormat.sampleRate()).arg(FSourceFormat.channelCount()).
		arg(outputDevice.description()).arg(FSinkFormat.sampleRate()).arg(FSinkFormat.channelCount()));
	FCaptureConverter.configure(FSourceFormat.sampleRate(), FSourceFormat.channelCount(),
		OpusSampleRate, OpusChannels);
	FPlaybackConverter.configure(OpusSampleRate, OpusChannels,
		FSinkFormat.sampleRate(), FSinkFormat.channelCount());

	FSource = new QAudioSource(inputDevice, FSourceFormat, this);
	FSink = new QAudioSink(outputDevice, FSinkFormat, this);
	FSource->setBufferSize(FSourceFormat.bytesForDuration(80000)); //80 ms
	FSink->setBufferSize(FSinkFormat.bytesForDuration(80000));

	//The canceller filters the microphone with the audio we are playing. It is
	//optional: without the WebRTC library, or when the user turned it off on the
	//Calls options page, the call simply has no echo control.
	delete FEchoCanceller;
	FEchoCanceller = nullptr;
	if (Options::node(OPV_JINGLE_AEC).value().toBool())
	{
		EchoCanceller *echo = new EchoCanceller;
		if (echo->initialize())
		{
			FEchoCanceller = echo;
			LOG_INFO(QString("MediaEngine: echo cancellation enabled (%1)").arg(echo->description()));
		}
		else
		{
			delete echo;
			LOG_INFO("MediaEngine: echo cancellation is not available, calls have no echo control");
		}
	}
	else
	{
		LOG_INFO("MediaEngine: echo cancellation is disabled in the options");
	}

	FCaptureIo = FSource->start();
	FPlaybackIo = FSink->start();
	if (!FCaptureIo || !FPlaybackIo)
	{
		stopAudio();
		return false;
	}

	FAudioActive = true;

	//Start the playout and the RTCP clock from a clean state: packets that
	//arrived before the media path was ready must not add latency
	FJitter.reset(clockMs());
	FSeqTracker.reset();
	FLastPlayoutMs = -1;
	FPlayoutDue = 0;
	FCapturePending.clear();
	FRtcpSession->start();
	scheduleRtcpReport(RtcpMinIntervalMs / 2 + int(QRandomGenerator::global()->bounded(RtcpMinIntervalMs / 2)));

	FMediaTimer->start();
	return true;
}

void MediaEngine::stopAudio()
{
	FMediaTimer->stop();
	FAudioActive = false;
	FRtcpTimer->stop();
	FRtcpSession->stop();

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
	FCaptureConverter.reset();
	FPlaybackConverter.reset();
	delete FEchoCanceller;
	FEchoCanceller = nullptr;
}
