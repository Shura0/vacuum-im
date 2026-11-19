#ifndef ECHOCANCELLER_H
#define ECHOCANCELLER_H

#include <QString>

/*
 * Acoustic echo cancellation for the call audio path, backed by WebRTC's
 * AudioProcessing (the system webrtc-audio-processing library). The canceller
 * works in the codec domain: 10 ms frames of interleaved 16-bit stereo at
 * 48 kHz. The far-end frame is what the speaker is about to play (the decoded
 * RTP stream), the near-end frame is the microphone signal that goes into Opus.
 *
 * When the library is not available at build time initialize() returns false
 * and all methods are no-ops, so calls keep working without the canceller.
 */
class EchoCanceller
{
public:
	EchoCanceller();
	~EchoCanceller();

	bool initialize();
	bool isActive() const;
	QString description() const;

	static int frameBytes();

	//Far-end (playback) audio, 10 ms of interleaved 16-bit stereo
	void processFarEnd(const char *AData, int ALength);
	//Near-end (microphone) audio, processed in place, 10 ms per call
	void processNearEnd(char *AData, int ALength);

private:
	class Private;
	Private *FImpl;
};

#endif // ECHOCANCELLER_H
