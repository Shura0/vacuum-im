#ifndef VIDEORECEIVER_H
#define VIDEORECEIVER_H

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QLibrary>

/*
 * Minimal VP8 video receiver for Jingle RTP. libvpx is loaded at runtime (the
 * same graceful-degrade pattern as Opus): systems without the library still
 * get audio calls. Only decoding is implemented for now; we declare the small
 * subset of the libvpx ABI that we use instead of requiring -dev headers.
 */
class VideoReceiver :
	public QObject
{
	Q_OBJECT;
public:
	explicit VideoReceiver(QObject *AParent = nullptr);
	virtual ~VideoReceiver();

	bool initialize();
	bool isReady() const;
	//A VP8 RTP payload (RFC 7741 descriptor stripped by the caller)
	bool decode(const QByteArray &AData, QImage &AOut);

private:
	QLibrary *FLib;
	alignas(16) char FCtx[8192]; //vpx_codec_ctx_t, allocated generously
	bool FReady;
};

#endif // VIDEORECEIVER_H
