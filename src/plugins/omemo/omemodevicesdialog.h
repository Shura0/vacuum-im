#ifndef OMEMODEVICESDIALOG_H
#define OMEMODEVICESDIALOG_H

#include <QDialog>

#include <utils/jid.h>

class OmemoAccount;
class QLabel;
class QListWidget;
class QPushButton;

// Minimal trust UI: shows the contact's devices and their fingerprints and
// lets the user trust or distrust a device.  The dialog is deliberately
// simple; the full device management screen can replace it later.
class OmemoDevicesDialog : public QDialog
{
	Q_OBJECT
public:
	OmemoDevicesDialog(OmemoAccount *AAccount, const Jid &AStreamJid,
		const Jid &AContactJid, QWidget *AParent = nullptr);

private slots:
	void refresh();
	void onTrustClicked();
	void onDistrustClicked();

private:
	OmemoAccount *FAccount;
	Jid FStreamJid;
	Jid FContactJid;
	QLabel *FOwnLabel;
	QLabel *FStatusLabel;
	QListWidget *FDevicesList;
	QPushButton *FTrustButton;
	QPushButton *FDistrustButton;
};

#endif // OMEMODEVICESDIALOG_H
