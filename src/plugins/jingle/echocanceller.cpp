#include "echocanceller.h"

#include <utils/logger.h>

#ifdef HAVE_WEBRTC_AEC
#include <modules/audio_processing/include/audio_processing.h>
#include <cstdint>
#endif

//The codec side of the call is always 48 kHz stereo with 20 ms Opus frames;
//WebRTC processes 10 ms frames, so every chunk below is half an Opus frame.
static const int AecSampleRate = 48000;
static const int AecChannels = 2;
static const int AecFrameSamples = AecSampleRate / 100; //480 samples per channel
static const int AecFrameBytes = AecFrameSamples * AecChannels * int(sizeof(qint16));

//EchoCanceller is deliberately not a QObject, so the LOG_* macros (they need
//staticMetaObject) cannot be used here
static void logWarning(const QString &AMessage)
{
	Logger::writeLog(Logger::Warning, QStringLiteral("EchoCanceller"), AMessage);
}

class EchoCanceller::Private
{
public:
#ifdef HAVE_WEBRTC_AEC
	webrtc::AudioProcessing *apm = nullptr;
	webrtc::StreamConfig streamConfig;
#endif
};

EchoCanceller::EchoCanceller()
{
	FImpl = new Private;
}

EchoCanceller::~EchoCanceller()
{
#ifdef HAVE_WEBRTC_AEC
	delete FImpl->apm;
#endif
	delete FImpl;
}

int EchoCanceller::frameBytes()
{
	return AecFrameBytes;
}

bool EchoCanceller::initialize()
{
#ifdef HAVE_WEBRTC_AEC
	if (FImpl->apm)
		return true;

	FImpl->apm = webrtc::AudioProcessingBuilder().Create();
	if (!FImpl->apm)
	{
		logWarning(QString("WebRTC AudioProcessing could not be created"));
		return false;
	}

	webrtc::AudioProcessing::Config config;
	config.echo_canceller.enabled = true;
	config.echo_canceller.mobile_mode = false;
	config.high_pass_filter.enabled = true;

	FImpl->apm->ApplyConfig(config);
	return true;
#else
	logWarning(QString("the plugin was built without webrtc-audio-processing, "
	                   "echo cancellation is unavailable"));
	return false;
#endif
}

bool EchoCanceller::isActive() const
{
#ifdef HAVE_WEBRTC_AEC
	return FImpl->apm != nullptr;
#else
	return false;
#endif
}

QString EchoCanceller::description() const
{
#ifdef HAVE_WEBRTC_AEC
	return QString("WebRTC AEC3, %1 Hz stereo, 10 ms frames").arg(AecSampleRate);
#else
	return QString("not available");
#endif
}

void EchoCanceller::processFarEnd(const char *AData, int ALength)
{
#ifdef HAVE_WEBRTC_AEC
	if (!FImpl->apm || !AData)
		return;

	//The render output of AudioProcessing is not used: the far-end reference
	//only needs to be analysed, the audio we play stays untouched
	char render[AecFrameBytes];
	for (int offset = 0; offset + AecFrameBytes <= ALength; offset += AecFrameBytes)
	{
		FImpl->apm->ProcessReverseStream(
			reinterpret_cast<const qint16 *>(AData + offset),
			FImpl->streamConfig, FImpl->streamConfig,
			reinterpret_cast<qint16 *>(render));
	}
#else
	Q_UNUSED(AData);
	Q_UNUSED(ALength);
#endif
}

void EchoCanceller::processNearEnd(char *AData, int ALength)
{
#ifdef HAVE_WEBRTC_AEC
	if (!FImpl->apm || !AData)
		return;

	for (int offset = 0; offset + AecFrameBytes <= ALength; offset += AecFrameBytes)
	{
		FImpl->apm->ProcessStream(
			reinterpret_cast<const qint16 *>(AData + offset),
			FImpl->streamConfig, FImpl->streamConfig,
			reinterpret_cast<qint16 *>(AData + offset));
	}
#else
	Q_UNUSED(AData);
	Q_UNUSED(ALength);
#endif
}
