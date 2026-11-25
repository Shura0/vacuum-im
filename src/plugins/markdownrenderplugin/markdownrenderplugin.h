#ifndef MARKDOWNRENDERPLUGIN_H
#define MARKDOWNRENDERPLUGIN_H

#include <QObject>
#include <QUuid>
#include <QRegularExpression>
#include <QTextCharFormat>
#include <interfaces/ipluginmanager.h>
#include <interfaces/imessageprocessor.h>

#define MARKDOWNRENDERPLUGIN_UUID "D02B339A-EE7B-471C-A6B4-4E492F6BC1A9"

class MarkdownRender :
	public QObject,
	public IPlugin,
	public IMessageWriter
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IMessageWriter);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.MarkdownRender");
public:
	MarkdownRender();
	virtual ~MarkdownRender();
	// IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(MARKDOWNRENDERPLUGIN_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings() { return true; }
	virtual bool startPlugin() { return true; }
	// IMessageWriter
	virtual bool writeMessageHasText(int AOrder, Message &AMessage, const QString &ALang);
	virtual bool writeMessageToText(int AOrder, Message &AMessage, QTextDocument *ADocument, const QString &ALang);
	virtual bool writeTextToMessage(int AOrder, QTextDocument *ADocument, Message &AMessage, const QString &ALang);
protected:
	bool linkifyMarkdownLinks(QTextDocument *ADocument) const;
	bool applyInlineFormat(QTextDocument *ADocument, const QRegularExpression &ARegexp, const QTextCharFormat &AFormat) const;
private:
	IMessageProcessor *FMessageProcessor;
};

#endif // MARKDOWNRENDERPLUGIN_H
