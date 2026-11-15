#include "callsoptionswidget.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <definitions/optionvalues.h>
#include <utils/options.h>

CallsOptionsWidget::CallsOptionsWidget(QWidget *AParent) : QWidget(AParent)
{
	FEnabled = new QCheckBox(tr("Enable voice and video calls"), this);
	FAllowUnknown = new QCheckBox(tr("Allow calls from not authorized contacts"), this);
	connect(FEnabled, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FAllowUnknown, SIGNAL(toggled(bool)), SIGNAL(modified()));

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
	layout->addWidget(FAllowUnknown);
	layout->addWidget(natBox);
	layout->addWidget(hint);
	layout->addStretch();

	reset();
}

void CallsOptionsWidget::apply()
{
	Options::node(OPV_JINGLE_ENABLED).setValue(FEnabled->isChecked());
	Options::node(OPV_JINGLE_ALLOWUNKNOWN).setValue(FAllowUnknown->isChecked());
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
	FAllowUnknown->setChecked(Options::node(OPV_JINGLE_ALLOWUNKNOWN).value().toBool());
	FUseDiscovery->setChecked(Options::node(OPV_JINGLE_USE_DISCOVERY).value().toBool());
	FForceRelay->setChecked(Options::node(OPV_JINGLE_FORCE_RELAY).value().toBool());
	FStunServer->setText(Options::node(OPV_JINGLE_STUN_SERVER).value().toString());
	FTurnServer->setText(Options::node(OPV_JINGLE_TURN_SERVER).value().toString());
	FTurnUsername->setText(Options::node(OPV_JINGLE_TURN_USERNAME).value().toString());
	FTurnPassword->setText(Options::node(OPV_JINGLE_TURN_PASSWORD).value().toString());
}
