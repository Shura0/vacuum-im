#include "callsoptionswidget.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <definitions/optionvalues.h>
#include <utils/options.h>

CallsOptionsWidget::CallsOptionsWidget(QWidget *AParent) : QWidget(AParent)
{
	FEnabled = new QCheckBox(tr("Enable voice and video calls"), this);
	FVideo = new QCheckBox(tr("Receive video from the contact (VP8)"), this);
	FAec = new QCheckBox(tr("Suppress the echo of the speakers (acoustic echo cancellation)"), this);
	FAllowUnknown = new QCheckBox(tr("Allow calls from not authorized contacts"), this);
	connect(FEnabled, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FVideo, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FAec, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FAllowUnknown, SIGNAL(toggled(bool)), SIGNAL(modified()));

	QGroupBox *deviceBox = new QGroupBox(tr("Audio devices"), this);
	FInputDevice = new QComboBox(deviceBox);
	FOutputDevice = new QComboBox(deviceBox);
	connect(FInputDevice, SIGNAL(currentIndexChanged(int)), SIGNAL(modified()));
	connect(FOutputDevice, SIGNAL(currentIndexChanged(int)), SIGNAL(modified()));

	QFormLayout *deviceLayout = new QFormLayout(deviceBox);
	deviceLayout->addRow(tr("Microphone:"), FInputDevice);
	deviceLayout->addRow(tr("Speaker:"), FOutputDevice);

	QGroupBox *natBox = new QGroupBox(tr("NAT traversal"), this);
	FUseDiscovery = new QCheckBox(tr("Use STUN and TURN servers published by the account's server"), natBox);
	FForceRelay = new QCheckBox(tr("Relay all media through TURN (for testing)"), natBox);
	FStunServer = new QLineEdit(natBox);
	FStunServer->setPlaceholderText(tr("empty: only the discovered servers"));
	FTurnServer = new QLineEdit(natBox);
	FTurnServer->setPlaceholderText(tr("host or host:port, e.g. turn.example.org:3478"));
	FTurnUsername = new QLineEdit(natBox);
	FTurnPassword = new QLineEdit(natBox);
	FTurnPassword->setEchoMode(QLineEdit::Password);

	connect(FUseDiscovery, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FForceRelay, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FStunServer, SIGNAL(textEdited(QString)), SIGNAL(modified()));
	connect(FTurnServer, SIGNAL(textEdited(QString)), SIGNAL(modified()));
	connect(FTurnUsername, SIGNAL(textEdited(QString)), SIGNAL(modified()));
	connect(FTurnPassword, SIGNAL(textEdited(QString)), SIGNAL(modified()));

	QFormLayout *natLayout = new QFormLayout(natBox);
	natLayout->addRow(FUseDiscovery);
	natLayout->addRow(FForceRelay);
	natLayout->addRow(tr("STUN server:"), FStunServer);
	natLayout->addRow(tr("TURN server:"), FTurnServer);
	natLayout->addRow(tr("TURN username:"), FTurnUsername);
	natLayout->addRow(tr("TURN password:"), FTurnPassword);

	QLabel *hint = new QLabel(tr("Without STUN and TURN only direct connections between peers on the "
		"same network work. A manual server overrides the discovered one; credentials are used for TURN."), this);
	hint->setWordWrap(true);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(FEnabled);
	layout->addWidget(FVideo);
	layout->addWidget(FAec);
	layout->addWidget(FAllowUnknown);
	layout->addWidget(deviceBox);
	layout->addWidget(natBox);
	layout->addWidget(hint);
	layout->addStretch();

	reset();
}

void CallsOptionsWidget::apply()
{
	Options::node(OPV_JINGLE_ENABLED).setValue(FEnabled->isChecked());
	Options::node(OPV_JINGLE_VIDEO).setValue(FVideo->isChecked());
	Options::node(OPV_JINGLE_AEC).setValue(FAec->isChecked());
	Options::node(OPV_JINGLE_ALLOWUNKNOWN).setValue(FAllowUnknown->isChecked());
	Options::node(OPV_JINGLE_INPUT_DEVICE).setValue(FInputDevice->currentData().toString());
	Options::node(OPV_JINGLE_OUTPUT_DEVICE).setValue(FOutputDevice->currentData().toString());
	Options::node(OPV_JINGLE_USE_DISCOVERY).setValue(FUseDiscovery->isChecked());
	Options::node(OPV_JINGLE_FORCE_RELAY).setValue(FForceRelay->isChecked());
	Options::node(OPV_JINGLE_STUN_SERVER).setValue(FStunServer->text().trimmed());
	Options::node(OPV_JINGLE_TURN_SERVER).setValue(FTurnServer->text().trimmed());
	Options::node(OPV_JINGLE_TURN_USERNAME).setValue(FTurnUsername->text());
	Options::node(OPV_JINGLE_TURN_PASSWORD).setValue(FTurnPassword->text());
	emit modified();
}

void CallsOptionsWidget::reset()
{
	FEnabled->setChecked(Options::node(OPV_JINGLE_ENABLED).value().toBool());
	FVideo->setChecked(Options::node(OPV_JINGLE_VIDEO).value().toBool());
	FAec->setChecked(Options::node(OPV_JINGLE_AEC).value().toBool());
	FAllowUnknown->setChecked(Options::node(OPV_JINGLE_ALLOWUNKNOWN).value().toBool());

	QSignalBlocker inputBlocker(FInputDevice);
	QSignalBlocker outputBlocker(FOutputDevice);
	fillDevices(FInputDevice, QMediaDevices::audioInputs());
	fillDevices(FOutputDevice, QMediaDevices::audioOutputs());
	const int inputIndex = FInputDevice->findData(Options::node(OPV_JINGLE_INPUT_DEVICE).value().toString());
	const int outputIndex = FOutputDevice->findData(Options::node(OPV_JINGLE_OUTPUT_DEVICE).value().toString());
	FInputDevice->setCurrentIndex(inputIndex>=0 ? inputIndex : 0);
	FOutputDevice->setCurrentIndex(outputIndex>=0 ? outputIndex : 0);

	FUseDiscovery->setChecked(Options::node(OPV_JINGLE_USE_DISCOVERY).value().toBool());
	FForceRelay->setChecked(Options::node(OPV_JINGLE_FORCE_RELAY).value().toBool());
	FStunServer->setText(Options::node(OPV_JINGLE_STUN_SERVER).value().toString());
	FTurnServer->setText(Options::node(OPV_JINGLE_TURN_SERVER).value().toString());
	FTurnUsername->setText(Options::node(OPV_JINGLE_TURN_USERNAME).value().toString());
	FTurnPassword->setText(Options::node(OPV_JINGLE_TURN_PASSWORD).value().toString());
}

void CallsOptionsWidget::fillDevices(QComboBox *ACombo, const QList<QAudioDevice> &ADevices)
{
	ACombo->clear();
	ACombo->addItem(tr("System default"), QString());
	foreach (const QAudioDevice &device, ADevices)
		if (!device.isNull())
			ACombo->addItem(device.description(), QString::fromLatin1(device.id().toHex()));
}
