#include "videoreceiver.h"

#include <utils/logger.h>

//The parts of the libvpx ABI used by the decoder. Declaring them locally keeps
//the plugin buildable without libvpx-dev (only the shared library is needed).
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

typedef void *(*VpxVp8DxFunc)();
typedef int (*VpxDecInitFunc)(void *, void *, const void *, int, int);
typedef int (*VpxDecodeFunc)(void *, const unsigned char *, unsigned int, void *, long);
typedef VpxImage *(*VpxGetFrameFunc)(void *, const void **);
typedef int (*VpxDestroyFunc)(void *);

static const int VpxImgFmtI420 = 0x102; //VPX_IMG_FMT_PLANAR (0x100) | 2
static const int VpxDecoderAbiVersion = 12; //3 + VPX_CODEC_ABI_VERSION (4 + 5)

VideoReceiver::VideoReceiver(QObject *AParent) : QObject(AParent)
{
	FLib = nullptr;
	FReady = false;
}

VideoReceiver::~VideoReceiver()
{
	VpxDestroyFunc destroy = FLib!=nullptr ?
		reinterpret_cast<VpxDestroyFunc>(FLib->resolve("vpx_codec_destroy")) : nullptr;
	if (destroy != nullptr && FReady)
		destroy(FCtx);
}

bool VideoReceiver::initialize()
{
	if (FReady)
		return true;

	FLib = new QLibrary(QStringLiteral("libvpx.so.12"), this);
	if (!FLib->load())
	{
		LOG_WARNING("VideoReceiver: libvpx is not available, incoming video is disabled");
		delete FLib;
		FLib = nullptr;
		return false;
	}

	VpxVp8DxFunc vp8dx = reinterpret_cast<VpxVp8DxFunc>(FLib->resolve("vpx_codec_vp8_dx"));
	VpxDecInitFunc decInit = reinterpret_cast<VpxDecInitFunc>(FLib->resolve("vpx_codec_dec_init_ver"));
	VpxDecodeFunc decodeFunc = reinterpret_cast<VpxDecodeFunc>(FLib->resolve("vpx_codec_decode"));
	VpxGetFrameFunc getFrame = reinterpret_cast<VpxGetFrameFunc>(FLib->resolve("vpx_codec_get_frame"));
	if (!vp8dx || !decInit || !decodeFunc || !getFrame)
	{
		LOG_WARNING("VideoReceiver: libvpx has no VP8 decoder, incoming video is disabled");
		FLib->unload();
		delete FLib;
		FLib = nullptr;
		return false;
	}

	if (decInit(FCtx, vp8dx(), nullptr, 0, VpxDecoderAbiVersion) != 0)
	{
		LOG_WARNING("VideoReceiver: failed to initialize the VP8 decoder");
		FLib->unload();
		delete FLib;
		FLib = nullptr;
		return false;
	}

	FReady = true;
	LOG_INFO("VideoReceiver: VP8 decoding through libvpx is ready");
	return true;
}

bool VideoReceiver::isReady() const
{
	return FReady;
}

bool VideoReceiver::decode(const QByteArray &AData, QImage &AOut)
{
	if (!FReady || AData.isEmpty())
		return false;

	VpxDecodeFunc decodeFunc = reinterpret_cast<VpxDecodeFunc>(FLib->resolve("vpx_codec_decode"));
	VpxGetFrameFunc getFrame = reinterpret_cast<VpxGetFrameFunc>(FLib->resolve("vpx_codec_get_frame"));
	if (decodeFunc(FCtx, reinterpret_cast<const unsigned char *>(AData.constData()),
	               unsigned(AData.size()), nullptr, 0) != 0)
	{
		static int s_failures = 0;
		if (++s_failures <= 3)
			LOG_WARNING(QString("VideoReceiver: VP8 decode failed, payload=%1 bytes").arg(AData.size()));
		return false;
	}

	const void *iter = nullptr;
	VpxImage *image = getFrame(FCtx, &iter);
	if (image == nullptr)
		return false;
	if (image == nullptr || image->fmt != VpxImgFmtI420 || image->w == 0 || image->h == 0)
		return false;

	const int width = int(image->d_w ? image->d_w : image->w);
	const int height = int(image->d_h ? image->d_h : image->h);
	const int chromaHeight = (height + 1) / 2;
	AOut = QImage(width, height, QImage::Format_RGB32);
	if (AOut.isNull())
		return false;

	const unsigned char *py = image->planes[0];
	const unsigned char *pu = image->planes[1];
	const unsigned char *pv = image->planes[2];
	for (int y = 0; y < height; ++y)
	{
		QRgb *line = reinterpret_cast<QRgb *>(AOut.scanLine(y));
		const unsigned char *yrow = py + y * image->stride[0];
		const unsigned char *urow = pu + qMin(y / 2, chromaHeight - 1) * image->stride[1];
		const unsigned char *vrow = pv + qMin(y / 2, chromaHeight - 1) * image->stride[2];
		for (int x = 0; x < width; ++x)
		{
			const int Y = int(yrow[x]) - 16;
			const int U = int(urow[x / 2]) - 128;
			const int V = int(vrow[x / 2]) - 128;
			int r = (298 * Y + 409 * V + 128) >> 8;
			int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
			int b = (298 * Y + 516 * U + 128) >> 8;
			r = qBound(0, r, 255);
			g = qBound(0, g, 255);
			b = qBound(0, b, 255);
			line[x] = qRgb(r, g, b);
		}
	}
	return true;
}
