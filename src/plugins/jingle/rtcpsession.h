#ifndef RTCPSESSION_H
#define RTCPSESSION_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

/*
 * RFC 3550 RTCP for one audio stream: reception statistics (expected/received,
 * fraction and cumulative loss, interarrival jitter), compound SR/RR reports
 * every few seconds, sender reports carrying the NTP/RTP mapping, and parsing
 * of the peer's reports (its loss/jitter view of our stream and the round-trip
 * time derived from the LSR/DLSR fields).
 *
 * Like the jitter buffer this class does not own a timer: the caller supplies
 * the current time, so reports can be built deterministically in tests.
 */
class RtcpSession :
	public QObject
{
	Q_OBJECT;
public:
	struct PeerReport
	{
		PeerReport() : valid(false), fractionLost(0), cumulativeLost(0),
			extendedHighestSeq(0), jitter(0), roundTripMs(-1.0) {}

		bool valid;
		int fractionLost;      //0..255, the peer's loss fraction on our stream
		int cumulativeLost;    //Signed 24-bit value as sent by the peer
		quint32 extendedHighestSeq;
		quint32 jitter;        //RTP timestamp units
		double roundTripMs;    //-1 when no round trip can be derived
	};

	explicit RtcpSession(QObject *AParent = nullptr);
	virtual ~RtcpSession();

	void setLocalSsrc(quint32 ASsrc);
	void setRemoteSsrc(quint32 ASsrc);
	quint32 remoteSsrc() const;
	void setClockRate(int AClockRate);

	void start();
	void stop();
	bool isActive() const;

	//Reception statistics: call for every decrypted inbound RTP packet
	void onRtpReceived(quint64 AExtendedSeq, quint32 ARtpTimestamp, qint64 AArrivalMs);
	//Sender statistics: call for every outbound RTP packet
	void onRtpSent(quint32 ARtpTimestamp, int APayloadBytes, qint64 ANowMs);
	//A decrypted inbound RTCP compound packet
	void onRtcpReceived(const QByteArray &APacket, qint64 ANowMs);

	//Builds the next compound report (SR while we send audio, RR otherwise).
	//Calling it advances the "since the last report" counters.
	QByteArray buildReport(qint64 ANowMs);
	QByteArray buildBye() const;

	PeerReport peerReport() const;
	int fractionLost() const;      //Our last reported loss fraction of the peer's stream
	int cumulativeLost() const;
	double jitterMs() const;
	quint64 receivedPackets() const;
	quint64 expectedPackets() const;

signals:
	void reportSent(const QString &ASummary);
	void peerReportReceived(const QString &ASummary);
	void peerBye();

private:
	void parseSenderReport(const char *APacket, int ASize, qint64 ANowMs);
	void parseReceiverReport(const char *APacket, int ASize, qint64 ANowMs);
	void parseReportBlocks(const char *APacket, int ASize, int AOffset, int ACount, qint64 ANowMs);
	void rememberLocalSr(quint32 AMiddleBits, qint64 ANowMs);
	QString peerReportSummary() const;

private:
	quint32 FLocalSsrc;
	quint32 FRemoteSsrc;
	int FClockRate;
	bool FActive;

	//Reception statistics of the peer's stream (RFC 3550 6.4.1 and A.3)
	bool FSeqInitialized;
	quint64 FBaseExtSeq;
	quint64 FMaxExtSeq;
	quint64 FReceived;
	quint64 FExpectedPrior;
	quint64 FReceivedPrior;
	bool FHasTransit;
	quint32 FLastTransit;
	double FJitter;
	int FLastFraction;

	//Sender statistics for the SR (RFC 3550 6.4.1)
	bool FHasSent;
	quint64 FSentPackets;
	quint64 FSentOctets;
	quint32 FLastSentTimestamp;
	qint64 FLastSentMs;

	//The peer's last sender report, echoed back in LSR/DLSR
	bool FHasPeerSr;
	quint32 FLastSrMiddleBits;
	qint64 FLastSrArrivalMs;

	//Our NTP middle bits -> send time, for the round trip calculation
	QHash<quint32, qint64> FLocalSrTimes;

	PeerReport FPeerReport;
};

#endif // RTCPSESSION_H
