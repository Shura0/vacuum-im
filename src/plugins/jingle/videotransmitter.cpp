#include "videotransmitter.h"

#include <QCamera>
#include <QCameraDevice>
#include <QDateTime>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoFrame>
#include <QVideoSink>

#include <utils/logger.h>
#include <utils/options.h>
#include <definitions/optionvalues.h>

//The small part of the libvpx encoder ABI used here. Only the shared library
//is needed at runtime; the offsets were verified against the libvpx headers.
struct VpxImage
{
	int fmt;
	int cs;
	int range;
	unsigned int w;
	unsigned int h;
	unsigned int bit_depth;
	unsigned int d_w;
	unsigned int d_h;
	unsigned int r_w;
	unsigned int r_h;
	unsigned int x_chroma_shift;
	unsigned int y_chroma_shift;
	unsigned char *planes[4];
	int stride[4];
};

struct VpxCxPacket
{
	int kind;
	char pad[4];
	void *buf;
	unsigned long sz;
};

typedef void *(*VpxVp8CxFunc)();
typedef int (*VpxEncConfigDefaultFunc)(void *, void *, unsigned int);
typedef int (*VpxEncInitFunc)(void *, void *, const void *, unsigned long, int);
typedef int (*VpxEncConfigSetFunc)(void *, const void *);
typedef VpxImage *(*VpxImgWrapFunc)(void *, int, unsigned int, unsigned int, unsigned int, unsigned char *);
typedef void (*VpxImgFreeFunc)(VpxImage *);
typedef int (*VpxEncodeFunc)(void *, const VpxImage *, qint64, unsigned long, unsigned int, unsigned long);
typedef const VpxCxPacket *(*VpxGetCxDataFunc)(void *, const void **);
typedef int (*VpxDestroyFunc)(void *);

static const int VpxImgFmtI420 = 258;
static const int VpxEncoderAbiVersion = 39;
static const int VpxFramePkt = 0;
static const int VideoWidth = 320;
static const int VideoHeight = 240;
static const int VideoFps = 15;

struct VideoQualityLevel { int width; int height; int bitrateKbps; };
static const VideoQualityLevel kQualityLevels[] = {
	{ 320, 240, 300 },
	{ 640, 480, 600 },
	{ 1280, 720, 1200 },
};
static const int kQualityLevelCount = int(sizeof(kQualityLevels)/sizeof(kQualityLevels[0]));

VideoTransmitter::VideoTransmitter(QObject *AParent) : QObject(AParent)
{
	FCamera = nullptr;
	FSession = nullptr;
	FSink = nullptr;
	FLib = nullptr;
	FReady = false;
	FSize = QSize(VideoWidth, VideoHeight);
	FPts = 0;
	FLastFrameMs = 0;
	FEncodedFrames = 0;
	FQualityMode = 0;
	FLevel = 0;
	FGoodReports = 0;
	FBadReports = 0;
}

VideoTransmitter::~VideoTransmitter()
{
	stop();
	VpxDestroyFunc destroy = FLib!=nullptr ?
		reinterpret_cast<VpxDestroyFunc>(FLib->resolve("vpx_codec_destroy")) : nullptr;
	if (destroy && FReady)
		destroy(FCtx);
}

