#ifndef SCREENSHARE_H
#define SCREENSHARE_H

#include <QImage>
#include <QObject>
#include <QVariantMap>

class QDBusError;
class QDBusObjectPath;
class QDBusUnixFileDescriptor;
class QThread;
class ScreenShareThread;

/*
 * Screen sharing source based on xdg-desktop-portal ScreenCast (for the
 * permission dialog and the PipeWire node) and a PipeWire stream that delivers
 * raw video frames. Compiled only when libpipewire is available at build time.
 */
class ScreenShare :
	public QObject
{
	Q_OBJECT;
public:
	explicit ScreenShare(QObject *AParent = nullptr);
	virtual ~ScreenShare();

	static bool isAvailable();
	void start();
	void stop();
	bool isActive() const;

signals:
	void frameReady(const QImage &AFrame);
	void failed(const QString &AError);

private slots:
	void onPortalResponse(uint AResponse, const QVariantMap &AResults);
	void onThreadFinished();

private:
	enum PortalStep { NoStep, CreateSessionStep, SelectSourcesStep, StartStep };

private:
	PortalStep FStep;
	QString FSessionPath;
	ScreenShareThread *FThread;
};

#endif // SCREENSHARE_H
