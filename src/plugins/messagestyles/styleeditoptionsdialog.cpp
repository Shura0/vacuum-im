#include "styleeditoptionsdialog.h"

#include <QTimer>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextOption>
#include <QVBoxLayout>
#include <definitions/optionvalues.h>
#include <definitions/messagemarks.h>
#include <utils/widgetmanager.h>
#include <utils/message.h>
#include <utils/options.h>
#include <utils/logger.h>

StyleEditOptionsDialog::StyleEditOptionsDialog(IMessageStyleManager *AMessageStyles, const OptionsNode &AStyleNode, QWidget *AParent) : QDialog(AParent)
{
	ui.setupUi(this);
	setWindowModality(Qt::WindowModal);
	setAttribute(Qt::WA_DeleteOnClose,true);
	ui.lblPreview->setText(QString("<h2>%1</h2>").arg(tr("Preview")));

	FUpdateStarted = false;
	FMessageStyleManager = AMessageStyles;

	QList<QString> params = AStyleNode.parentNSpaces();
	FMessageType = params.value(1).toInt();
	FContext = params.value(2);
	FEngineId = params.value(3);
	FStyleId = params.value(4);

	FStyleEngine = FMessageStyleManager->findStyleEngine(FEngineId);
	FStyleSettings = FStyleEngine!=NULL ? FStyleEngine->styleSettingsWidget(AStyleNode,this) : NULL;
	
	IMessageStyleOptions soptions = FStyleSettings!=NULL ? FStyleEngine->styleSettinsOptions(FStyleSettings) : IMessageStyleOptions();
	FStyle = FStyleSettings!=NULL ? FStyleEngine->styleForOptions(soptions) : NULL;
	FStyleView = FStyle!=NULL ? FStyle->createWidget(soptions,this) : NULL;

	if (FStyleEngine && FStyleSettings && FStyle && FStyleView)
	{
		setWindowTitle(tr("Message Style - %1 - %2").arg(FStyleEngine->engineName(),soptions.styleId));

		ui.wdtStyleSettings->setLayout(new QVBoxLayout);
		ui.wdtStyleSettings->layout()->setContentsMargins(0,0,0,0);
		ui.wdtStyleSettings->layout()->addWidget(FStyleSettings->instance());
		connect(FStyleSettings->instance(),SIGNAL(modified()),SLOT(startStyleViewUpdate()));

		ui.frmStyleView->setLayout(new QVBoxLayout);
		ui.frmStyleView->layout()->setContentsMargins(0,0,0,0);
		ui.frmStyleView->layout()->addWidget(FStyleView);
		FStyleView->installEventFilter(this);
	}

	connect(ui.dbbButtons,SIGNAL(accepted()),SLOT(accept()));
	connect(ui.dbbButtons,SIGNAL(rejected()),SLOT(reject()));

	if (!restoreGeometry(Options::fileValue("message-styles.style-edit-options-dialog.geometry").toByteArray()))
		setGeometry(WidgetManager::alignGeometry(QSize(550,500),this));

	startStyleViewUpdate();
}

StyleEditOptionsDialog::~StyleEditOptionsDialog()
{
	Options::setFileValue(saveGeometry(),"message-styles.style-edit-options-dialog.geometry");
}

void StyleEditOptionsDialog::accept()
{
	if (FStyleSettings)
		FStyleSettings->apply();
	QDialog::accept();
}

void StyleEditOptionsDialog::startStyleViewUpdate()
{
	if (!FUpdateStarted)
	{
		FUpdateStarted = true;
		QTimer::singleShot(0,this,SLOT(onUpdateStyleView()));
	}
}

