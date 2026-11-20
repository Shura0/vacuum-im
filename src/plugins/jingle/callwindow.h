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
	void closeBySession();

signals:
	void answerRequested();
	void declineRequested();
	void hangupRequested();
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
	QPushButton *FAnswerButton;
	QPushButton *FDeclineButton;
	QPushButton *FHangupButton;
	QTimer *FTimer;
	qint64 FSeconds;
	bool FProgrammaticClose;
};

#endif // CALLWINDOW_H
