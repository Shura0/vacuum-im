#ifndef CALLSOPTIONSWIDGET_H
#define CALLSOPTIONSWIDGET_H

#include <QWidget>
#include <QAudioDevice>

#include <interfaces/ioptionsmanager.h>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLineEdit;

/*
 * The "Calls" options page: the two session toggles plus the NAT traversal
 * settings (STUN/TURN servers, XEP-0215 discovery, the relay-only test mode).
 * The values are read and written through Options::node(), the same store the
 * plugin consults when it starts a call.
 */
class CallsOptionsWidget :
	public QWidget,
	public IOptionsDialogWidget
{
	Q_OBJECT;
	Q_INTERFACES(IOptionsDialogWidget);
public:
	explicit CallsOptionsWidget(QWidget *AParent = nullptr);
	virtual QWidget *instance() { return this; }
public slots:
	virtual void apply();
	virtual void reset();
signals:
	void modified();
	void childApply();
	void childReset();
private:
	static void fillDevices(QComboBox *ACombo, const QList<QAudioDevice> &ADevices);
	void updateEnabledState(bool AEnabled);
private:
	QCheckBox *FEnabled;
	QGroupBox *FDeviceBox;
	QGroupBox *FNatBox;
	QCheckBox *FVideo;
	QComboBox *FQuality;
	QCheckBox *FAec;
	QCheckBox *FAllowUnknown;
	QComboBox *FInputDevice;
	QComboBox *FOutputDevice;
	QCheckBox *FUseDiscovery;
	QCheckBox *FForceRelay;
	QLineEdit *FStunServer;
	QLineEdit *FTurnServer;
	QLineEdit *FTurnUsername;
	QLineEdit *FTurnPassword;
	bool FAecAvailable;
};

#endif // CALLSOPTIONSWIDGET_H