bool VideoTransmitter::initialize()
{
	if (FReady)
		return true;

	FLib = new QLibrary(QStringLiteral("libvpx.so.12"), this);
	if (!FLib->load())
	{
		LOG_WARNING("VideoTransmitter: libvpx is not available, video sending is disabled");
		delete FLib;
		FLib = nullptr;
		return false;
	}

	VpxVp8CxFunc vp8cx = reinterpret_cast<VpxVp8CxFunc>(FLib->resolve("vpx_codec_vp8_cx"));
	VpxEncConfigDefaultFunc configDefault = reinterpret_cast<VpxEncConfigDefaultFunc>(FLib->resolve("vpx_codec_enc_config_default"));
	VpxEncInitFunc encInit = reinterpret_cast<VpxEncInitFunc>(FLib->resolve("vpx_codec_enc_init_ver"));
	VpxEncodeFunc encode = reinterpret_cast<VpxEncodeFunc>(FLib->resolve("vpx_codec_encode"));
	VpxGetCxDataFunc getCxData = reinterpret_cast<VpxGetCxDataFunc>(FLib->resolve("vpx_codec_get_cx_data"));
	if (!vp8cx || !configDefault || !encInit || !encode || !getCxData)
	{
		LOG_WARNING("VideoTransmitter: libvpx has no VP8 encoder, video sending is disabled");
		FLib->unload();
		delete FLib;
		FLib = nullptr;
		return false;
	}

	memset(FCfg, 0, sizeof(FCfg));
	if (configDefault(vp8cx(), FCfg, 0) != 0)
	{
		LOG_WARNING("VideoTransmitter: failed to get the default VP8 encoder configuration");
		return false;
	}

	//Field offsets in vpx_codec_enc_cfg_t (verified with the header):
	//g_w=12, g_h=16, g_timebase=28 (num,den), rc_end_usage=72,
	//rc_target_bitrate=112 (kbit/s), g_threads=4.
	quint32 *cfg = reinterpret_cast<quint32 *>(FCfg);
	cfg[12/4] = quint32(FSize.width());
	cfg[16/4] = quint32(FSize.height());
	cfg[28/4] = 1;     //g_timebase.num
	cfg[32/4] = 90000; //g_timebase.den
	cfg[72/4] = 1;     //VPX_CBR
	cfg[112/4] = 500;  //500 kbit/s
	cfg[4/4] = 2;      //g_threads

	if (encInit(FCtx, vp8cx(), FCfg, 0, VpxEncoderAbiVersion) != 0)
	{
		LOG_WARNING("VideoTransmitter: failed to initialize the VP8 encoder");
		return false;
	}

	FReady = true;
	LOG_INFO("VideoTransmitter: VP8 encoding through libvpx is ready");
	setQualityMode(Options::node(OPV_JINGLE_VIDEO_QUALITY).value().toInt());
	return true;
}

void VideoTransmitter::start()
{
	if (FCamera != nullptr || !initialize())
		return;

	const QCameraDevice device = QMediaDevices::defaultVideoInput();
	if (device.isNull())
	{
		LOG_WARNING("VideoTransmitter: no camera is available");
		return;
	}

	FSession = new QMediaCaptureSession(this);
	FSink = new QVideoSink(this);
	connect(FSink, &QVideoSink::videoFrameChanged, this, &VideoTransmitter::onVideoFrame);

	FCamera = new QCamera(device, this);
	FSession->setCamera(FCamera);
	FSession->setVideoSink(FSink);
	FCamera->start();
	LOG_INFO(QString("VideoTransmitter: camera '%1' started").arg(device.description()));
}

void VideoTransmitter::stop()
{
	if (FSession)
	{
		delete FSession;
		FSession = nullptr;
	}
	if (FCamera)
	{
		FCamera->stop();
		delete FCamera;
		FCamera = nullptr;
	}
	if (FSink)
	{
		delete FSink;
		FSink = nullptr;
	}
}

bool VideoTransmitter::isActive() const
{
	return FCamera != nullptr;
}

bool VideoTransmitter::hasCamera()
{
	return !QMediaDevices::defaultVideoInput().isNull();
}

void VideoTransmitter::setQualityMode(int AMode)
{
	FQualityMode = qBound(0, AMode, kQualityLevelCount);
	FGoodReports = 0;
	FBadReports = 0;
	const int level = FQualityMode == 0 ? 0 : FQualityMode - 1;
	applyQuality(level, FQualityMode == 0 ? QStringLiteral("auto start") : QStringLiteral("manual"));
}

