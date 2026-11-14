#include "audioconvert.h"

#include <cmath>
#include <cstring>

AudioConverter::AudioConverter()
{
	FFromRate = 0;
	FFromChannels = 0;
	FToRate = 0;
	FToChannels = 0;
	FStep = 1.0;
	FNextTime = 0.0;
	FBufferStart = 0;
	reset();
}

void AudioConverter::configure(int AFromRate, int AFromChannels, int AToRate, int AToChannels)
{
	FFromRate = AFromRate;
	FFromChannels = AFromChannels;
	FToRate = AToRate;
	FToChannels = AToChannels;
	FStep = (AFromRate > 0 && AToRate > 0) ? (double(AFromRate) / double(AToRate)) : 1.0;
	reset();
}

bool AudioConverter::isBypass() const
{
	return FFromChannels > 0 && FFromChannels == FToChannels && FFromRate > 0 && FFromRate == FToRate;
}

void AudioConverter::reset()
{
	FNextTime = 0.0;
	FBufferStart = 0;
	FBuffer.clear();
	FRemainder.clear();
}

QByteArray AudioConverter::convert(const QByteArray &AInput)
{
	if (FFromChannels <= 0 || FToChannels <= 0 || FFromRate <= 0 || FToRate <= 0)
		return QByteArray();
	if (isBypass())
		return AInput;

	//Frames are appended to the buffer with an absolute index so that the
	//interpolation position survives the chunk boundaries: the last frame of a
	//chunk is needed together with the first frame of the next one.
	const int fromFrameBytes = FFromChannels * int(sizeof(short));
	QByteArray data = FRemainder.isEmpty() ? AInput : (FRemainder + AInput);
	const int frames = data.size() / fromFrameBytes;
	FRemainder = (frames > 0) ? data.mid(frames * fromFrameBytes) : data;
	if (frames <= 0)
		return QByteArray();

	const short *input = reinterpret_cast<const short *>(data.constData());

	//Same rate: only the channel layout changes, every frame is mapped directly
	//(this is the common case of a mono headset and it adds no delay at all)
	if (FFromRate == FToRate)
	{
		QByteArray result;
		result.reserve(frames * FToChannels * int(sizeof(short)));
		for (int frame = 0; frame < frames; ++frame)
		{
			const short *samples = input + frame * FFromChannels;
			for (int channel = 0; channel < FToChannels; ++channel)
			{
				short value;
				if (FFromChannels == FToChannels)
					value = samples[channel];
				else if (FFromChannels == 1)
					value = samples[0];
				else
				{
					int sum = 0;
					for (int source = 0; source < FFromChannels; ++source)
						sum += samples[source];
					value = short(qBound(-32768, sum / FFromChannels, 32767));
				}
				result.append(reinterpret_cast<const char *>(&value), int(sizeof(short)));
			}
		}
		return result;
	}

	//Frames are appended to the buffer with an absolute index so that the
	//interpolation position survives the chunk boundaries: the last frame of a
	//chunk is needed together with the first frame of the next one.
	{
		const int first = FBuffer.size();
		FBuffer.resize(first + frames * FFromChannels);
		memcpy(FBuffer.data() + first, input, frames * fromFrameBytes);
	}

	const qint64 lastIndex = FBufferStart + FBuffer.size() / FFromChannels - 1;
	const qint64 firstIndex = static_cast<qint64>(std::floor(FNextTime));

	QByteArray result;
	result.reserve(int((lastIndex - firstIndex + 2) / FStep + 2) * FToChannels * int(sizeof(short)));

	QVector<double> mixed(FFromChannels);
	while (static_cast<qint64>(std::floor(FNextTime)) + 1 <= lastIndex)
	{
		const qint64 base = static_cast<qint64>(std::floor(FNextTime));
		const double fraction = FNextTime - double(base);
		const int index = int(base - FBufferStart);

		for (int channel = 0; channel < FFromChannels; ++channel)
		{
			const double previous = FBuffer.at(index * FFromChannels + channel);
			const double next = FBuffer.at((index + 1) * FFromChannels + channel);
			mixed[channel] = previous + (next - previous) * fraction;
		}

		for (int channel = 0; channel < FToChannels; ++channel)
		{
			double value;
			if (FFromChannels == FToChannels)
				value = mixed.at(channel);
			else if (FFromChannels == 1)
				value = mixed.at(0); //A mono source feeds every output channel
			else
			{
				//Downmix: average the input channels into every output channel
				value = 0.0;
				for (int source = 0; source < FFromChannels; ++source)
					value += mixed.at(source);
				value /= FFromChannels;
			}
			const short sample = short(qBound(-32768, int(std::lround(value)), 32767));
			result.append(reinterpret_cast<const char *>(&sample), int(sizeof(short)));
		}

		FNextTime += FStep;
	}

	//Drop the frames that can no longer take part in an interpolation: the one
	//at floor(FNextTime) and its successor are still needed
	const qint64 dropped = qMin(static_cast<qint64>(std::floor(FNextTime)) - FBufferStart,
		qint64(FBuffer.size() / FFromChannels));
	if (dropped > 0)
	{
		FBuffer.remove(0, int(dropped) * FFromChannels);
		FBufferStart += dropped;
	}

	return result;
}
