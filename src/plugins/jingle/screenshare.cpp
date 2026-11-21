#include "screenshare.h"

#ifdef HAVE_SCREENSHARE

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusUnixFileDescriptor>
#include <QThread>
#include <functional>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <QUuid>


#include <utils/logger.h>

static const char *PortalService = "org.freedesktop.portal.Desktop";
static const char *PortalPath = "/org/freedesktop/portal/desktop";
static const char *PortalInterface = "org.freedesktop.portal.ScreenCast";
static const char *RequestInterface = "org.freedesktop.portal.Request";

static QString makeToken()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').left(12);
}

//Uses GStreamer pipewiresrc (the same backend vokoscreenNG uses on Wayland)
//to import the portal's DMA-BUF stream and convert it to plain RGBA frames.
class ScreenShareThread :
	public QThread
{
public:
	ScreenShareThread(int ARemoteFd, uint ANodeId, int AWidth, int AHeight,
	                  std::function<void(const QImage &)> AFrameCallback,
	                  std::function<void(const QString &)> AErrorCallback) :
		FRemoteFd(ARemoteFd), FNodeId(ANodeId), FWidth(AWidth), FHeight(AHeight),
		FFrameCallback(AFrameCallback), FErrorCallback(AErrorCallback)
	{
	}

	void quitLoop() { requestInterruption(); }

protected:
	void run() override
	{
		gst_init(nullptr, nullptr);

		GstElement *pipeline = gst_pipeline_new("vacuum-screen-share");
		GstElement *src = gst_element_factory_make("pipewiresrc", "screen-src");
		GstElement *convert = gst_element_factory_make("videoconvert", "screen-convert");
		GstElement *capsFilter = gst_element_factory_make("capsfilter", "screen-caps");
		GstElement *sink = gst_element_factory_make("appsink", "screen-sink");
		if (!pipeline || !src || !convert || !capsFilter || !sink)
		{
			FErrorCallback(QStringLiteral("GStreamer elements are missing (pipewiresrc/videoconvert/appsink)"));
			return;
		}

		g_object_set(src, "fd", FRemoteFd, nullptr);
		const QByteArray nodePath = QByteArray::number(FNodeId);
		g_object_set(src, "path", nodePath.constData(), nullptr);

		//Let pipewiresrc negotiate the portal's native format/size; videoconvert
		//turns whatever it delivers into plain RGBA. Forcing size/fps here makes
		//it fail with "no more input formats".
		const QByteArray capsString = "video/x-raw,format=RGBA";
		GstCaps *caps = gst_caps_from_string(capsString.constData());
		g_object_set(capsFilter, "caps", caps, nullptr);
		gst_caps_unref(caps);

		g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, "max-buffers", 2, "drop", TRUE, nullptr);
		g_signal_connect(sink, "new-sample", G_CALLBACK(onNewSample), this);

		gst_bin_add_many(GST_BIN(pipeline), src, convert, capsFilter, sink, nullptr);
		if (!gst_element_link_many(src, convert, capsFilter, sink, nullptr))
		{
			FErrorCallback(QStringLiteral("Failed to link the GStreamer screen pipeline"));
			gst_object_unref(pipeline);
			return;
		}

		const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
		LOG_INFO(QString("ScreenShare: GStreamer pipeline state result=%1, node=%2").
			arg(int(stateResult)).arg(FNodeId));
		if (stateResult == GST_STATE_CHANGE_FAILURE)
		{
			GstBus *bus = gst_element_get_bus(pipeline);
			GstMessage *message = gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND,
				GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
			if (message)
			{
				GError *error = nullptr;
				gchar *debug = nullptr;
				if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
					gst_message_parse_error(message, &error, &debug);
				else
					gst_message_parse_warning(message, &error, &debug);
				LOG_WARNING(QString("ScreenShare: GStreamer %1 from %2: %3 (%4)").
					arg(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ? "error" : "warning").
					arg(GST_MESSAGE_SRC_NAME(message)).
					arg(error ? error->message : "unknown").arg(debug ? debug : ""));
				if (error) g_error_free(error);
				if (debug) g_free(debug);
				gst_message_unref(message);
			}
			gst_object_unref(bus);
		}

		while (!isInterruptionRequested())
			QThread::msleep(50);

		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
		LOG_INFO("ScreenShare: GStreamer pipeline stopped");
	}

