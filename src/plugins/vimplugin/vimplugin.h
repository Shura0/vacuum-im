#ifndef VIMPLUGIN_H
#define VIMPLUGIN_H

#include <QElapsedTimer>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include <interfaces/ipluginmanager.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/imainwindow.h>
#include <interfaces/irostersview.h>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/ipresencemanager.h>
#include <interfaces/imultiuserchat.h>

#define VIMPLUGIN_UUID  "{A0593123-5391-4ae7-96fd-6442b2916263}"
// duplicated here on purpose: the shortcutmanager header is not shared across
// plugin build units, and the conflict must point to the same plugin identity
#define SHORTCUTMANAGER_UUID "{3F6D20F1-401D-4832-92C3-DB6687891EFD}"

// style handler order for the message bounds markers, leaves a free slot
// between the known handlers (see definitions/messagestylehandlerorders.h)
#define MSHO_VIM 300

// plugin-local option keys
#define OPN_VIM                         "Vim"
#define ONO_VIM                         760
#define OHO_VIM                         50
#define OWO_VIM                         100
#define OPV_VIM_ENABLE                  QString("vim.enable")
#define OPV_VIM_COUNTS                  QString("vim.counts")
#define OPV_VIM_KEYS                    QString("vim.keys[%1]")

enum VimMode
{
	NormalMode,
	InsertMode,
	VisualMode,
	CommandMode,
	SearchPromptMode
};

enum VimActionId
{
	ActionScrollDown = 0,
	ActionScrollUp,
	ActionScrollMessageUp,
	ActionScrollMessageDown,
	ActionSearchForward,
	ActionFocusRoster,
	ActionGotoFirstLine,
	ActionGotoLastLine,
	ActionCopyMessage,
	ActionCopyBlock,
	ActionEnterVisual,
	ActionEnterInsert,
	ActionEditLastMessage,
	ActionEnterCommand,
	ActionNextSearchMatch,
	ActionPrevSearchMatch,
	ActionVisualCopy,
	ActionPasteClipboard,
	ActionOpenNotification,
	Action__Count
};

class VimPlugin :
	public QObject,
	public IPlugin,
	public IOptionsDialogHolder,
	public IMessageStyleHandler
{
	Q_OBJECT;
	Q_INTERFACES(IPlugin IOptionsDialogHolder IMessageStyleHandler);
	Q_PLUGIN_METADATA(IID "org.vacuum-im.plugins.VimPlugin");
public:
	VimPlugin();
	~VimPlugin();
public:
	// IPlugin
	virtual QObject *instance() { return this; }
	virtual QUuid pluginUuid() const { return QUuid(VIMPLUGIN_UUID); }
	virtual void pluginInfo(IPluginInfo *APluginInfo);
	virtual bool initConnections(IPluginManager *APluginManager, int &AInitOrder);
	virtual bool initObjects();
	virtual bool initSettings();
	virtual bool startPlugin();
	// IOptionsDialogHolder
	virtual QMultiMap<int, IOptionsDialogWidget *> optionsDialogWidgets(const QString &ANodeId, QWidget *AParent);
	// IMessageStyleHandler
	virtual bool messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions);
	virtual QString messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions);
	// options widget API
	static QString actionTitle(VimActionId AAction);
	static QString actionKeysString(VimMode AMode, VimActionId AAction);
	static void saveActionKeysString(VimMode AMode, VimActionId AAction, const QString &AKeys);
	// global hotkeys (they are shared with mainwindow/notifications, the
	// shortcut manager that usually configures them conflicts with this plugin)
	static QKeySequence globalShortcutKey(const QString &AId);
	static void saveGlobalShortcut(const QString &AId, const QKeySequence &AKey);
protected:
	bool eventFilter(QObject *AWatched, QEvent *AEvent) override;
private slots:
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onChatWindowDestroyed(IMessageChatWindow *AWindow);
	void onOptionsOpened();
	void onOptionsClosed();
	void onWindowContentAppended();
	void onWindowStyleChanged();
