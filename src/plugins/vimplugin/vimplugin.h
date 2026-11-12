#ifndef VIMPLUGIN_H
#define VIMPLUGIN_H

#include <QElapsedTimer>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <interfaces/ipluginmanager.h>
#include <interfaces/imessagewidgets.h>
#include <interfaces/imainwindow.h>
#include <interfaces/irostersview.h>
#include <interfaces/irostermanager.h>
#include <interfaces/ioptionsmanager.h>
#include <interfaces/ipresencemanager.h>
#include <interfaces/imultiuserchat.h>
#include <interfaces/ibookmarks.h>
#include <interfaces/irecentcontacts.h>

#define VIMPLUGIN_UUID  "{A0593123-5391-4ae7-96fd-6442b2916263}"
// duplicated here on purpose: the shortcutmanager header is not shared across
// plugin build units, and the conflict must point to the same plugin identity
#define SHORTCUTMANAGER_UUID "{3F6D20F1-401D-4832-92C3-DB6687891EFD}"

class VimHelpDialog;

// a conference room and the nick it was last joined with: the ":join"
// completion works on a list of these
typedef QPair<QString, QString> VimKnownRoom;

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
	SearchPromptMode,
	CaretMode	// NORMAL with a visible, movable block cursor: a staging
			// mode for VISUAL, entered with 'v' when nothing is selected
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
	ActionMoveLeft,
	ActionMoveRight,
	ActionPasteQuotation,
	ActionReselectVisual,
	ActionSwapSelectionEnds,
	ActionEnterVisualLine,
	ActionCopyNick,
	ActionCopyJid,
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
	// the vim key character of a key event with the layout fallback used by
	// the chat windows (the help window reuses it for its pager keys)
	static QChar keyChar(QKeyEvent *AEvent);
	// global hotkeys (they are shared with mainwindow/notifications, the
	// shortcut manager that usually configures them conflicts with this plugin)
	static QKeySequence globalShortcutKey(const QString &AId);
	static void saveGlobalShortcut(const QString &AId, const QKeySequence &AKey);
protected:
	bool eventFilter(QObject *AWatched, QEvent *AEvent) override;
