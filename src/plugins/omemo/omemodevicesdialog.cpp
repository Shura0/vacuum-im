#include "omemodevicesdialog.h"

#include "omemoaccount.h"
#include "omemostore.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

OmemoDevicesDialog::OmemoDevicesDialog(OmemoAccount *AAccount, const Jid &AStreamJid,
	const Jid &AContactJid, QWidget *AParent) :
	QDialog(AParent),
	FAccount(AAccount),
	FStreamJid(AStreamJid),
	FContactJid(AContactJid)
{
	setWindowTitle(tr("OMEMO devices — %1").arg(FContactJid.bare()));
	resize(620,420);

	QVBoxLayout *layout = new QVBoxLayout(this);

	FOwnLabel = new QLabel(this);
	FOwnLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(FOwnLabel);

	FStatusLabel = new QLabel(this);
	FStatusLabel->setWordWrap(true);
	layout->addWidget(FStatusLabel);

	FDevicesList = new QListWidget(this);
	FDevicesList->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(FDevicesList,1);

	QHBoxLayout *buttons = new QHBoxLayout;
	FTrustButton = new QPushButton(tr("Trust selected"),this);
	FDistrustButton = new QPushButton(tr("Block selected"),this);
	QPushButton *closeButton = new QPushButton(tr("Close"),this);
	buttons->addWidget(FTrustButton);
	buttons->addWidget(FDistrustButton);
	buttons->addStretch(1);
	buttons->addWidget(closeButton);
	layout->addLayout(buttons);

	connect(FTrustButton,SIGNAL(clicked()),SLOT(onTrustClicked()));
	connect(FDistrustButton,SIGNAL(clicked()),SLOT(onDistrustClicked()));
	connect(closeButton,SIGNAL(clicked()),SLOT(accept()));
	connect(FAccount,SIGNAL(stateChanged(QString)),SLOT(refresh()));

	FAccount->refreshDeviceList(FContactJid.bare());
	FAccount->refreshDeviceList(FStreamJid.bare());
	FAccount->fetchDeviceBundles(FContactJid.bare());
	refresh();
}

void OmemoDevicesDialog::refresh()
{
	if (FAccount == nullptr)
		return;

	const quint32 ownDevice = FAccount->crypto() != nullptr ? FAccount->crypto()->deviceId() : 0;
	QList<quint32> ownDevices = FAccount->devices(FStreamJid.bare());
	if (ownDevice != 0 && !ownDevices.contains(ownDevice))
		ownDevices.append(ownDevice);
	QStringList ownIds;
	for (quint32 id : ownDevices)
		ownIds.append(QString::number(id));
	FOwnLabel->setText(tr("This device: %1\nMy devices: %2\nFingerprint: %3")
		.arg(ownDevice).arg(ownIds.join(QStringLiteral(", "))).arg(FAccount->ownFingerprint()));

	const QString contact = FContactJid.bare();
	const QList<quint32> devices = FAccount->devices(contact);
	if (FAccount->isDeviceListKnown(contact))
		FAccount->fetchDeviceBundles(contact);
	if (!FAccount->isDeviceListKnown(contact))
	{
		FStatusLabel->setText(tr("The device list is not loaded yet. "
			"It will be fetched from the contact's PEP node."));
	}
	else if (devices.isEmpty())
	{
		FStatusLabel->setText(tr("This contact has not published any OMEMO devices."));
	}
	else
	{
		FStatusLabel->setText(tr("Compare fingerprints with the contact using a trusted channel."));
	}

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
