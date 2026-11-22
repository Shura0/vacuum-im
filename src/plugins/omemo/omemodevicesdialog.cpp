#include "omemodevicesdialog.h"

#include "omemoaccount.h"
#include "omemostore.h"
#include "omemodefinitions.h"

#include <definitions/optionvalues.h>
#include <utils/options.h>

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

OmemoDevicesDialog::OmemoDevicesDialog(OmemoAccount *AAccount, const Jid &AStreamJid,
	const Jid &AContactJid, QWidget *AParent) :
	QDialog(AParent),
	FAccount(AAccount),
	FStreamJid(AStreamJid),
	FContactJid(AContactJid)
{
	setWindowTitle(tr("OMEMO devices — %1").arg(FContactJid.bare()));
	resize(660,560);

	QVBoxLayout *layout = new QVBoxLayout(this);

	FStrictBox = new QCheckBox(tr("Strict trust: accept messages only from trusted devices of this contact"),this);
	connect(FStrictBox,SIGNAL(toggled(bool)),SLOT(onStrictToggled(bool)));
	layout->addWidget(FStrictBox);

	QGroupBox *contactBox = new QGroupBox(tr("Contact devices"),this);
	QVBoxLayout *contactLayout = new QVBoxLayout(contactBox);
	FDevicesList = new QListWidget(contactBox);
	FDevicesList->setSelectionMode(QAbstractItemView::SingleSelection);
	contactLayout->addWidget(FDevicesList,1);
	QHBoxLayout *contactButtons = new QHBoxLayout;
	FTrustButton = new QPushButton(tr("Trust selected"),contactBox);
	FDistrustButton = new QPushButton(tr("Block selected"),contactBox);
	contactButtons->addWidget(FTrustButton);
	contactButtons->addWidget(FDistrustButton);
	contactButtons->addStretch(1);
	contactLayout->addLayout(contactButtons);
	layout->addWidget(contactBox,1);

	QGroupBox *ownBox = new QGroupBox(tr("Own devices"),this);
	QVBoxLayout *ownLayout = new QVBoxLayout(ownBox);
	FOwnDevicesList = new QListWidget(ownBox);
	FOwnDevicesList->setSelectionMode(QAbstractItemView::SingleSelection);
	ownLayout->addWidget(FOwnDevicesList,1);
	QHBoxLayout *ownButtons = new QHBoxLayout;
	FResetButton = new QPushButton(tr("Reset session"),ownBox);
	FRemoveButton = new QPushButton(tr("Remove device"),ownBox);
	QPushButton *refreshButton = new QPushButton(tr("Refresh device lists"),ownBox);
	ownButtons->addWidget(FResetButton);
	ownButtons->addWidget(FRemoveButton);
	ownButtons->addWidget(refreshButton);
	ownButtons->addStretch(1);
	ownLayout->addLayout(ownButtons);
	layout->addWidget(ownBox,1);

	FStatusLabel = new QLabel(this);
	FStatusLabel->setWordWrap(true);
	layout->addWidget(FStatusLabel);

	FFingerprintLabel = new QLabel(this);
	FFingerprintLabel->setWordWrap(true);
	FFingerprintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(FFingerprintLabel);

	QHBoxLayout *bottom = new QHBoxLayout;
	QPushButton *closeButton = new QPushButton(tr("Close"),this);
	bottom->addStretch(1);
	bottom->addWidget(closeButton);
	layout->addLayout(bottom);

	connect(FTrustButton,SIGNAL(clicked()),SLOT(onTrustClicked()));
	connect(FDistrustButton,SIGNAL(clicked()),SLOT(onDistrustClicked()));
	connect(FResetButton,SIGNAL(clicked()),SLOT(onResetSessionClicked()));
	connect(FRemoveButton,SIGNAL(clicked()),SLOT(onRemoveDeviceClicked()));
	connect(refreshButton,SIGNAL(clicked()),SLOT(onRefreshClicked()));
	connect(closeButton,SIGNAL(clicked()),SLOT(accept()));
	connect(FAccount,SIGNAL(stateChanged(QString)),SLOT(refresh()));
	connect(FDevicesList,SIGNAL(currentItemChanged(QListWidgetItem*,QListWidgetItem*)),
		SLOT(onSelectionChanged()));
	connect(FOwnDevicesList,SIGNAL(currentItemChanged(QListWidgetItem*,QListWidgetItem*)),
		SLOT(onSelectionChanged()));

	FAccount->refreshDeviceList(FContactJid.bare());
	FAccount->refreshDeviceList(FStreamJid.bare());
	FAccount->fetchDeviceBundles(FContactJid.bare());
	refresh();
}

