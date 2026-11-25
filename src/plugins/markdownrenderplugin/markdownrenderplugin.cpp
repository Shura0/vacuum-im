#include "markdownrenderplugin.h"

#include <QTextCursor>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <QFont>
#include <definitions/messagewriterorders.h>

MarkdownRender::MarkdownRender()
{
	FMessageProcessor = nullptr;
}

MarkdownRender::~MarkdownRender()
{
	if (FMessageProcessor)
		FMessageProcessor->removeMessageWriter(MWO_MARKDOWNRENDER,this);
}

void MarkdownRender::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Markdown Render");
	APluginInfo->description = tr("Renders Markdown-style links as clickable links and _italic_ / *bold* text in chat messages");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->homePage = "http://www.vacuum-im.org";
	APluginInfo->dependences.append(QUuid(MESSAGEPROCESSOR_UUID));
}

bool MarkdownRender::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	IPlugin *plugin = APluginManager->pluginInterface("IMessageProcessor").value(0,nullptr);
	if (plugin)
		FMessageProcessor = qobject_cast<IMessageProcessor *>(plugin->instance());

	return FMessageProcessor!=nullptr;
}

bool MarkdownRender::initObjects()
{
	FMessageProcessor->insertMessageWriter(MWO_MARKDOWNRENDER,this);
	return true;
}

bool MarkdownRender::writeMessageHasText(int AOrder, Message &AMessage, const QString &ALang)
{
	Q_UNUSED(AOrder); Q_UNUSED(AMessage); Q_UNUSED(ALang);
	return false;
}

bool MarkdownRender::writeMessageToText(int AOrder, Message &AMessage, QTextDocument *ADocument, const QString &ALang)
{
	Q_UNUSED(AMessage); Q_UNUSED(ALang);
	if (AOrder != MWO_MARKDOWNRENDER)
		return false;

	// Links first: their replacement text gets the link character format, so
	// formatting them here would be lost. Inline formats are merged, not set,
	// so they keep the anchor and each other on overlapping ranges.
	bool changed = linkifyMarkdownLinks(ADocument);

	static const QRegularExpression italic("_(?!\\s)([^_\\r\\n\\x{2029}]*[^_\\s\\r\\n\\x{2029}])_");
	static const QRegularExpression bold("\\*(?!\\s)([^\\*\\r\\n\\x{2029}]*[^\\*\\s\\r\\n\\x{2029}])\\*");

	QTextCharFormat italicFormat;
	italicFormat.setFontItalic(true);
	QTextCharFormat boldFormat;
	boldFormat.setFontWeight(QFont::Bold);

	changed = applyInlineFormat(ADocument,italic,italicFormat) || changed;
	changed = applyInlineFormat(ADocument,bold,boldFormat) || changed;
	return changed;
}

bool MarkdownRender::writeTextToMessage(int AOrder, QTextDocument *ADocument, Message &AMessage, const QString &ALang)
{
	Q_UNUSED(AOrder); Q_UNUSED(ADocument); Q_UNUSED(AMessage); Q_UNUSED(ALang);
	return false;
}

bool MarkdownRender::linkifyMarkdownLinks(QTextDocument *ADocument) const
{
	// Markdown-style links in a plain message body: [visible text](https://...).
	// Only well-known, safe-to-open schemes are accepted - the text comes from
	// a remote contact and must not become an arbitrary (e.g. javascript:) link.
	static const QRegularExpression regexp(
		"\\[([^\\]\\r\\n\\x{2029}]+)\\]\\("
		"((?:https?|ftp)://[^\\s)]+|www\\.[^\\s)]+|(?:xmpp|magnet|mailto):[^\\s)]+)\\)",
		QRegularExpression::CaseInsensitiveOption);

	bool changed = false;
	QTextCursor found = ADocument->find(regexp);
	while (!found.isNull())
	{
		const QRegularExpressionMatch match = regexp.match(found.selectedText());
		if (match.hasMatch())
		{
			const QUrl url = QUrl::fromUserInput(match.captured(2));
			if (url.isValid() && !url.isEmpty())
			{
				found.removeSelectedText();
				QTextCharFormat linkFormat = found.charFormat();
				linkFormat.setAnchor(true);
				linkFormat.setAnchorHref(url.toEncoded());
				linkFormat.setToolTip(url.toString());
				found.insertText(match.captured(1),linkFormat);
				changed = true;
			}
		}
		// Continue after the inserted text (or after the unchanged match).
		found = ADocument->find(regexp,found);
	}
	return changed;
}

bool MarkdownRender::applyInlineFormat(QTextDocument *ADocument, const QRegularExpression &ARegexp, const QTextCharFormat &AFormat) const
{
	// Only the captured text between the markers is formatted; the markers
	// themselves stay in the document and remain visible in the chat.
	bool changed = false;
	QTextCursor found = ADocument->find(ARegexp);
	while (!found.isNull())
	{
		const QRegularExpressionMatch match = ARegexp.match(found.selectedText());
		if (match.hasMatch() && match.capturedLength(1) > 0)
		{
			QTextCursor cursor(ADocument);
			cursor.setPosition(found.selectionStart()+match.capturedStart(1));
			cursor.setPosition(found.selectionStart()+match.capturedEnd(1),QTextCursor::KeepAnchor);
			cursor.mergeCharFormat(AFormat);
			changed = true;
		}
		found = ADocument->find(ARegexp,found);
	}
	return changed;
}
