#ifndef JITTERBUFFER_H
#define JITTERBUFFER_H

#include <QByteArray>
#include <QMap>

/*
 * Playout buffer for one incoming RTP stream. It absorbs network reordering and
 * jitter, adapts its depth to the lateness actually observed and reports missing
 * frames so that the caller can run the decoder's packet loss concealment.
 *
 * The class is a pure state machine: every call takes the current time in
 * milliseconds, so the playout timing is deterministic and can be tested
 * without a socket, a timer or an audio device.
 */
class JitterBuffer
{
public:
	enum Result {
		Empty = 0, //Nothing is due: the buffer is still filling or the stream stalled
		Data = 1,  //APayload holds the packet to decode
		Lost = 2   //The next frame is missing, the caller should conceal it
	};

	JitterBuffer();

	void reset(qint64 ANowMs);
	//Returns false for a late, duplicated or implausibly delayed packet
	bool push(quint64 AExtendedSeq, const QByteArray &APayload, qint64 ANowMs);
	//One playout step; call it as often as the audio clock needs a frame
	Result pop(QByteArray &APayload, qint64 ANowMs);

	int depth() const;
	int targetFrames() const;
	quint32 latePackets() const;
	quint32 duplicatePackets() const;
	quint32 concealedFrames() const;
	quint32 skippedPackets() const;
	quint32 resyncs() const;

private:
	static const int DefaultTargetFrames = 3; //60 ms of slack for a typical LAN/Wi-Fi link
	static const int MinTargetFrames = 2;     //40 ms
	static const int MaxTargetFrames = 10;    //200 ms
	static const int MaxReorderFrames = 3;    //60 ms to wait for one missing packet
	static const int MaxConcealFrames = 5;    //100 ms of concealment, then silence
	static const int StartupMaxMs = 300;      //Do not wait longer for the target depth
	static const int ResyncGapFrames = 50;    //1 s of silence: jump instead of concealing
	static const int MaxExtraFrames = 2;      //40 ms above the target triggers acceleration
	static const int MaxPackets = 512;        //Hard cap, ~10 s of packets

private:
	void growTarget(qint64 ANowMs);

private:
	QMap<quint64, QByteArray> FPackets;
	quint64 FNextSeq;
	qint64 FFirstArrivalMs;
	qint64 FLastAdaptMs;
	int FTargetFrames;
	int FWaitFrames;
	int FEmptyFrames;
	int FInsertFrames; //Playout frames of concealment still owed after a late packet
	bool FStarted;
	quint32 FLate;
	quint32 FDuplicates;
	quint32 FConcealed;
	quint32 FSkipped;
	quint32 FResyncs;
};

#endif // JITTERBUFFER_H