void OmemoDevicesDialog::refresh()
{
	if (FAccount == nullptr)
		return;

	const QString ownJid = FStreamJid.bare();
	const quint32 ownDevice = FAccount->crypto() != nullptr ? FAccount->crypto()->deviceId() : 0;
	QList<quint32> ownDevices = FAccount->devices(ownJid);
	if (ownDevice != 0 && !ownDevices.contains(ownDevice))
		ownDevices.append(ownDevice);
	const QString label = FAccount->deviceLabel();

	const QString contact = FContactJid.bare();
	const bool globalStrict = Options::node(OPV_OMEMO_GLOBAL_POLICY).value().toInt()==OMEMO_POLICY_STRICT;
	{
		QSignalBlocker blocker(FStrictBox);
		FStrictBox->setChecked(FAccount->isChatStrict(contact));
	}
	FStrictBox->setEnabled(!globalStrict);
	FStrictBox->setToolTip(globalStrict
		? tr("Strict trust is already enabled by the global trust policy")
		: tr("When enabled, messages from devices you have not approved are not decrypted"));

	const QList<quint32> devices = FAccount->devices(contact);
	if (FAccount->isDeviceListKnown(contact))
		FAccount->fetchDeviceBundles(contact);
	if (!FAccount->isDeviceListKnown(contact))
		FStatusLabel->setText(tr("The device list is not loaded yet. "
			"It will be fetched from the contact's PEP node."));
	else if (devices.isEmpty())
		FStatusLabel->setText(tr("This contact has not published any OMEMO devices."));
	else
		FStatusLabel->setText(tr("Compare fingerprints with the contact using a trusted channel."));

	// Contact devices
	const quint32 selectedId = FDevicesList->currentItem() != nullptr
		? FDevicesList->currentItem()->data(Qt::UserRole).toUInt() : 0;
	FDevicesList->clear();
	for (quint32 deviceId : devices)
	{
		const int trust = FAccount->deviceTrust(contact,deviceId);
		QString state;
		if (trust == OmemoStore::TrustTrusted)
			state = tr("trusted");
		else if (trust == OmemoStore::TrustDistrusted)
			state = tr("blocked");
		else
			state = tr("not verified");

		QListWidgetItem *item = new QListWidgetItem(
			tr("Device %1 — %2").arg(deviceId).arg(state),FDevicesList);
		item->setData(Qt::UserRole,deviceId);
		const QString fingerprint = FAccount->deviceFingerprint(contact,deviceId);
		item->setToolTip(fingerprint.isEmpty()
			? tr("Fingerprint becomes available after the bundle is fetched")
			: fingerprint);
		if (deviceId == selectedId)
			FDevicesList->setCurrentItem(item);
	}
	FTrustButton->setEnabled(!devices.isEmpty());
	FDistrustButton->setEnabled(!devices.isEmpty());

	// Own devices
	const quint32 selectedOwnId = FOwnDevicesList->currentItem() != nullptr
		? FOwnDevicesList->currentItem()->data(Qt::UserRole).toUInt() : 0;
	FOwnDevicesList->clear();
	for (quint32 deviceId : ownDevices)
	{
		const bool current = FAccount->isOwnDevice(deviceId);
		QString text = current
			? tr("Device %1 — this device").arg(deviceId)
			: tr("Device %1").arg(deviceId);
		if (current && !label.isEmpty())
			text += QStringLiteral(" (%1)").arg(label);
		QListWidgetItem *item = new QListWidgetItem(text,FOwnDevicesList);
		item->setData(Qt::UserRole,deviceId);
		const QString fingerprint = current ? FAccount->ownFingerprint()
			: FAccount->deviceFingerprint(ownJid,deviceId);
		item->setToolTip(fingerprint.isEmpty()
			? tr("Fingerprint becomes available after the bundle is fetched")
			: fingerprint);
		if (deviceId == selectedOwnId)
			FOwnDevicesList->setCurrentItem(item);
	}
	FResetButton->setEnabled(!ownDevices.isEmpty() || !devices.isEmpty());
	FRemoveButton->setEnabled(!ownDevices.isEmpty());
	onSelectionChanged();
}

void OmemoDevicesDialog::onTrustClicked()
{
	QListWidgetItem *item = FDevicesList->currentItem();
	if (item == nullptr || FAccount == nullptr)
		return;
	FAccount->setDeviceTrust(FContactJid.bare(),item->data(Qt::UserRole).toUInt(),
		OmemoStore::TrustTrusted);
	refresh();
}