private:
	void loadKeymap();
	void loadGlobalShortcuts();
	QList< QList<QKeyCombination> > actionKeys(VimMode AMode, VimActionId AAction) const;
	QList< QList<QKeyCombination> > parseKeyString(const QString &AKeys) const;
	int resolveAction(VimMode AMode, const QList<QKeyCombination> &AKeys) const;
	bool actionHasPartialMatch(VimMode AMode, const QList<QKeyCombination> &AKeys) const;
	// the double-Escape gesture: two presses within a second close the
	// main window (the vim modes swallow the single Escape everywhere)
	bool registerEscapeKey();
	bool inMainWindowContext(QWidget *AFocusWidget) const;
	bool processKey(QKeyEvent *AEvent);
	bool editorEventFilter(QWidget *editorWidget, QEvent *event);
	bool processViewerKey(QKeyEvent *AEvent);
	bool processRosterKey(QKeyEvent *AEvent);
	bool processEditorKey(QKeyEvent *AEvent);
	// vim filter on the chat editors: installed AFTER the EditWidget own
	// filter, runs BEFORE it and before the widget shortcuts activation -
	// the only reliable point to block Enter ("send") and Escape ("close")
	void installEditorFilter(IMessageChatWindow *AWindow);
	void placeCursorAtLastMessage(IMessageChatWindow *AWindow);
	bool executeAction(VimMode AMode, VimActionId AAction, int ACount);
	// actions
	void doScroll(bool AVisual, int ABlocks);
	void doGotoEdge(bool AFirst, bool AVisual);
	void doSearchPrompt();
	void doSearchNext(bool ABackward);
	void doCopyBlock(IMessageChatWindow *AWindow, bool AMessage);
	void doEnterInsert(bool AEditLast);
	void doPasteClipboard();
	void doEnterCommand();
	void executeCommand();
	void applyPresenceShow(int AShow, const QString &AStatusText);
	// helpers
	IMessageChatWindow *contextWindow(QWidget *AFocusWidget) const;
	IMessageChatWindow *activeChatWindow() const;
	QTextBrowser *chatViewer(IMessageChatWindow *AWindow) const;
	IMessageEditWidget *chatEditor(IMessageChatWindow *AWindow) const;
	void closeMainWindow();
	void setApplicationMode(VimMode AMode);
	void applyModeToWindow(IMessageChatWindow *AWindow);
	void updateCursor(IMessageChatWindow *AWindow, bool AEnsureVisible=false);
	void focusModeWidget(IMessageChatWindow *AWindow);
	QList< QPair<int,int> > messageMarkerRanges(QTextDocument *ADocument) const;
private:
	IPluginManager     *FPluginManager = nullptr;
	IMessageWidgets    *FMessageWidgets = nullptr;
	IMainWindowPlugin  *FMainWindowPlugin = nullptr;
	IRostersViewPlugin *FRostersViewPlugin = nullptr;
	IOptionsManager    *FOptionsManager = nullptr;
	IPresenceManager   *FPresenceManager = nullptr;

	VimMode FMode = NormalMode;
	QString FSearchString;
	int FCount = -1;
	QList<QKeyCombination> FKeyBuffer;
	// timestamps of the Escape presses, two within a second close the window
	QElapsedTimer FEscapeElapsed;
	// set while a synthesized cursor key is delivered to the viewer, so the
	// application event filter lets it through to the native text browser
	bool FForwardingKey = false;
	QHash<VimActionId, QList< QList<QKeyCombination> > > FKeymap;
	// the key strings are re-read on every key press, the parsed variants
	// are cached to keep the matching cheap
	mutable QHash<QString, QList< QList<QKeyCombination> > > FParseCache;
	QList<QPointer<QWidget> > FEditorWidgets;
	// keys whose press was consumed by vim, their releases are swallowed too
	QSet<int> FConsumedKeys;
};

#endif // VIMPLUGIN_H
