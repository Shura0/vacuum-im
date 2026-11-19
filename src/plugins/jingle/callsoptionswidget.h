#ifndef CALLSOPTIONSWIDGET_H
#define CALLSOPTIONSWIDGET_H

#include <QWidget>

#include <interfaces/ioptionsmanager.h>

class QCheckBox;
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
	QCheckBox *FEnabled;
	QCheckBox *FAec;
	QCheckBox *FAllowUnknown;
	QCheckBox *FUseDiscovery;
	QCheckBox *FForceRelay;
	QLineEdit *FStunServer;
	QLineEdit *FTurnServer;
	QLineEdit *FTurnUsername;
	QLineEdit *FTurnPassword;
};

#endif // CALLSOPTIONSWIDGET_H
