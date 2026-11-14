#ifndef RTPDEFS_H
#define RTPDEFS_H

#include <QByteArray>
#include <QtEndian>

//Fixed RTP header without CSRCs or extensions (RFC 3550 5.1)
static const int RtpHeaderSize = 12;

struct RtpHeader
{
	RtpHeader() : valid(false), payloadType(0), marker(false), sequence(0),
		timestamp(0), ssrc(0), payloadOffset(0), payloadLength(0) {}

	bool valid;
	int payloadType;
	bool marker;
	quint16 sequence;
	quint32 timestamp;
	quint32 ssrc;
	int payloadOffset;   //First byte after the header, the CSRCs and the extension
	int payloadLength;   //Payload size without the RTP padding
};

//Parses the fixed header plus the CSRC list and the header extension so that
//payloadOffset always points at the codec payload.
inline RtpHeader parseRtpHeader(const QByteArray &AData)
{
	RtpHeader header;
	if (AData.size() < RtpHeaderSize)
		return header;
	if ((quint8(AData.at(0)) >> 6) != 2)
		return header;

	header.payloadType = quint8(AData.at(1)) & 0x7F;
	header.marker = quint8(AData.at(1)) & 0x80;
	header.sequence = qFromBigEndian<quint16>(AData.constData() + 2);
	header.timestamp = qFromBigEndian<quint32>(AData.constData() + 4);
	header.ssrc = qFromBigEndian<quint32>(AData.constData() + 8);

	int offset = RtpHeaderSize + (quint8(AData.at(0)) & 0x0F) * 4;
	if (offset > AData.size())
		return header;
	if (quint8(AData.at(0)) & 0x10)
	{
		if (offset + 4 > AData.size())
			return header;
		const quint16 words = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		offset += 4 + words * 4;
		if (offset > AData.size())
			return header;
	}
	header.payloadOffset = offset;
	int length = AData.size() - offset;
	if (length > 0 && (quint8(AData.at(0)) & 0x20))
	{
		//RFC 3550 5.1: the last octet counts the padding, itself included
		const int padding = quint8(AData.at(AData.size() - 1));
		if (padding <= 0 || padding > length)
			return header;
		length -= padding;
	}
	if (length <= 0)
		return header; //A header-only packet carries no payload
	header.payloadLength = length;
	header.valid = true;
	return header;
}

//RFC 3550 5.1: the 16-bit sequence number wraps. Extending it to 64 bits keeps
//ordering, loss accounting and the jitter buffer stable across the wrap.
class RtpSeqTracker
{
public:
	RtpSeqTracker() { reset(); }

	void reset()
	{
		FInitialized = false;
		FLast = 0;
		FExtended = 0;
	}

	quint64 extend(quint16 ASequence)
	{
		if (!FInitialized)
		{
			FInitialized = true;
			FLast = ASequence;
			FExtended = ASequence;
			return FExtended;
		}
		//A jump smaller than half of the sequence space is forward movement;
		//anything larger moved backwards into the previous cycle (or is late).
		const int delta = int(quint16(ASequence - FLast));
		if (delta < 0x8000)
			FExtended += quint64(delta);
		else
			FExtended -= quint64(quint16(FLast - ASequence));
		FLast = ASequence;
		return FExtended;
	}

	quint64 highest() const { return FExtended; }
	bool isValid() const { return FInitialized; }

private:
	bool FInitialized;
	quint16 FLast;
	quint64 FExtended;
};

#endif // RTPDEFS_H
