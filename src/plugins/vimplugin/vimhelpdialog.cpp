#include "vimhelpdialog.h"

#include "vimplugin.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>

VimHelpDialog::VimHelpDialog(QWidget *AParent) :
	QDialog(AParent)
{
	setWindowTitle(tr("Vim mode help"));
	setWindowFlags(windowFlags() | Qt::Tool);
	resize(820, 620);
	setMinimumSize(420, 300);

	FBrowser = new QTextBrowser(this);
	FBrowser->setOpenLinks(false);
	FBrowser->setOpenExternalLinks(false);
	FBrowser->installEventFilter(this);
	// the scroll area may take the keyboard focus on its viewport
	FBrowser->viewport()->installEventFilter(this);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(FBrowser);
}

void VimHelpDialog::setHtml(const QString &AHtml, const QString &AAnchor)
{
	FGg = false;
	FBrowser->setHtml(AHtml);
	if (!AAnchor.isEmpty())
		FBrowser->scrollToAnchor(AAnchor);
	else
		FBrowser->verticalScrollBar()->setValue(0);
}

void VimHelpDialog::focusBrowser()
{
	FBrowser->setFocus();
}

// the read-only text browser consumes every printable key itself, so the
// pager keys have to be taken before it sees them
bool VimHelpDialog::eventFilter(QObject *AWatched, QEvent *AEvent)
{
	if ((AWatched==FBrowser || AWatched==FBrowser->viewport()) && AEvent->type()==QEvent::KeyPress
		&& handleKey(static_cast<QKeyEvent *>(AEvent)))
	{
		AEvent->accept();
		return true;
	}
	return QDialog::eventFilter(AWatched, AEvent);
}

void VimHelpDialog::keyPressEvent(QKeyEvent *AEvent)
{
	// the focus is normally on the browser (handled by the filter), but the
	// Escape must close the window wherever it is pressed inside it
	if (AEvent->key()==Qt::Key_Escape)
		close();
	else
		QDialog::keyPressEvent(AEvent);
}

bool VimHelpDialog::handleKey(QKeyEvent *AEvent)
{
	if (AEvent->key()==Qt::Key_Escape)
	{
		close();
		return true;
	}

	const Qt::KeyboardModifiers modifiers = AEvent->modifiers() & ~Qt::KeypadModifier;
	if (modifiers==Qt::ControlModifier)
	{
		FGg = false;
		const int page = qMax(lineHeight(), viewportHeight()-lineHeight());
		switch (AEvent->key())
		{
		case Qt::Key_D: scrollBy(viewportHeight()/2); return true;
		case Qt::Key_U: scrollBy(-viewportHeight()/2); return true;
		case Qt::Key_F: scrollBy(page); return true;
		case Qt::Key_B: scrollBy(-page); return true;
		default: return false;
		}
	}
	if (modifiers!=Qt::NoModifier)
		return false;

	// the same layout independent characters as the chat windows: a Cyrillic
	// or Greek layout still navigates the help page by the physical keys
	const QChar character = VimPlugin::keyChar(AEvent);
	if (character==QLatin1Char('g'))
	{
		if (FGg)
		{
			FGg = false;
			scrollBy(-FBrowser->verticalScrollBar()->value());
			return true;
		}
		FGg = true;
		return true;
	}

	FGg = false;
	if (character==QLatin1Char('G'))
		scrollBy(FBrowser->verticalScrollBar()->maximum());
	else if (character==QLatin1Char('j'))
		scrollBy(lineHeight());
	else if (character==QLatin1Char('k'))
		scrollBy(-lineHeight());
	else if (character==QLatin1Char('q'))
	{
		close();
		return true;
	}
	else
		return false;
	return true;
}

void VimHelpDialog::scrollBy(int APixels)
{
	QScrollBar *bar = FBrowser->verticalScrollBar();
	bar->setValue(bar->value() + APixels);
}

int VimHelpDialog::lineHeight() const
{
	return QFontMetrics(FBrowser->font()).lineSpacing();
}

int VimHelpDialog::viewportHeight() const
{
	return FBrowser->verticalScrollBar()->pageStep();
}