void StyleEditOptionsDialog::createViewContent()
{
	IMessageStyleContentOptions i_options;
	IMessageStyleContentOptions o_options;

	i_options.noScroll = true;
	i_options.kind = IMessageStyleContentOptions::KindMessage;
	i_options.direction = IMessageStyleContentOptions::DirectionIn;
	i_options.senderId = "contact@example.com";
	i_options.senderName = tr("Contact");
	i_options.senderIcon = FMessageStyleManager->contactIcon(i_options.senderId,IPresence::Chat,SUBSCRIPTION_BOTH,false);

	o_options.noScroll = true;
	o_options.kind = IMessageStyleContentOptions::KindMessage;
	o_options.direction = IMessageStyleContentOptions::DirectionOut;
	o_options.senderId = "you@example.com";
	o_options.senderName = tr("You");
	o_options.senderIcon = FMessageStyleManager->contactIcon(i_options.senderId,IPresence::Online,SUBSCRIPTION_BOTH,false);

	if (FMessageType==Message::Normal || FMessageType==Message::Headline || FMessageType==Message::Error)
	{
		i_options.time = QDateTime::currentDateTime().addDays(-1);
		i_options.timeFormat = FMessageStyleManager->timeFormat(i_options.time);

		if (FMessageType == Message::Error)
		{
			i_options.senderName.clear();
			i_options.kind = IMessageStyleContentOptions::KindMessage;
			QString html = "<b>"+tr("The message with a error code %1 is received").arg(999)+"</b>";
			html += "<p style='color:red;'>"+tr("Error description")+"</p>";
			html += "<hr>";
			FStyle->appendContent(FStyleView,html,i_options);
		}

		i_options.kind = IMessageStyleContentOptions::KindTopic;
		FStyle->appendContent(FStyleView,tr("Subject: Message subject"),i_options);

		i_options.kind = IMessageStyleContentOptions::KindMessage;
		FStyle->appendContent(FStyleView,tr("Message body line 1")+"<br>"+tr("Message body line 2"),i_options);
	}
	else if (FMessageType==Message::Chat || FMessageType==Message::GroupChat)
	{
		if (FMessageType==Message::GroupChat)
		{
			i_options.senderColor = FStyle->senderColorById(i_options.senderName);
			o_options.senderColor = FStyle->senderColorById(o_options.senderName);
		}
		else
		{
			i_options.senderColor.clear();
			o_options.senderColor.clear();
		}

		i_options.time = QDateTime::currentDateTime().addDays(-1);
		i_options.type = IMessageStyleContentOptions::TypeHistory;
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
		{
			i_options.timeFormat = " ";
			i_options.kind = IMessageStyleContentOptions::KindStatus;
			i_options.status = IMessageStyleContentOptions::StatusDateSeparator;
			FStyle->appendContent(FStyleView,FMessageStyleManager->dateSeparator(i_options.time.date()),i_options);
		}

		i_options.kind = IMessageStyleContentOptions::KindMessage;
		i_options.status = IMessageStyleContentOptions::StatusEmpty;
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
			i_options.timeFormat = FMessageStyleManager->timeFormat(i_options.time,i_options.time);
		else
			i_options.timeFormat = FMessageStyleManager->timeFormat(i_options.time);
		FStyle->appendContent(FStyleView,tr("Incoming history message"),i_options);

		i_options.time = QDateTime::currentDateTime().addDays(-1).addSecs(80);
		FStyle->appendContent(FStyleView,tr("Incoming history consecutive message"),i_options);

		i_options.kind = IMessageStyleContentOptions::KindStatus;
		FStyle->appendContent(FStyleView,tr("Incoming status message"),i_options);

		o_options.type = IMessageStyleContentOptions::TypeHistory;
		o_options.time = QDateTime::currentDateTime().addDays(-1).addSecs(100);
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
			o_options.timeFormat = FMessageStyleManager->timeFormat(o_options.time,o_options.time);
		else
			o_options.timeFormat = FMessageStyleManager->timeFormat(o_options.time);
		FStyle->appendContent(FStyleView,tr("Outgoing history message"),o_options);

		o_options.kind = IMessageStyleContentOptions::KindStatus;
		FStyle->appendContent(FStyleView,tr("Outgoing status message"),o_options);

		i_options.type = 0;
		i_options.time = QDateTime::currentDateTime();
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
		{
			i_options.timeFormat = " ";
			i_options.kind = IMessageStyleContentOptions::KindStatus;
			i_options.status = IMessageStyleContentOptions::StatusDateSeparator;
			FStyle->appendContent(FStyleView,FMessageStyleManager->dateSeparator(i_options.time.date()),i_options);
		}

		i_options.status = IMessageStyleContentOptions::StatusEmpty;
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
			i_options.timeFormat = FMessageStyleManager->timeFormat(i_options.time,i_options.time);
		else
			i_options.timeFormat = FMessageStyleManager->timeFormat(i_options.time);
		if (FMessageType == Message::GroupChat)
		{
			i_options.kind = IMessageStyleContentOptions::KindTopic;
			FStyle->appendContent(FStyleView,tr("Conference topic"),i_options);
		}

		i_options.kind = IMessageStyleContentOptions::KindMessage;
		FStyle->appendContent(FStyleView,tr("Incoming message"),i_options);

		i_options.kind = IMessageStyleContentOptions::KindMeCommand;
		FStyle->appendContent(FStyleView,tr("speaks of himself in the third person"),i_options);

		i_options.type = IMessageStyleContentOptions::TypeEvent;
		i_options.kind = IMessageStyleContentOptions::KindStatus;
		FStyle->appendContent(FStyleView,tr("Incoming event"),i_options);

		i_options.type = IMessageStyleContentOptions::TypeNotification;
		FStyle->appendContent(FStyleView,tr("Incoming notification"),i_options);

		if (FMessageType == Message::GroupChat)
		{
			i_options.kind = IMessageStyleContentOptions::KindMessage;
			i_options.type = IMessageStyleContentOptions::TypeMention;
			FStyle->appendContent(FStyleView,tr("Incoming mention message"),i_options);
		}

		o_options.type = 0;
		o_options.time = QDateTime::currentDateTime();
		if (Options::node(OPV_MESSAGES_SHOWDATESEPARATORS).value().toBool())
			o_options.timeFormat = FMessageStyleManager->timeFormat(o_options.time,o_options.time);
		else
			o_options.timeFormat = FMessageStyleManager->timeFormat(o_options.time);
		o_options.kind = IMessageStyleContentOptions::KindMessage;
		FStyle->appendContent(FStyleView,tr("Outgoing message"),o_options);

		o_options.time = QDateTime::currentDateTime().addSecs(5);
		FStyle->appendContent(FStyleView,tr("Outgoing consecutive message"),o_options);

		// A reply with the quoted lines, marked by the style with '> ' prefixes
		o_options.time = QDateTime::currentDateTime().addSecs(10);
		QString quoteHtml = tr("Reply where a part is quoted")+"<br>";
		quoteHtml += "&gt; "+tr("Quoted message line 1")+"<br>";
		quoteHtml += "&gt; "+tr("Quoted message line 2");
		FStyle->appendContent(FStyleView,quoteHtml,o_options);

		o_options.kind = IMessageStyleContentOptions::KindMeCommand;
		FStyle->appendContent(FStyleView,tr("speaks of himself in the third person"),o_options);

		o_options.kind = IMessageStyleContentOptions::KindMessage;
		// Delivery/read marks are normally placed by the style handlers
		// through the reserved 'marks' template slot; in the standalone
		// preview they are embedded right after the message body so the
		// variant CSS classes can be evaluated.
		o_options.time = QDateTime::currentDateTime().addSecs(15);
		FStyle->appendContent(FStyleView,tr("Outgoing message waiting to be delivered")
			+ QString("&nbsp;<span class=\"%1\">&#10003;</span>").arg(MSMC_MARK_PENDING),o_options);

		o_options.time = QDateTime::currentDateTime().addSecs(20);
		FStyle->appendContent(FStyleView,tr("Outgoing message delivered to the contact")
			+ QString("&nbsp;<span class=\"%1\">&#10004;</span>").arg(MSMC_MARK_DELIVERED),o_options);

		o_options.time = QDateTime::currentDateTime().addSecs(25);
		FStyle->appendContent(FStyleView,tr("Outgoing message read by the contact")
			+ QString("&nbsp;<span class=\"%1\">&#10004;</span><span class=\"%2\">&#10004;</span>")
				.arg(MSMC_MARK_DELIVERED,MSMC_MARK_DISPLAYED),o_options);
	}
}

