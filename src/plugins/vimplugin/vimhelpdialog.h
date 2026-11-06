#ifndef VIMHELPDIALOG_H
#define VIMHELPDIALOG_H

#include <QDialog>
#include <QString>

class QKeyEvent;
class QTextBrowser;

// The ":help" window: a modeless read-only page with the modes, the current
// key bindings and the COMMAND list.  It is navigated like a vim pager
// (j/k, Ctrl-D/Ctrl-U, Ctrl-F/Ctrl-B, gg/G) and closes with q or Escape.
class VimHelpDialog :
	public QDialog
{
	Q_OBJECT;
public:
	explicit VimHelpDialog(QWidget *AParent = NULL);
	void setHtml(const QString &AHtml, const QString &AAnchor = QString());
	void focusBrowser();
protected:
	bool eventFilter(QObject *AWatched, QEvent *AEvent) override;
	void keyPressEvent(QKeyEvent *AEvent) override;
private:
	bool handleKey(QKeyEvent *AEvent);
	void scrollBy(int APixels);
	int lineHeight() const;
	int viewportHeight() const;
private:
	QTextBrowser *FBrowser;
	// pending "g" of the "gg" gesture
	bool FGg = false;
};

#endif // VIMHELPDIALOG_H
