#include "rtcpsession.h"

#include <QDateTime>
#include <QtEndian>

namespace
{
void append16(QByteArray &AData, quint16 AValue)
{
	const quint16 value = qToBigEndian<quint16>(AValue);
	AData.append(reinterpret_cast<const char *>(&value), 2);
}

void append32(QByteArray &AData, quint32 AValue)
{
	const quint32 value = qToBigEndian<quint32>(AValue);
	AData.append(reinterpret_cast<const char *>(&value), 4);
}

quint32 read32(const char *AData)
{
	return qFromBigEndian<quint32>(AData);
}

//NTP timestamp: seconds since 1900 and a 32-bit fraction of a second (RFC 3550 4)
quint64 ntpTimestamp()
{
	const qint64 ms = QDateTime::currentMSecsSinceEpoch();
	const quint64 seconds = quint64(ms / 1000) + 2208988800ULL;
	const quint64 fraction = (quint64(ms % 1000) << 32) / 1000;
	return (seconds << 32) | fraction;
}
}

RtcpSession::RtcpSession(QObject *AParent) : QObject(AParent)
{
	FLocalSsrc = 0;
	FRemoteSsrc = 0;
	FClockRate = 48000;
	FActive = false;

	FSeqInitialized = false;
	FBaseExtSeq = 0;
	FMaxExtSeq = 0;
	FReceived = 0;
	FExpectedPrior = 0;
	FReceivedPrior = 0;
	FHasTransit = false;
	FLastTransit = 0;
	FJitter = 0.0;
	FLastFraction = 0;

	FHasSent = false;
	FSentPackets = 0;
	FSentOctets = 0;
	FLastSentTimestamp = 0;
	FLastSentMs = 0;

	FHasPeerSr = false;
	FLastSrMiddleBits = 0;
	FLastSrArrivalMs = 0;
}

RtcpSession::~RtcpSession()
{
}

void RtcpSession::setLocalSsrc(quint32 ASsrc)
{
	FLocalSsrc = ASsrc;
}

void RtcpSession::setRemoteSsrc(quint32 ASsrc)
{
	FRemoteSsrc = ASsrc;
}

quint32 RtcpSession::remoteSsrc() const
{
	return FRemoteSsrc;
}

void RtcpSession::setClockRate(int AClockRate)
{
	if (AClockRate > 0)
		FClockRate = AClockRate;
}

void RtcpSession::start()
{
	FActive = true;
}

void RtcpSession::stop()
{
	FActive = false;
}

bool RtcpSession::isActive() const
{
	return FActive;
}

void RtcpSession::onRtpReceived(quint64 AExtendedSeq, quint32 ARtpTimestamp, qint64 AArrivalMs)
{
	if (!FSeqInitialized)
	{
		FSeqInitialized = true;
		FBaseExtSeq = AExtendedSeq;
		FMaxExtSeq = AExtendedSeq;
		FReceived = 1;
	}
	else
	{
		if (AExtendedSeq > FMaxExtSeq)
			FMaxExtSeq = AExtendedSeq;
		//Every packet that arrived counts; reordered packets and duplicates can
		//make the cumulative loss negative, which RFC 3550 explicitly allows.
		if (AExtendedSeq >= FBaseExtSeq)
			++FReceived;
	}

	//Interarrival jitter (RFC 3550 A.8), kept in RTP timestamp units
	const quint32 arrival = quint32((quint64(AArrivalMs) * quint64(FClockRate)) / 1000);
	const quint32 transit = arrival - ARtpTimestamp;
	if (FHasTransit)
	{
		const qint32 delta = qint32(transit - FLastTransit);
		FJitter += (double(qAbs(delta)) - FJitter) / 16.0;
	}
	FLastTransit = transit;
	FHasTransit = true;
}

void RtcpSession::onRtpSent(quint32 ARtpTimestamp, int APayloadBytes, qint64 ANowMs)
{
	++FSentPackets;
	if (APayloadBytes > 0)
		FSentOctets += quint64(APayloadBytes);
	FLastSentTimestamp = ARtpTimestamp;
	FLastSentMs = ANowMs;
	FHasSent = true;
}