void StyleEditOptionsDialog::onUpdateStyleView()
{
	if (FStyleEngine && FStyleSettings && FStyle && FStyleView)
	{
		FStyle->changeOptions(FStyleView,FStyleEngine->styleSettinsOptions(FStyleSettings),true);
		createViewContent();
		// The mark glyphs are aligned by the tab stops, which can only be
		// applied after the content has landed in the style document.
		QTimer::singleShot(0,this,SLOT(alignMarkGlyphs()));
	}
	FUpdateStarted = false;
}

bool StyleEditOptionsDialog::eventFilter(QObject *AWatched,QEvent *AEvent)
{
	if (AWatched==FStyleView && AEvent->type()==QEvent::Resize)
		QTimer::singleShot(0,this,SLOT(alignMarkGlyphs()));
	return QDialog::eventFilter(AWatched,AEvent);
}

void StyleEditOptionsDialog::alignMarkGlyphs()
{
	// Couples the marks appended by createViewContent to the right border of
	// the view, exactly like MessageReceipts::applyReceiptMark does for the
	// real chat view: a tab character before the glyph and a right tab stop
	// set on the message block.
	QTextBrowser *browser = qobject_cast<QTextBrowser *>(FStyleView);
	QTextDocument *document = browser!=NULL ? browser->document() : NULL;
	if (document==NULL)
		return;

	qreal usableWidth = browser->viewport()->width() - 2*document->documentMargin();
	QFontMetricsF metrics(browser->font());
	foreach (const QChar &glyph, QList<QChar>()<<QChar(0x2713)<<QChar(0x2714))
	{
		QTextCursor cursor(document);
		while (true)
		{
			cursor = document->find(QString(glyph),cursor);
			if (cursor.isNull() || cursor.selectionStart()==0)
				break;
			int glyphPos = cursor.selectionStart();
			QTextBlock glyphBlock = document->findBlock(glyphPos);
			QString blockText = glyphBlock.text();
			QChar prevChar = blockText.mid(glyphPos-1-glyphBlock.position(),1).at(0);
			if (blockText.isEmpty())
				break;
			if (prevChar!=QChar(0x0009) && prevChar!=QChar(0x2713) && prevChar!=QChar(0x2714))
			{
				QTextCursor tabCursor(document);
				tabCursor.setPosition(glyphPos);
				tabCursor.insertText(QChar(0x0009));
			}

			QString glyphText = glyph;
			qreal markerWidth = metrics.horizontalAdvance(glyphText) + 8;
			if (usableWidth <= markerWidth)
				break;

			QTextCursor blockCursor(document);
			blockCursor.setPosition(glyphPos);
			QTextBlockFormat blockFormat = blockCursor.block().blockFormat();
			QList<QTextOption::Tab> tabs = blockFormat.tabPositions();
			int rightTab = -1;
			for (int i=0; i<tabs.count(); i++)
			{
				if (tabs.at(i).type == QTextOption::RightTab)
				{
					rightTab = i;
					break;
				}
			}
			QTextOption::Tab tab;
			tab.type = QTextOption::RightTab;
			tab.position = usableWidth - markerWidth;
			if (rightTab>=0)
				tabs[rightTab] = tab;
			else
				tabs.append(tab);
			blockFormat.setTabPositions(tabs);
			blockCursor.setBlockFormat(blockFormat);

			// Continue the search after the aligned glyph; a repeated glyph of
			// the other kind placed right after it is pushed to the border too.
			cursor = QTextCursor(document);
			cursor.setPosition(glyphPos+1);
		}
	}
}