private slots:
	void onChatWindowCreated(IMessageChatWindow *AWindow);
	void onChatWindowDestroyed(IMessageChatWindow *AWindow);
	// conference (MUC) windows are not IMessageChatWindow, they have their
	// own manager interface and are tracked separately
	void onMultiChatWindowCreated(IMultiUserChatWindow *AWindow);
	void onMultiChatWindowDestroyed(IMultiUserChatWindow *AWindow);
	void onOptionsOpened();
	void onOptionsClosed();
	void onWindowContentAppended(const QString &AHtml, const IMessageStyleContentOptions &AOptions);
	void onWindowStyleChanged();
	void onWindowTabPageActivated();
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
	bool inRosterContext(QWidget *AFocusWidget) const;
	bool inHelpContext(QWidget *AFocusWidget) const;
	bool processKey(QKeyEvent *AEvent);
	QKeyCombination eventCombination(VimMode AMode, QKeyEvent *AEvent) const;
	bool editorEventFilter(QWidget *editorWidget, QEvent *event);
	bool processViewerKey(QKeyEvent *AEvent);
	bool processRosterKey(QKeyEvent *AEvent);
	bool processEditorKey(QKeyEvent *AEvent);
	// vim filter on the chat editors: installed AFTER the EditWidget own
	// filter, runs BEFORE it and before the widget shortcuts activation -
	// the only reliable point to block Enter ("send") and Escape ("close")
	void installEditorFilter(IMessageWindow *AWindow);
	void installWindow(IMessageWindow *AWindow);
	void uninstallWindow(IMessageWindow *AWindow);
	void placeCursorAtLastMessage(IMessageWindow *AWindow);
	bool executeAction(VimMode AMode, VimActionId AAction, int ACount);
	// actions
	void doScroll(bool AVisual, int ABlocks);
	void doGotoEdge(bool AFirst, bool AVisual);
	void doSearchPrompt();
	void doSearchNext(bool ABackward);
	void doCopyBlock(IMessageWindow *AWindow, bool AMessage);
	void doCopyAuthor(IMessageWindow *AWindow, bool AJid);
	void doEnterVisualLine(IMessageWindow *AWindow);
	void doReselectVisual(IMessageWindow *AWindow);
	void rememberVisualSelection();
	void doEnterInsert(bool AEditLast);
	void doPasteClipboard();
	void doPasteQuotation();
	void doEnterCommand();
	void executeCommand();
	// Tab completion of the COMMAND mode name, cycling through the
	// kHelpCommands aliases with Tab/Shift+Tab
	bool completeCommandLine(bool ABackwards);
	QStringList rosterJidCandidates(const QString &APrefix) const;
	// resolves the token under the cursor: the text before it in AHead, the
	// token itself in ATyped and the candidates in ACandidates
	bool commandCompletionCandidates(const QString &ALine, QString &AHead, QString &ATyped, QStringList &ACandidates) const;
	// the stream of the first presence, the one the commands talk to
	Jid commandStreamJid() const;
	// the rooms ":join" knows about with the nick they were last joined with
	QList<VimKnownRoom> knownRooms() const;
	QStringList joinRoomCandidates(const QString &APrefix) const;
	QStringList joinNickCandidates(const QString &ARoom, const QString &APrefix) const;
	QStringList leaveRoomCandidates(const QString &APrefix) const;
	void resetCommandCompletion();
	void showHelp(const QString &ATopic);
	QString buildHelpHtml() const;
	void applyPresenceShow(int AShow, const QString &AStatusText);
	// helpers
	QList<IMessageWindow *> messageWindows() const;
	IMessageWindow *contextWindow(QWidget *AFocusWidget) const;
	IMessageWindow *activeChatWindow() const;
	QTextBrowser *chatViewer(IMessageWindow *AWindow) const;
	IMessageEditWidget *chatEditor(IMessageWindow *AWindow) const;
	void closeMainWindow();
	void setApplicationMode(VimMode AMode);
	void applyModeToWindow(IMessageWindow *AWindow);
	void updateCursor(IMessageWindow *AWindow, bool AEnsureVisible=false);
	void focusModeWidget(IMessageWindow *AWindow);
	QList< QPair<int,int> > messageMarkerRanges(QTextDocument *ADocument) const;
	QPair<int,int> nearestMessageRange(QTextDocument *ADocument, int APosition) const;
	QString messageCopyText(QTextDocument *ADocument, int AStart, int AEnd) const;
	QString visualSelectionText(QTextBrowser *AViewer) const;
	void flashCopiedRange(IMessageWindow *AWindow, int AStart, int AEnd);
private:
	IPluginManager     *FPluginManager = nullptr;
	IMessageWidgets    *FMessageWidgets = nullptr;
	IMainWindowPlugin  *FMainWindowPlugin = nullptr;
	IRostersViewPlugin *FRostersViewPlugin = nullptr;
	IOptionsManager    *FOptionsManager = nullptr;
	IPresenceManager   *FPresenceManager = nullptr;
	IMultiUserChatManager *FMultiChatManager = nullptr;
	IRosterManager     *FRosterManager = nullptr;
	// optional: the :join command completes the bookmarked and recently
	// joined conference rooms
	IBookmarks         *FBookmarks = nullptr;
	IRecentContacts    *FRecentContacts = nullptr;

	VimMode FMode = NormalMode;
	// VISUAL comes in two flavours like in vim: charwise ('v') and
	// linewise ('V'); the latter selects whole displayed lines and ignores
	// the horizontal motions
	bool FVisualLine = false;
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
	// identifies the newest copy flash: an older timer must not clear it
	quint64 FFlashGeneration = 0;
	// the last VISUAL selection range, restored by "gv" in the same viewer
	QPointer<QTextBrowser> FLastVisualViewer;
	int FLastVisualAnchor = -1;
	int FLastVisualPosition = -1;
	bool FLastVisualLine = false;
	// the single ":help" window, kept alive (hidden on close) so its
	// geometry survives the session
	QPointer<VimHelpDialog> FHelpDialog;
	// the running COMMAND mode completion cycle: the candidates, the current
	// one and the name applied last, so repeated Tab keeps cycling instead of
	// restarting; any other key in COMMAND mode drops the cycle
	QStringList FCompletionCandidates;
	int FCompletionIndex = -1;
	QString FCompletionApplied;
};

#endif // VIMPLUGIN_H