QByteArray RtcpSession::buildReport(qint64 ANowMs)
{
	const quint64 expected = FSeqInitialized ? (FMaxExtSeq - FBaseExtSeq + 1) : 0;
	const qint64 cumulative = qint64(expected) - qint64(FReceived);

	//Loss fraction since the previous report, in 1/256 units (RFC 3550 A.3)
	const quint64 expectedInterval = expected - FExpectedPrior;
	const quint64 receivedInterval = FReceived >= FReceivedPrior ? FReceived - FReceivedPrior : 0;
	const qint64 lostInterval = qint64(expectedInterval) - qint64(receivedInterval);
	int fraction = 0;
	if (expectedInterval > 0 && lostInterval > 0)
		fraction = int((lostInterval * 256) / qint64(expectedInterval));
	fraction = qBound(0, fraction, 255);

	FExpectedPrior = expected;
	FReceivedPrior = FReceived;
	FLastFraction = fraction;

	//The peer's last sender report: LSR is its middle NTP word, DLSR the delay
	quint32 lsr = 0;
	quint32 dlsr = 0;
	if (FHasPeerSr)
	{
		lsr = FLastSrMiddleBits;
		const qint64 delay = ANowMs - FLastSrArrivalMs;
		if (delay > 0)
			dlsr = quint32((quint64(delay) << 16) / 1000);
	}

	QByteArray block;
	if (FRemoteSsrc != 0)
	{
		append32(block, FRemoteSsrc);
		block.append(char(fraction));
		const quint32 lost = quint32(cumulative & 0xFFFFFF);
		block.append(char((lost >> 16) & 0xFF));
		block.append(char((lost >> 8) & 0xFF));
		block.append(char(lost & 0xFF));
		append32(block, quint32(FMaxExtSeq));
		append32(block, quint32(qMin(FJitter, double(0xFFFFFFFFu))));
		append32(block, lsr);
		append32(block, dlsr);
	}

	const int reportCount = block.isEmpty() ? 0 : 1;
	QByteArray packet;
	if (FHasSent)
	{
		//Sender report: the receiver derives the NTP/RTP mapping from it and
		//echoes the LSR/DLSR part back in its report about our stream.
		const quint32 rtpTimestamp = FLastSentTimestamp +
			quint32(qMax<qint64>(0, ANowMs - FLastSentMs) * FClockRate / 1000);
		const quint64 ntp = ntpTimestamp();

		packet.reserve(28 + block.size());
		packet.append(char(0x80 | reportCount));
		packet.append(char(200));
		append16(packet, quint16((28 + block.size()) / 4 - 1));
		append32(packet, FLocalSsrc);
		append32(packet, quint32(ntp >> 32));
		append32(packet, quint32(ntp & 0xFFFFFFFF));
		append32(packet, rtpTimestamp);
		append32(packet, quint32(FSentPackets));
		append32(packet, quint32(FSentOctets));
		packet.append(block);

		rememberLocalSr(quint32(ntp >> 16), ANowMs);
	}
	else
	{
		packet.reserve(8 + block.size());
		packet.append(char(0x80 | reportCount));
		packet.append(char(201));
		append16(packet, quint16((8 + block.size()) / 4 - 1));
		append32(packet, FLocalSsrc);
		packet.append(block);
	}

	const double jitterMs = double(FJitter) * 1000.0 / FClockRate;
	emit reportSent(QString("rtcp report sent: rx=%1 expected=%2 lost=%3 frac=%4% jitter=%5 ms%6").
		arg(FReceived).arg(expected).arg(cumulative).arg(fraction * 100 / 256).
		arg(jitterMs, 0, 'f', 1).
		arg(FPeerReport.valid ? QString(", peer: loss=%1% rtt=%2 ms").arg(FPeerReport.fractionLost * 100 / 256)
			.arg(FPeerReport.roundTripMs < 0 ? QString("n/a") : QString::number(FPeerReport.roundTripMs, 'f', 1))
			: QString()));

	return packet;
}

QByteArray RtcpSession::buildBye() const
{
	QByteArray packet;
	packet.reserve(8);
	packet.append(char(0x80));
	packet.append(char(203));
	append16(packet, 1); //One SSRC, no reason text
	append32(packet, FLocalSsrc);
	return packet;
}

void RtcpSession::onRtcpReceived(const QByteArray &AData, qint64 ANowMs)
{
	int offset = 0;
	while (offset + 4 <= AData.size())
	{
		const quint8 packetType = quint8(AData.at(offset + 1));
		const int words = qFromBigEndian<quint16>(AData.constData() + offset + 2);
		const int size = (words + 1) * 4;
		if (size < 4 || offset + size > AData.size())
			break;

		const char *packet = AData.constData() + offset;
		if (packetType == 200)
			parseSenderReport(packet, size, ANowMs);
		else if (packetType == 201)
			parseReceiverReport(packet, size, ANowMs);
		else if (packetType == 203)
			emit peerBye();

		offset += size;
	}
}

