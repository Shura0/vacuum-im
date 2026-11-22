#ifndef OMEMOOPTIONSWIDGET_H
#define OMEMOOPTIONSWIDGET_H

#include <QWidget>

#include <interfaces/ioptionsmanager.h>

class QComboBox;
class QLineEdit;
class OmemoAccount;
class Omemo;

/*
 * Per-account OMEMO page.  OMEMO is always enabled, so the page only offers
 * the device name, the global trust policy and the session maintenance
 * action.  The device name lives in the encrypted store next to the identity.
 */
class OmemoOptionsWidget :
	public QWidget,
	public IOptionsDialogWidget
{
	Q_OBJECT
	Q_INTERFACES(IOptionsDialogWidget)
public:
	OmemoOptionsWidget(Omemo *APlugin, OmemoAccount *AAccount,
		IOptionsManager *AOptionsManager, QWidget *AParent = nullptr);
	virtual QWidget *instance() { return this; }
public slots:
	virtual void apply();
	virtual void reset();
signals:
	void modified();
	void childApply();
	void childReset();
private slots:
	void onClearSessionsClicked();
private:
	Omemo *FPlugin;
	OmemoAccount *FAccount;
	QComboBox *FPolicyCombo;
	QLineEdit *FLabel;
};

#endif // OMEMOOPTIONSWIDGET_H