void OmemoDevicesDialog::onDistrustClicked()
{
	QListWidgetItem *item = FDevicesList->currentItem();
	if (item == nullptr || FAccount == nullptr)
		return;
	FAccount->setDeviceTrust(FContactJid.bare(),item->data(Qt::UserRole).toUInt(),
		OmemoStore::TrustDistrusted);
	refresh();
}

void OmemoDevicesDialog::onStrictToggled(bool AChecked)
{
	if (FAccount == nullptr)
		return;
	FAccount->setChatStrict(FContactJid.bare(),AChecked);
	refresh();
}

void OmemoDevicesDialog::onResetSessionClicked()
{
	if (FAccount == nullptr)
		return;
	QString jid;
	quint32 deviceId = 0;
	QListWidgetItem *ownItem = FOwnDevicesList->currentItem();
	if (ownItem != nullptr)
	{
		jid = FStreamJid.bare();
		deviceId = ownItem->data(Qt::UserRole).toUInt();
	}
	else if (FDevicesList->currentItem() != nullptr)
	{
		jid = FContactJid.bare();
		deviceId = FDevicesList->currentItem()->data(Qt::UserRole).toUInt();
	}
	if (!jid.isEmpty() && deviceId != 0)
		FAccount->resetDeviceSession(jid,deviceId);
	refresh();
}

void OmemoDevicesDialog::onRemoveDeviceClicked()
{
	QListWidgetItem *item = FOwnDevicesList->currentItem();
	if (item == nullptr || FAccount == nullptr)
		return;
	const quint32 deviceId = item->data(Qt::UserRole).toUInt();
	const bool current = FAccount->isOwnDevice(deviceId);
	QMessageBox::StandardButton result = QMessageBox::warning(this,tr("Remove OMEMO device"),
		current
			? tr("Remove the current device? A new identity with new keys will be generated, "
				"the old bundle stays published and this device gets a new id. Continue?")
			: tr("Remove this device from the published device list and delete its session? Continue?"),
		QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel);
	if (result != QMessageBox::Yes)
		return;
	QString error;
	if (!FAccount->removeOwnDevice(deviceId,&error))
		QMessageBox::warning(this,tr("Remove OMEMO device"),error);
	refresh();
}

void OmemoDevicesDialog::onRefreshClicked()
{
	if (FAccount == nullptr)
		return;
	FAccount->refreshDeviceList(FContactJid.bare());
	FAccount->refreshDeviceList(FStreamJid.bare());
	FAccount->fetchDeviceBundles(FContactJid.bare());
	refresh();
}

void OmemoDevicesDialog::onSelectionChanged()
{
	if (FAccount == nullptr)
	{
		FFingerprintLabel->setText(QString());
		return;
	}
	// Both lists can keep a selection at the same time; follow the one the
	// user actually clicked last instead of always preferring own devices.
	QListWidget *source = qobject_cast<QListWidget *>(sender());
	if (source == FDevicesList || source == FOwnDevicesList)
		FLastSelectionList = source;
	QListWidget *list = FLastSelectionList.data();
	if (list != FDevicesList && list != FOwnDevicesList)
		list = FDevicesList->currentItem() != nullptr ? FDevicesList : FOwnDevicesList;

	QString title;
	QString fingerprint;
	if (list == FOwnDevicesList && FOwnDevicesList->currentItem() != nullptr)
	{
		const quint32 ownId = FOwnDevicesList->currentItem()->data(Qt::UserRole).toUInt();
		const bool current = FAccount->isOwnDevice(ownId);
		title = current
			? tr("Fingerprint of this device (%1):").arg(ownId)
			: tr("Fingerprint of own device %1:").arg(ownId);
		fingerprint = current ? FAccount->ownFingerprint()
			: FAccount->deviceFingerprint(FStreamJid.bare(),ownId);
	}
	else if (FDevicesList->currentItem() != nullptr)
	{
		const quint32 contactId = FDevicesList->currentItem()->data(Qt::UserRole).toUInt();
		title = tr("Fingerprint of contact device %1:").arg(contactId);
		fingerprint = FAccount->deviceFingerprint(FContactJid.bare(),contactId);
	}
	if (title.isEmpty())
		FFingerprintLabel->setText(tr("Select a device to see its fingerprint."));
	else
		FFingerprintLabel->setText(title + QLatin1Char('\n')
			+ (fingerprint.isEmpty() ? tr("not available yet") : fingerprint));
}
