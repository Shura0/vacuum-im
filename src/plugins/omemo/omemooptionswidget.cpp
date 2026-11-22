#include "omemooptionswidget.h"

#include "omemoaccount.h"
#include "omemodefinitions.h"

#include <definitions/optionvalues.h>
#include <utils/options.h>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

OmemoOptionsWidget::OmemoOptionsWidget(Omemo *APlugin, OmemoAccount *AAccount,
	IOptionsManager *AOptionsManager, QWidget *AParent) :
	QWidget(AParent),
	FPlugin(APlugin),
	FAccount(AAccount),
	FPolicyCombo(nullptr),
	FLabel(nullptr)
{
	Q_UNUSED(FPlugin);
	QVBoxLayout *layout = new QVBoxLayout(this);

	FLabel = new QLineEdit(this);
	FLabel->setPlaceholderText(tr("For example: phone, work laptop"));
	connect(FLabel,SIGNAL(textEdited(const QString &)),SIGNAL(modified()));

	QFormLayout *form = new QFormLayout;
	form->addRow(tr("Device name:"),FLabel);
	layout->addLayout(form);

	FPolicyCombo = new QComboBox(this);
	FPolicyCombo->addItem(tr("Trust new contacts' devices automatically (TOFU/BTBV)"),OMEMO_POLICY_BTBV);
	FPolicyCombo->addItem(tr("Trust approved devices only (strict)"),OMEMO_POLICY_STRICT);
	if (AOptionsManager != nullptr)
	{
		IOptionsDialogWidget *policyWidget = AOptionsManager->newOptionsDialogWidget(
			Options::node(OPV_OMEMO_GLOBAL_POLICY),tr("Trust policy:"),FPolicyCombo,this);
		addChildOptionsWidget(policyWidget);
		layout->addWidget(policyWidget->instance());
	}

	QPushButton *clearButton = new QPushButton(tr("Clear all OMEMO sessions"),this);
	connect(clearButton,SIGNAL(clicked(bool)),SLOT(onClearSessionsClicked()));
	layout->addWidget(clearButton);

	QLabel *hint = new QLabel(tr("Clearing sessions drops the encrypted ratchets; they are rebuilt from the devices' published keys. Your identity, trust decisions and readable history are kept."),this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	layout->addStretch(1);
}

void OmemoOptionsWidget::apply()
{
	if (FAccount != nullptr)
		FAccount->setDeviceLabel(FLabel->text());
	emit childApply();
}

void OmemoOptionsWidget::reset()
{
	FLabel->setText(FAccount != nullptr ? FAccount->deviceLabel() : QString());
	emit childReset();
}

void OmemoOptionsWidget::onClearSessionsClicked()
{
	if (FAccount == nullptr)
		return;
	QMessageBox::StandardButton result = QMessageBox::question(this,tr("Clear OMEMO sessions"),
		tr("Drop all encrypted sessions of this account? They will be rebuilt from the published keys; your identity, trust decisions and readable history are kept."),
		QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel);
	if (result == QMessageBox::Yes)
	{
		FAccount->clearSessions();
		QMessageBox::information(this,tr("OMEMO"),tr("OMEMO sessions were cleared."));
	}
}