void VideoTransmitter::adaptToNetwork(int ALossPercent, double ARttMs)
{
	if (!FReady || FQualityMode != 0)
		return;

	const bool bad = ALossPercent > 8 || (ARttMs >= 0 && ARttMs > 250);
	const bool good = ALossPercent < 2 && ARttMs >= 0 && ARttMs < 120;
	if (bad)
	{
		++FBadReports;
		FGoodReports = 0;
	}
	else if (good)
	{
		++FGoodReports;
		FBadReports = 0;
	}
	else
	{
		FGoodReports = 0;
		FBadReports = 0;
	}

	if (FBadReports >= 3 && FLevel > 0)
	{
		applyQuality(FLevel - 1, QStringLiteral("high loss/RTT"));
	}
	else if (FGoodReports >= 8 && FLevel < kQualityLevelCount - 1)
	{
		applyQuality(FLevel + 1, QStringLiteral("good network"));
	}
}

bool VideoTransmitter::applyQuality(int ALevel, const QString &AReason)
{
	if (!FReady || ALevel < 0 || ALevel >= kQualityLevelCount)
		return false;
	const int clamped = qBound(0, ALevel, kQualityLevelCount - 1);
	const VideoQualityLevel &level = kQualityLevels[clamped];
	quint32 *cfg = reinterpret_cast<quint32 *>(FCfg);

	VpxEncConfigSetFunc configSet = reinterpret_cast<VpxEncConfigSetFunc>(FLib->resolve("vpx_codec_enc_config_set"));
	VpxEncInitFunc encInit = reinterpret_cast<VpxEncInitFunc>(FLib->resolve("vpx_codec_enc_init_ver"));
	VpxDestroyFunc destroy = reinterpret_cast<VpxDestroyFunc>(FLib->resolve("vpx_codec_destroy"));
	if (!configSet || !encInit || !destroy)
		return false;

	if (clamped != FLevel)
	{
		destroy(FCtx);
		memset(FCtx, 0, sizeof(FCtx));
		cfg[12/4] = quint32(level.width);
		cfg[16/4] = quint32(level.height);
		VpxVp8CxFunc vp8cx = reinterpret_cast<VpxVp8CxFunc>(FLib->resolve("vpx_codec_vp8_cx"));
		if (!vp8cx || encInit(FCtx, vp8cx(), FCfg, 0, VpxEncoderAbiVersion) != 0)
		{
			LOG_WARNING("VideoTransmitter: failed to reinitialize the VP8 encoder for a new resolution");
			FReady = false;
			return false;
		}
		FSize = QSize(level.width, level.height);
		FLevel = clamped;
	}
	cfg[112/4] = quint32(level.bitrateKbps);
	if (configSet(FCtx, FCfg) != 0)
		LOG_WARNING("VideoTransmitter: failed to update the VP8 bitrate");
	FGoodReports = 0;
	FBadReports = 0;
	LOG_INFO(QString("VideoTransmitter: quality level=%1 %2x%3 bitrate=%4 kbps, reason=%5").
		arg(FLevel).arg(level.width).arg(level.height).arg(level.bitrateKbps).arg(AReason));
	return true;
}

void VideoTransmitter::onVideoFrame(const QVideoFrame &AFrame)
{
	submitFrame(AFrame.toImage());
}

void VideoTransmitter::submitFrame(const QImage &AImage)
{
	if (!FReady || AImage.isNull())
		return;
	static int s_submitted = 0;
	if (++s_submitted == 1 || s_submitted % 50 == 0)
		LOG_INFO(QString("VideoTransmitter: submitted frames=%1").arg(s_submitted));

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - FLastFrameMs < 1000 / VideoFps)
		return;
	FLastFrameMs = now;

	QImage image = AImage.scaled(FSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	emit localFrame(image);
	encodeImage(image);
}

