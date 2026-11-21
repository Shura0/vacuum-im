#include "callwindow.h"

#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

CallWindow::CallWindow(const QString &ASid, QWidget *AParent)
	: QWidget(AParent)
{
	FSid = ASid;
	FSeconds = 0;
	FProgrammaticClose = false;

	setWindowTitle(tr("Voice Call"));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setMinimumWidth(300);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	FNameLabel = new QLabel(this);
	FNameLabel->setAlignment(Qt::AlignHCenter);
	FNameLabel->setWordWrap(true);
	QFont nameFont = FNameLabel->font();
	nameFont.setPointSize(nameFont.pointSize() + 3);
	nameFont.setBold(true);
	FNameLabel->setFont(nameFont);
	mainLayout->addWidget(FNameLabel);

	FStatusLabel = new QLabel(this);
	FStatusLabel->setAlignment(Qt::AlignHCenter);
	FStatusLabel->setWordWrap(true);
	mainLayout->addWidget(FStatusLabel);

	FTimeLabel = new QLabel(this);
	FTimeLabel->setAlignment(Qt::AlignHCenter);
	FTimeLabel->hide();
	mainLayout->addWidget(FTimeLabel);

	FVideoLabel = new QLabel(this);
	FVideoLabel->setAlignment(Qt::AlignCenter);
	FVideoLabel->setMinimumSize(320, 240);
	FVideoLabel->setScaledContents(true);
	FVideoLabel->hide();
	mainLayout->addWidget(FVideoLabel);

	FLocalVideoLabel = new QLabel(this);
	FLocalVideoLabel->setAlignment(Qt::AlignCenter);
	FLocalVideoLabel->setFixedSize(160, 120);
	FLocalVideoLabel->setScaledContents(true);
	FLocalVideoLabel->hide();
	mainLayout->addWidget(FLocalVideoLabel, 0, Qt::AlignRight);

	FNoCameraLabel = new QLabel(tr("No camera available"), this);
	FNoCameraLabel->setAlignment(Qt::AlignCenter);
	FNoCameraLabel->hide();
	mainLayout->addWidget(FNoCameraLabel);

	mainLayout->addStretch();

	QHBoxLayout *buttonsLayout = new QHBoxLayout();
	buttonsLayout->setContentsMargins(0, 0, 0, 0);

	FAnswerButton = new QPushButton(tr("&Answer"), this);
	FAnswerButton->hide();
	buttonsLayout->addWidget(FAnswerButton);

	FDeclineButton = new QPushButton(tr("&Decline"), this);
	FDeclineButton->hide();
	buttonsLayout->addWidget(FDeclineButton);

	FHangupButton = new QPushButton(tr("Hang &up"), this);
	FHangupButton->hide();
	buttonsLayout->addWidget(FHangupButton);

	FMuteButton = new QPushButton(tr("&Mute"), this);
	FMuteButton->setCheckable(true);
	FMuteButton->hide();
	buttonsLayout->addWidget(FMuteButton);

	FCameraButton = new QPushButton(tr("Camera &on"), this);
	FCameraButton->setCheckable(true);
	FCameraButton->setChecked(true);
	FCameraButton->hide();
	buttonsLayout->addWidget(FCameraButton);

	FScreenButton = new QPushButton(tr("Share &screen"), this);
	FScreenButton->setCheckable(true);
	FScreenButton->hide();
	buttonsLayout->addWidget(FScreenButton);

	mainLayout->addLayout(buttonsLayout);

	connect(FAnswerButton, &QPushButton::clicked, this, &CallWindow::answerRequested);
	connect(FDeclineButton, &QPushButton::clicked, this, &CallWindow::declineRequested);
	connect(FHangupButton, &QPushButton::clicked, this, &CallWindow::hangupRequested);
	connect(FMuteButton, &QPushButton::toggled, this, [this](bool AChecked) {
		FMuteButton->setText(AChecked ? tr("&Unmute") : tr("&Mute"));
		emit muteToggled(AChecked);
	});
	connect(FCameraButton, &QPushButton::toggled, this, [this](bool AChecked) {
		FCameraButton->setText(AChecked ? tr("Camera &on") : tr("Camera &off"));
		emit cameraToggled(AChecked);
	});
	connect(FScreenButton, &QPushButton::toggled, this, [this](bool AChecked) {
		FScreenButton->setText(AChecked ? tr("Stop sharing") : tr("Share &screen"));
		emit screenToggled(AChecked);
	});

	FTimer = new QTimer(this);
	FTimer->setInterval(1000);
	connect(FTimer, &QTimer::timeout, this, &CallWindow::onTimerTick);

	resize(300, 140);
}

