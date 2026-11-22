#ifndef OMEMODEVICESDIALOG_H
#define OMEMODEVICESDIALOG_H

#include <QDialog>
#include <QPointer>

#include <utils/jid.h>

class OmemoAccount;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;

// Device and trust management for one contact and for the account itself.
// The contact list can be trusted/blocked; both lists allow a single session
// reset, and own devices can be removed from the published PEP device list.
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
	void onStrictToggled(bool AChecked);
	void onResetSessionClicked();
	void onRemoveDeviceClicked();
	void onRefreshClicked();
	void onSelectionChanged();
	void onResetIdentityClicked();

private:
	OmemoAccount *FAccount;
	Jid FStreamJid;
	Jid FContactJid;
	QCheckBox *FStrictBox;
	QLabel *FStatusLabel;
	QLabel *FFingerprintLabel;
	QListWidget *FDevicesList;
	QListWidget *FOwnDevicesList;
	QPushButton *FTrustButton;
	QPushButton *FDistrustButton;
	QPushButton *FResetIdentityButton;
	QPushButton *FResetButton;
	QPushButton *FRemoveButton;
	// Which list the fingerprint label currently follows
	QPointer<QListWidget> FLastSelectionList;
};

#endif // OMEMODEVICESDIALOG_H