bool VideoTransmitter::encodeImage(const QImage &AImage)
{
	if (!FReady)
		return false;

	const int width = AImage.width();
	const int height = AImage.height();
	const int yBytes = width * height;
	QByteArray i420(yBytes + (yBytes / 2), Qt::Uninitialized);
	unsigned char *yPlane = reinterpret_cast<unsigned char *>(i420.data());
	unsigned char *uPlane = yPlane + yBytes;
	unsigned char *vPlane = uPlane + (yBytes / 4);

	const QImage rgb = AImage.convertToFormat(QImage::Format_RGB32);
	for (int y = 0; y < height; y += 2)
	{
		const QRgb *row0 = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
		const QRgb *row1 = y + 1 < height ? reinterpret_cast<const QRgb *>(rgb.constScanLine(y + 1)) : row0;
		for (int x = 0; x < width; x += 2)
		{
			int sumY = 0, sumU = 0, sumV = 0;
			for (int dy = 0; dy < 2 && y + dy < height; ++dy)
			{
				const QRgb *row = dy == 0 ? row0 : row1;
				for (int dx = 0; dx < 2 && x + dx < width; ++dx)
				{
					const int r = qRed(row[x + dx]);
					const int g = qGreen(row[x + dx]);
					const int b = qBlue(row[x + dx]);
					sumY += (77*r + 150*g + 29*b + 128) >> 8;
					sumU += ((-43*r - 85*g + 128*b + 128) >> 8) + 128;
					sumV += ((128*r - 107*g - 21*b + 128) >> 8) + 128;
				}
			}
			const int count = ((y + 1 < height) ? 2 : 1) * ((x + 1 < width) ? 2 : 1);
			yPlane[y*width + x] = quint8(qBound(0, sumY/count, 255));
			yPlane[y*width + x + 1] = (x + 1 < width) ? yPlane[y*width + x] : 0;
			if (y + 1 < height)
				memcpy(yPlane + (y+1)*width + x, yPlane + y*width + x, 2);
			uPlane[(y/2)*(width/2) + x/2] = quint8(qBound(0, sumU/count, 255));
			vPlane[(y/2)*(width/2) + x/2] = quint8(qBound(0, sumV/count, 255));
		}
	}

	VpxImgWrapFunc imgWrap = reinterpret_cast<VpxImgWrapFunc>(FLib->resolve("vpx_img_wrap"));
	VpxImgFreeFunc imgFree = reinterpret_cast<VpxImgFreeFunc>(FLib->resolve("vpx_img_free"));
	VpxEncodeFunc encode = reinterpret_cast<VpxEncodeFunc>(FLib->resolve("vpx_codec_encode"));
	VpxGetCxDataFunc getCxData = reinterpret_cast<VpxGetCxDataFunc>(FLib->resolve("vpx_codec_get_cx_data"));
	if (!imgWrap || !imgFree || !encode || !getCxData)
		return false;

	VpxImage *image = imgWrap(nullptr, VpxImgFmtI420, width, height, 1,
	                          reinterpret_cast<unsigned char *>(i420.data()));
	if (!image)
		return false;

	const qint64 pts = FPts++;
	const int result = encode(FCtx, image, pts, 90000 / VideoFps, 0, 1 /*VPX_DL_REALTIME*/);
	imgFree(image);
	if (result != 0)
	{
		static int s_failures = 0;
		if (++s_failures <= 3)
			LOG_WARNING(QString("VideoTransmitter: vpx_codec_encode failed, result=%1").arg(result));
		return false;
	}

	const void *iter = nullptr;
	const VpxCxPacket *packet = nullptr;
	while ((packet = getCxData(FCtx, &iter)) != nullptr)
	{
		if (packet->kind == VpxFramePkt && packet->buf != nullptr && packet->sz > 0)
		{
			++FEncodedFrames;
			if (FEncodedFrames == 1 || FEncodedFrames % 50 == 0)
				LOG_INFO(QString("VideoTransmitter: encoded frames=%1 size=%2 bytes").arg(FEncodedFrames).arg(int(packet->sz)));
			emit encodedFrame(QByteArray(reinterpret_cast<const char *>(packet->buf), int(packet->sz)),
			                  90000 / VideoFps);
			return true;
		}
	}
	return false;
}
