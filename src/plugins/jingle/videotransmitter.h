#ifndef VIDEOTRANSMITTER_H
#define VIDEOTRANSMITTER_H

#include <QByteArray>
#include <QImage>
#include <QLibrary>
#include <QObject>
#include <QSize>

class QCamera;
class QMediaCaptureSession;
class QVideoFrame;
class QVideoSink;

/*
 * VP8 video transmitter: captures frames from the default camera and encodes
 * them with libvpx loaded at runtime. The encoded bitstream is delivered as
 * one packet per frame; MediaEngine takes care of the VP8 RTP packetization.
 */
class VideoTransmitter :
	public QObject
{
	Q_OBJECT;
public:
	explicit VideoTransmitter(QObject *AParent = nullptr);
	virtual ~VideoTransmitter();

	bool initialize();
	void start();
	void stop();
	bool isActive() const;
	static bool hasCamera();
	void submitFrame(const QImage &AImage);
	void setQualityMode(int AMode);
	void adaptToNetwork(int ALossPercent, double ARttMs);

signals:
	void encodedFrame(const QByteArray &AData, quint32 ADuration90k);
	void localFrame(const QImage &AFrame);

private slots:
	void onVideoFrame(const QVideoFrame &AFrame);

private:
	bool encodeImage(const QImage &AImage);
	bool applyQuality(int ALevel, const QString &AReason);

private:
	QCamera *FCamera;
	QMediaCaptureSession *FSession;
	QVideoSink *FSink;
	QLibrary *FLib;
	alignas(16) char FCtx[8192]; //vpx_codec_ctx_t
	alignas(16) char FCfg[1024];  //vpx_codec_enc_cfg_t
	bool FReady;
	QSize FSize;
	qint64 FPts;
	qint64 FLastFrameMs;
	int FEncodedFrames;
	int FQualityMode;
	int FLevel;
	int FGoodReports;
	int FBadReports;
};

#endif // VIDEOTRANSMITTER_H