CallWindow::~CallWindow()
{

}

void CallWindow::setRemoteVideoFrame(const QImage &AFrame)
{
	if (AFrame.isNull())
		return;
	FVideoLabel->setPixmap(QPixmap::fromImage(AFrame));
	FVideoLabel->show();
}

void CallWindow::setLocalVideoFrame(const QImage &AFrame)
{
	if (AFrame.isNull())
		return;
	FLocalVideoLabel->setPixmap(QPixmap::fromImage(AFrame));
	FLocalVideoLabel->show();
}

void CallWindow::setMuteVisible(bool AVisible)
{
	FMuteButton->setVisible(AVisible);
}

void CallWindow::setCameraVisible(bool AVisible)
{
	FCameraButton->setVisible(AVisible);
}

void CallWindow::setScreenVisible(bool AVisible)
{
	FScreenButton->setVisible(AVisible);
}

void CallWindow::setScreenSharing(bool AEnabled)
{
	QSignalBlocker blocker(FScreenButton);
	FScreenButton->setChecked(AEnabled);
	FScreenButton->setText(AEnabled ? tr("Stop sharing") : tr("Share &screen"));
}

void CallWindow::setMuted(bool AMuted)
{
	FMuteButton->setChecked(AMuted);
}

void CallWindow::setCameraEnabled(bool AEnabled)
{
	FCameraButton->setChecked(AEnabled);
	FCameraButton->setEnabled(AEnabled || FCameraButton->isChecked());
}

void CallWindow::setNoCameraVisible(bool AVisible)
{
	FNoCameraLabel->setVisible(AVisible);
}

void CallWindow::setPeer(const QString &AName, const QString &AJid)
{
	if (AName == AJid || AName.isEmpty())
		FNameLabel->setText(AJid);
	else
		FNameLabel->setText(QString("%1\n%2").arg(AName, AJid));
}

void CallWindow::setStatusText(const QString &AText)
{
	FStatusLabel->setText(AText);
}

void CallWindow::setAnswerVisible(bool AVisible)
{
	FAnswerButton->setVisible(AVisible);
}

void CallWindow::setDeclineVisible(bool AVisible)
{
	FDeclineButton->setVisible(AVisible);
}

void CallWindow::setHangupVisible(bool AVisible)
{
	FHangupButton->setVisible(AVisible);
}

void CallWindow::setTimerActive(bool AActive)
{
	if (AActive)
	{
		FSeconds = 0;
		FTimeLabel->setText("00:00");
		FTimeLabel->show();
		FTimer->start();
	}
	else
	{
		FTimer->stop();
		FTimeLabel->hide();
	}
}

void CallWindow::closeBySession()
{
	FProgrammaticClose = true;
	close();
}

void CallWindow::closeEvent(QCloseEvent *ACloseEvent)
{
	if (!FProgrammaticClose)
	{
		//The user pressed the window close button - let the owner decide what to do with the call
		FProgrammaticClose = true;
		ACloseEvent->accept();
		emit userClosed();
	}
	else
	{
		ACloseEvent->accept();
	}
}

void CallWindow::onTimerTick()
{
	++FSeconds;

	const qint64 minutes = FSeconds / 60;
	const qint64 seconds = FSeconds % 60;
	FTimeLabel->setText(QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0')));
}
