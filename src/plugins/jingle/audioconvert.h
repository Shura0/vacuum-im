#ifndef AUDIOCONVERT_H
#define AUDIOCONVERT_H

#include <QByteArray>
#include <QVector>

/*
 * Converts an interleaved 16-bit audio stream between the format of a device
 * and the format of the codec (48 kHz, interleaved 16-bit).
 *
 * The device boundary is the only place where the two formats can differ: a
 * headset microphone is often mono, a Bluetooth link runs at 8 or 16 kHz, and
 * the sound server may only accept its own rate. The converter resamples with
 * linear interpolation and maps the channel count (mono is duplicated, stereo
 * is averaged down), so the Opus side always stays at 48 kHz.
 *
 * The class is a pure state machine: convert() accepts chunks of any size, an
 * incomplete frame at the end of a chunk is kept for the next call, and the
 * interpolation state is carried across chunks. A rate change adds one frame of
 * latency (the interpolator needs both neighbours); a pure channel change maps
 * every frame directly. No timers are involved.
 */
class AudioConverter
{
public:
	AudioConverter();

	void configure(int AFromRate, int AFromChannels, int AToRate, int AToChannels);
	bool isBypass() const;
	//Returns the converted chunk; a frame that cannot be interpolated yet is held
	QByteArray convert(const QByteArray &AInput);
	void reset();

private:
	int FFromRate;
	int FFromChannels;
	int FToRate;
	int FToChannels;
	double FStep;      //Input frames consumed per output frame
	double FNextTime;  //Absolute position of the next output frame, in input frames
	qint64 FBufferStart; //Absolute index of the first frame held in FBuffer
	QVector<short> FBuffer;
	QByteArray FRemainder;
};

#endif // AUDIOCONVERT_H