private:
	static GstFlowReturn onNewSample(GstElement *ASink, gpointer AData)
	{
		ScreenShareThread *self = static_cast<ScreenShareThread *>(AData);
		GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(ASink));
		if (!sample)
			return GST_FLOW_OK;

		GstBuffer *buffer = gst_sample_get_buffer(sample);
		int width = self->FWidth;
		int height = self->FHeight;
		GstCaps *sampleCaps = gst_sample_get_caps(sample);
		if (sampleCaps && gst_caps_get_size(sampleCaps) > 0)
		{
			const GstStructure *structure = gst_caps_get_structure(sampleCaps, 0);
			gst_structure_get_int(structure, "width", &width);
			gst_structure_get_int(structure, "height", &height);
		}
		GstMapInfo map;
		if (buffer && width > 0 && height > 0 && gst_buffer_map(buffer, &map, GST_MAP_READ))
		{
			QImage frame(static_cast<const uchar *>(map.data), width, height,
			             width * 4, QImage::Format_RGBA8888);
			if (!frame.isNull())
			{
				static int s_frames = 0;
				if (++s_frames == 1)
					LOG_INFO(QString("ScreenShare: first GStreamer frame, %1x%2, bytes=%3").
						arg(width).arg(height).arg(map.size));
				self->FFrameCallback(frame.copy());
			}
			gst_buffer_unmap(buffer, &map);
		}
		gst_sample_unref(sample);
		return GST_FLOW_OK;
	}

private:
	int FRemoteFd;
	uint FNodeId;
	int FWidth;
	int FHeight;
	std::function<void(const QImage &)> FFrameCallback;
	std::function<void(const QString &)> FErrorCallback;
};

ScreenShare::ScreenShare(QObject *AParent) : QObject(AParent)
{
	FStep = NoStep;
	FThread = nullptr;
}

ScreenShare::~ScreenShare()
{
	stop();
}

bool ScreenShare::isAvailable()
{
	return true;
}

bool ScreenShare::isActive() const
{
	return FThread != nullptr;
}

void ScreenShare::start()
{
	if (FThread || FStep != NoStep)
		return;

	QDBusInterface portal(PortalService, PortalPath, PortalInterface, QDBusConnection::sessionBus());
	QVariantMap options;
	options.insert(QStringLiteral("handle_token"), makeToken());
	//The portal asserts (session->token != NULL) if this is missing.
	options.insert(QStringLiteral("session_handle_token"), makeToken());
	QDBusPendingCall call = portal.asyncCall(QStringLiteral("CreateSession"), options);
	QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *AWatcher) {
		QDBusPendingReply<QDBusObjectPath> reply = *AWatcher;
		AWatcher->deleteLater();
		if (reply.isError())
		{
			emit failed(reply.error().message());
			FStep = NoStep;
			return;
		}
		const QString requestPath = reply.value().path();
		FStep = CreateSessionStep;
		QDBusConnection::sessionBus().connect(QString(), requestPath, RequestInterface,
			QStringLiteral("Response"), this, SLOT(onPortalResponse(uint,QVariantMap)));
	});
}

