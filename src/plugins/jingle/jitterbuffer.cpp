#include "jitterbuffer.h"

JitterBuffer::JitterBuffer()
{
	reset(0);
}

void JitterBuffer::reset(qint64 ANowMs)
{
	FPackets.clear();
	FNextSeq = 0;
	FFirstArrivalMs = ANowMs;
	FLastAdaptMs = ANowMs;
	FTargetFrames = DefaultTargetFrames;
	FWaitFrames = 0;
	FEmptyFrames = 0;
	FInsertFrames = 0;
	FStarted = false;
	FLate = 0;
	FDuplicates = 0;
	FConcealed = 0;
	FSkipped = 0;
	FResyncs = 0;
}

void JitterBuffer::growTarget(qint64 ANowMs)
{
	if (FTargetFrames < MaxTargetFrames)
		++FTargetFrames;
	//A packet arrived too late for its playout slot, so the buffer is running
	//short: extend the previous frame by one (up to two) to regain the target
	//depth without dropping any audio.
	if (FStarted && FPackets.size() < FTargetFrames)
		FInsertFrames = qMin(FTargetFrames - FPackets.size(), 2);
	FLastAdaptMs = ANowMs;
}

bool JitterBuffer::push(quint64 AExtendedSeq, const QByteArray &APayload, qint64 ANowMs)
{
	//Once playout started, anything behind the playout position is too late to help
	if (FStarted && AExtendedSeq < FNextSeq)
	{
		++FLate;
		growTarget(ANowMs);
		return false;
	}
	if (FPackets.contains(AExtendedSeq))
	{
		++FDuplicates;
		return false;
	}

	//The peer restarted its stream (new SSRC with the same sequence range or a
	//long stall): drop the stale queue instead of buffering a second of audio.
	if (FStarted && AExtendedSeq > FNextSeq + quint64(ResyncGapFrames))
	{
		FPackets.clear();
		FNextSeq = AExtendedSeq;
		++FResyncs;
		FWaitFrames = 0;
		FEmptyFrames = 0;
		FInsertFrames = 0;
	}

	if (FPackets.size() >= MaxPackets)
	{
		//Keep the newest packets; the oldest ones would be dropped as late anyway
		FPackets.erase(FPackets.begin());
		++FResyncs;
	}

	if (!FStarted && FPackets.isEmpty())
		FFirstArrivalMs = ANowMs;

	FPackets.insert(AExtendedSeq, APayload);
	return true;
}

JitterBuffer::Result JitterBuffer::pop(QByteArray &APayload, qint64 ANowMs)
{
	APayload.clear();

	//Wait for the target depth before the first frame, but never longer than
	//StartupMaxMs: a peer that sends sparse audio must still be heard.
	if (!FStarted)
	{
		if (FPackets.isEmpty())
			return Empty;
		if (FPackets.size() < FTargetFrames && ANowMs - FFirstArrivalMs < StartupMaxMs)
			return Empty;
		FNextSeq = FPackets.firstKey();
		FStarted = true;
	}

	//Widen the buffer by one frame: concealment now, the queued packets play
	//one frame later, which is exactly the extra delay the network asked for.
	if (FInsertFrames > 0 && !FPackets.isEmpty())
	{
		--FInsertFrames;
		++FConcealed;
		return Lost;
	}

	if (FPackets.isEmpty())
	{
		FWaitFrames = 0;
		if (FEmptyFrames < MaxConcealFrames)
		{
			++FEmptyFrames;
			++FConcealed;
			return Lost;
		}
		return Empty;
	}

	const quint64 lowest = FPackets.firstKey();

	//The next frame has not arrived yet: give reordering a short window before
	//declaring the packet lost, because a reordered packet is indistinguishable
	//from a missing one until the window expires.
	if (FNextSeq < lowest)
	{
		if (lowest - FNextSeq > quint64(ResyncGapFrames))
		{
			//The stream stopped and resumed: bridging the gap with concealment
			//would produce seconds of fake audio, so jump to the new position.
			FNextSeq = lowest;
			++FResyncs;
		}
		else if (FWaitFrames < MaxReorderFrames)
		{
			++FWaitFrames;
			return Empty;
		}
		else
		{
			++FNextSeq;
			FWaitFrames = 0;
			++FConcealed;
			FEmptyFrames = 0;
			return Lost;
		}
	}

	//Drop anything that fell behind while we were resynchronizing
	while (!FPackets.isEmpty() && FPackets.firstKey() < FNextSeq)
	{
		FPackets.erase(FPackets.begin());
		++FLate;
	}
	if (FPackets.isEmpty())
	{
		FWaitFrames = 0;
		if (FEmptyFrames < MaxConcealFrames)
		{
			++FEmptyFrames;
			++FConcealed;
			return Lost;
		}
		return Empty;
	}

	//The buffer carries much more than the target delay (a burst after a
	//stall, a sender restart): drop the frame that is about to play instead of
	//adding its latency to the whole call.
	if (FPackets.size() > FTargetFrames + MaxExtraFrames)
	{
		QMap<quint64, QByteArray>::iterator stale = FPackets.find(FNextSeq);
		if (stale != FPackets.end())
		{
			FPackets.erase(stale);
			++FNextSeq;
			++FSkipped;
			FWaitFrames = 0;
		}
	}

	QMap<quint64, QByteArray>::iterator packet = FPackets.find(FNextSeq);
	if (packet == FPackets.end())
		return Empty;

	APayload = packet.value();
	FPackets.erase(packet);
	++FNextSeq;
	FWaitFrames = 0;
	FEmptyFrames = 0;

	//Give the extra delay back slowly once the network has been quiet for a
	//while; one frame per five seconds, down to MinTargetFrames.
	if (FTargetFrames > MinTargetFrames && ANowMs - FLastAdaptMs > 5000)
	{
		--FTargetFrames;
		FLastAdaptMs = ANowMs;
	}

	return Data;
}

int JitterBuffer::depth() const
{
	return FPackets.size();
}

int JitterBuffer::targetFrames() const
{
	return FTargetFrames;
}

quint32 JitterBuffer::latePackets() const
{
	return FLate;
}

quint32 JitterBuffer::duplicatePackets() const
{
	return FDuplicates;
}

quint32 JitterBuffer::concealedFrames() const
{
	return FConcealed;
}

quint32 JitterBuffer::skippedPackets() const
{
	return FSkipped;
}

quint32 JitterBuffer::resyncs() const
{
	return FResyncs;
}
