#ifndef VIMOPTIONSWIDGET_H
#define VIMOPTIONSWIDGET_H

#include <QCheckBox>
#include <QWidget>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/imessagewidgets.h>

class QAbstractButton;
class QKeySequenceEdit;
class QTableWidget;

class VimOptionsWidget :
	public QWidget,
	public IOptionsDialogWidget
{
	Q_OBJECT;
	Q_INTERFACES(IOptionsDialogWidget);
public:
	explicit VimOptionsWidget(QWidget *AParent);
	virtual QWidget *instance() { return this; }
public slots:
	virtual void apply();
	virtual void reset();
signals:
	void modified();
	void childApply();
	void childReset();
protected:
	void widgetModified();
	void createRow(int AAction);
private:
	int keyRow(int AAction) const;
	bool hasVisualColumn(int AAction) const;
private:
	QCheckBox *FEnable;
	QCheckBox *FCounts;
	QKeySequenceEdit *FShowRosterKey;
	QKeySequenceEdit *FShowMessageKey;
	QTableWidget *FKeyTable;
};

#endif // VIMOPTIONSWIDGET_H