void ScreenShare::onPortalResponse(uint AResponse, const QVariantMap &AResults)
{
	QStringList resultInfo;
	for (const QString &key : AResults.keys())
		resultInfo.append(key + QStringLiteral("=") + AResults.value(key).toString());
	LOG_INFO(QString("ScreenShare: portal response step=%1 code=%2 results=%3").
		arg(FStep).arg(AResponse).arg(resultInfo.join(QStringLiteral(", "))));

	if (AResponse != 0)
	{
		emit failed(QStringLiteral("ScreenCast portal rejected the request (%1)").arg(AResponse));
		FStep = NoStep;
		return;
	}

	QDBusInterface portal(PortalService, PortalPath, PortalInterface, QDBusConnection::sessionBus());
	const QString token = makeToken();
	QVariantMap options;
	options.insert(QStringLiteral("handle_token"), token);

	QString requestPath;
	if (FStep == CreateSessionStep)
	{
		const QVariant sessionValue = AResults.value(QStringLiteral("session_handle"));
		FSessionPath = sessionValue.canConvert<QDBusObjectPath>() ?
			sessionValue.value<QDBusObjectPath>().path() : sessionValue.toString();
		if (FSessionPath.isEmpty())
		{
			emit failed(QStringLiteral("ScreenCast portal did not return a session"));
			FStep = NoStep;
			return;
		}
		options.insert(QStringLiteral("types"), quint32(1)); //monitor
		options.insert(QStringLiteral("multiple"), false);
		options.insert(QStringLiteral("cursor_mode"), quint32(1));
		QDBusPendingReply<QDBusObjectPath> reply = portal.asyncCall(QStringLiteral("SelectSources"),
			QVariant::fromValue(QDBusObjectPath(FSessionPath)), options);
		//handled by the generic watcher below
		QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *AWatcher) {
			QDBusPendingReply<QDBusObjectPath> r = *AWatcher;
			AWatcher->deleteLater();
			if (r.isError())
			{
				emit failed(r.error().message());
				FStep = NoStep;
				return;
			}
			FStep = SelectSourcesStep;
			QDBusConnection::sessionBus().connect(QString(), r.value().path(), RequestInterface,
				QStringLiteral("Response"), this, SLOT(onPortalResponse(uint,QVariantMap)));
		});
		return;
	}
	else if (FStep == SelectSourcesStep)
	{
		QDBusPendingReply<QDBusObjectPath> reply = portal.asyncCall(QStringLiteral("Start"),
			QVariant::fromValue(QDBusObjectPath(FSessionPath)), QString(), options);
		QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *AWatcher) {
			QDBusPendingReply<QDBusObjectPath> r = *AWatcher;
			AWatcher->deleteLater();
			if (r.isError())
			{
				emit failed(r.error().message());
				FStep = NoStep;
				return;
			}
			FStep = StartStep;
			QDBusConnection::sessionBus().connect(QString(), r.value().path(), RequestInterface,
				QStringLiteral("Response"), this, SLOT(onPortalResponse(uint,QVariantMap)));
		});
		return;
	}

	FStep = NoStep;
	const QDBusArgument streamsArg = AResults.value(QStringLiteral("streams")).value<QDBusArgument>();
	uint nodeId = 0;
	QVariantMap props;
	streamsArg.beginArray();
	while (!streamsArg.atEnd())
	{
		streamsArg.beginStructure();
		quint32 id = 0;
		QVariantMap streamProps;
		streamsArg >> id >> streamProps;
		streamsArg.endStructure();
		if (nodeId == 0)
		{
			nodeId = id;
			props = streamProps;
		}
	}
	streamsArg.endArray();
	if (nodeId == 0)
	{
		emit failed(QStringLiteral("ScreenCast portal returned no streams"));
		return;
	}

	QDBusPendingReply<QDBusUnixFileDescriptor> fdReply =
		portal.call(QStringLiteral("OpenPipeWireRemote"), QVariant::fromValue(QDBusObjectPath(FSessionPath)), QVariantMap());
	if (fdReply.isError())
	{
		emit failed(fdReply.error().message());
		return;
	}

	QDBusUnixFileDescriptor fd = fdReply.value();
	int width = 1920;
	int height = 1080;
	const QVariant sizeValue = props.value(QStringLiteral("size"));
	if (sizeValue.isValid() && sizeValue.canConvert<QVariantList>())
	{
		const QVariantList list = sizeValue.toList();
		if (list.size() == 2)
		{
			width = list.at(0).toInt();
			height = list.at(1).toInt();
		}
	}

	FThread = new ScreenShareThread(::dup(fd.fileDescriptor()), nodeId, width, height,
		[this](const QImage &AFrame) { emit frameReady(AFrame); },
		[this](const QString &AError) { emit failed(AError); });
	connect(FThread, &QThread::finished, this, &ScreenShare::onThreadFinished);
	FThread->start();
	LOG_INFO(QString("ScreenShare: screen capture started, node=%1, %2x%3").arg(nodeId).arg(width).arg(height));
}

void ScreenShare::onThreadFinished()
{
	if (FThread)
	{
		FThread->deleteLater();
		FThread = nullptr;
	}
}

void ScreenShare::stop()
{
	if (!FSessionPath.isEmpty())
	{
		QDBusInterface session(PortalService, FSessionPath, QStringLiteral("org.freedesktop.portal.Session"),
		                       QDBusConnection::sessionBus());
		if (session.isValid())
			session.call(QStringLiteral("Close"));
		FSessionPath.clear();
	}
	if (FThread)
	{
		LOG_INFO("ScreenShare: stopping screen capture");
		FThread->quitLoop();
		FThread->wait(3000);
		if (FThread->isRunning())
			FThread->terminate();
		delete FThread;
		FThread = nullptr;
	}
	FStep = NoStep;
}

#else // !HAVE_SCREENSHARE

ScreenShare::ScreenShare(QObject *AParent) : QObject(AParent) { FThread = nullptr; }
ScreenShare::~ScreenShare() {}
bool ScreenShare::isAvailable() { return false; }
bool ScreenShare::isActive() const { return false; }
void ScreenShare::start() {}
void ScreenShare::stop() {}
void ScreenShare::onPortalResponse(uint, const QVariantMap &) {}
void ScreenShare::onThreadFinished() {}

#endif