void RtcpSession::parseSenderReport(const char *APacket, int ASize, qint64 ANowMs)
{
	if (ASize < 28)
		return;
	const int count = quint8(APacket[0]) & 0x1F;

	//Remember the middle 32 bits of the peer's NTP timestamp: our next report
	//echoes them as LSR so the peer can compute the round trip time.
	const quint32 seconds = read32(APacket + 8);
	const quint32 fraction = read32(APacket + 12);
	FHasPeerSr = true;
	FLastSrMiddleBits = (seconds << 16) | (fraction >> 16);
	FLastSrArrivalMs = ANowMs;

	parseReportBlocks(APacket, ASize, 28, count, ANowMs);
}

void RtcpSession::parseReceiverReport(const char *APacket, int ASize, qint64 ANowMs)
{
	if (ASize < 8)
		return;
	parseReportBlocks(APacket, ASize, 8, quint8(APacket[0]) & 0x1F, ANowMs);
}

void RtcpSession::parseReportBlocks(const char *APacket, int ASize, int AOffset, int ACount, qint64 ANowMs)
{
	for (int i = 0; i < ACount; ++i)
	{
		const int at = AOffset + i * 24;
		if (at + 24 > ASize)
			break;

		//Only the block that describes our own stream matters to us
		if (read32(APacket + at) != FLocalSsrc || FLocalSsrc == 0)
			continue;

		FPeerReport.valid = true;
		FPeerReport.fractionLost = quint8(APacket[at + 4]);
		int cumulative = int((quint32(quint8(APacket[at + 5])) << 16) |
			(quint32(quint8(APacket[at + 6])) << 8) | quint32(quint8(APacket[at + 7])));
		if (cumulative & 0x800000)
			cumulative -= 0x1000000; //Sign-extend the 24-bit field
		FPeerReport.cumulativeLost = cumulative;
		FPeerReport.extendedHighestSeq = read32(APacket + at + 8);
		FPeerReport.jitter = read32(APacket + at + 12);

		const quint32 lsr = read32(APacket + at + 16);
		const quint32 dlsr = read32(APacket + at + 20);
		double roundTrip = -1.0;
		if (lsr != 0 && FLocalSrTimes.contains(lsr))
			roundTrip = qMax(0.0, double(ANowMs - FLocalSrTimes.value(lsr)) - double(dlsr) * 1000.0 / 65536.0);
		FPeerReport.roundTripMs = roundTrip;

		emit peerReportReceived(peerReportSummary());
	}
}

void RtcpSession::rememberLocalSr(quint32 AMiddleBits, qint64 ANowMs)
{
	FLocalSrTimes.insert(AMiddleBits, ANowMs);
	if (FLocalSrTimes.size() <= 8)
		return;

	//Reports are echoed within a few seconds; drop the oldest correlation entry
	QHash<quint32, qint64>::iterator oldest = FLocalSrTimes.begin();
	for (QHash<quint32, qint64>::iterator it = FLocalSrTimes.begin(); it != FLocalSrTimes.end(); ++it)
	{
		if (it.value() < oldest.value())
			oldest = it;
	}
	FLocalSrTimes.erase(oldest);
}

QString RtcpSession::peerReportSummary() const
{
	if (!FPeerReport.valid)
		return QString();

	const double jitterMs = double(FPeerReport.jitter) * 1000.0 / FClockRate;
	return QString("peer reports our stream: loss=%1% cumulative=%2 highest=%3 jitter=%4 ms rtt=%5 ms").
		arg(FPeerReport.fractionLost * 100 / 256).
		arg(FPeerReport.cumulativeLost).
		arg(FPeerReport.extendedHighestSeq).
		arg(jitterMs, 0, 'f', 1).
		arg(FPeerReport.roundTripMs < 0 ? QString("n/a") : QString::number(FPeerReport.roundTripMs, 'f', 1));
}

RtcpSession::PeerReport RtcpSession::peerReport() const
{
	return FPeerReport;
}

int RtcpSession::fractionLost() const
{
	return FLastFraction;
}

int RtcpSession::cumulativeLost() const
{
	return FSeqInitialized ? int(qint64(FMaxExtSeq - FBaseExtSeq + 1) - qint64(FReceived)) : 0;
}

double RtcpSession::jitterMs() const
{
	return FJitter * 1000.0 / FClockRate;
}

quint64 RtcpSession::receivedPackets() const
{
	return FReceived;
}

quint64 RtcpSession::expectedPackets() const
{
	return FSeqInitialized ? (FMaxExtSeq - FBaseExtSeq + 1) : 0;
}
