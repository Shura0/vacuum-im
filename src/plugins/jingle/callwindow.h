#ifndef CALLWINDOW_H
#define CALLWINDOW_H

#include <QWidget>
#include <QImage>

class QLabel;
class QPushButton;
class QTimer;

class CallWindow :
	public QWidget
{
	Q_OBJECT;
public:
	CallWindow(const QString &ASid, QWidget *AParent = nullptr);
	virtual ~CallWindow();

	void setPeer(const QString &AName, const QString &AJid);
	void setStatusText(const QString &AText);
	void setAnswerVisible(bool AVisible);
	void setDeclineVisible(bool AVisible);
	void setHangupVisible(bool AVisible);
	void setTimerActive(bool AActive);
	void setRemoteVideoFrame(const QImage &AFrame);
	void setLocalVideoFrame(const QImage &AFrame);
	void setMuteVisible(bool AVisible);
	void setCameraVisible(bool AVisible);
	void setScreenVisible(bool AVisible);
	void setScreenSharing(bool AEnabled);
	void setMuted(bool AMuted);
	void setCameraEnabled(bool AEnabled);
	void setNoCameraVisible(bool AVisible);
	void closeBySession();

signals:
	void answerRequested();
	void declineRequested();
	void hangupRequested();
	void muteToggled(bool AMuted);
	void cameraToggled(bool AEnabled);
	void screenToggled(bool AEnabled);
	void userClosed();

protected:
	virtual void closeEvent(QCloseEvent *ACloseEvent);

private slots:
	void onTimerTick();

private:
	QString FSid;
	QLabel *FNameLabel;
	QLabel *FStatusLabel;
	QLabel *FTimeLabel;
	QLabel *FVideoLabel;
	QLabel *FLocalVideoLabel;
	QPushButton *FAnswerButton;
	QPushButton *FDeclineButton;
	QPushButton *FHangupButton;
	QPushButton *FMuteButton;
	QPushButton *FCameraButton;
	QPushButton *FScreenButton;
	QLabel *FNoCameraLabel;
	QTimer *FTimer;
	qint64 FSeconds;
	bool FProgrammaticClose;
};

#endif // CALLWINDOW_H
