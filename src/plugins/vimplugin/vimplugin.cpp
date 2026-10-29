#include "vimplugin.h"

#include "vimoptionswidget.h"

#include <QApplication>
#include <QClipboard>
#include <QScrollBar>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>

#include <definitions/optionvalues.h>
#include <definitions/shortcuts.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <utils/jid.h>
#include <utils/shortcuts.h>
#include <utils/widgetmanager.h>

// invisible zero-width char used to bind the start of each message in the
// viewer document: gives "message ranges" for vim motions (`{`/`}`) and
// for yb ("copy whole message")
#define VIM_MESSAGE_MARKER      QChar(0x2060)

// the double-Escape gesture window: the vim modes swallow the single Escape
// in the chat windows and in the main window, two presses inside this
// interval close the main window like the plain Esc does without the plugin
#define VIM_DOUBLE_ESCAPE_MSECS  1000

static const QColor kColorNormal  = QColor(0xB4,0xB4,0xB4);	// grey
static const QColor kColorVisual  = QColor(0xA8,0xDC,0xF0);	// light blue
static const QColor kColorCommand = QColor(0xFF,0xD2,0x8A);	// orange
static const QColor kColorSearch  = QColor(0xC0,0xF0,0xC0);	// light green
static const QColor kColorCursor  = QColor(0x64,0x82,0xFF);	// block cursor

static QString stylesheetForMode(VimMode AMode)
{
	switch (AMode)
	{
	case NormalMode:      return QStringLiteral("background-color:%1;").arg(kColorNormal.name());
	case VisualMode:      return QStringLiteral("background-color:%1;").arg(kColorVisual.name());
	case CommandMode:     return QStringLiteral("background-color:%1;").arg(kColorCommand.name());
	case SearchPromptMode:return QStringLiteral("background-color:%1;").arg(kColorSearch.name());
	case InsertMode:      break;	// white = default editor coloring
	}
	return QString();
}

// ============ vim chord parsing ============
// QKeySequence("gg") parses into ONE Key_Unknown chord and loses the case of
// the letters ("G" == "g") but vim keys are both sequence chords and
// case-sensitive: the key strings are parsed here, "gg" means g+g,
// "G" means Shift+g, "Down"/"Ctrl+x" are parsed by Qt as single chords

// symbols which QKeySequence refuses to parse from the single-character
// notation are bound by their ASCII codes directly
static QKeyCombination vimSymbolKey(QChar AChar)
{
	switch (AChar.unicode())
	{
	case ' ':  return QKeyCombination(Qt::Key_Space);
	case '\\': return QKeyCombination(Qt::Key_Backslash);
	case '[':  return QKeyCombination(Qt::Key_BracketLeft);
	case ']':  return QKeyCombination(Qt::Key_BracketRight);
	default:   return QKeyCombination();
	}
}

// the characters '0'..'9' map onto the digit keys directly
static QKeyCombination vimDigitKey(QChar AChar)
{
	if (AChar.unicode() >= 0x0030 && AChar.unicode() <= 0x0039)
	{
		const int offset = AChar.unicode() - 0x0030;
		return QKeyCombination(Qt::Key(int(Qt::Key_0) + offset));
	}
	return QKeyCombination();
}

static QKeyCombination vimChordForCharacter(QChar AChar)
{
	if (AChar.isDigit())
		return vimDigitKey(AChar);
	QChar key = AChar;
	Qt::KeyboardModifiers modifiers = Qt::NoModifier;
	if (AChar.isLetter() && AChar.isUpper())
	{
		modifiers = Qt::ShiftModifier;
		key = AChar.toLower();
	}
	const QKeySequence parsed(key);
	if (parsed.count()==1 && parsed[0].key()!=Qt::Key_unknown)
		return QKeyCombination(modifiers, parsed[0].key());
	const QKeyCombination symbol = vimSymbolKey(AChar);
	if (symbol.key()!=Qt::Key_unknown)
		return symbol;
	return QKeyCombination();
}

static const struct { int action; const char *normal; const char *visual; } kDefaultKeys[] =
{
	{ ActionScrollDown,        "j;Down",   "j;Down" },
	{ ActionScrollUp,          "k;Up",     "k;Up"   },
	{ ActionScrollMessageUp,   "{",        "{"      },
	{ ActionScrollMessageDown, "}",        "}"      },
	{ ActionSearchForward,     "/",        ""       },
	{ ActionFocusRoster,       "gd",       ""       },
	{ ActionGotoFirstLine,     "gg",       "gg"     },
	{ ActionGotoLastLine,      "G",        "G"      },
	{ ActionCopyMessage,       "yb",       ""       },
	{ ActionCopyBlock,         "yy",       ""       },
	{ ActionEnterVisual,       "v",        ""       },
	{ ActionEnterInsert,       "i",        ""       },
	{ ActionEditLastMessage,   "c",        ""       },
	{ ActionEnterCommand,      ":",        ""       },
	{ ActionNextSearchMatch,   "n",        ""       },
	{ ActionPrevSearchMatch,   "N",        ""       },
	{ ActionVisualCopy,        "",         "y"      },
	{ ActionPasteClipboard,    "p",        ""       },
	{ ActionOpenNotification,  "gn",       ""       }
};

// vim cursor may land on the invisible message bounds markers, skip them
// so the drawn block cursor covers a real (visible) character
static QTextCursor normalizeCursor(QTextCursor ACursor)
{
	QTextDocument *document = ACursor.document();
	while (document!=NULL && document->characterAt(ACursor.position()) == VIM_MESSAGE_MARKER
		&& document->characterAt(ACursor.position()) != QChar(QChar::Null))
	{
		if (ACursor.position()+1 >= document->characterCount())
		{
			ACursor.movePosition(QTextCursor::Left);
			break;
		}
		ACursor.movePosition(QTextCursor::Right);
	}
	return ACursor;
}

// splits a vim key notation into the chord sequence: multi-char tokens
// like "Down" or "Ctrl+x" (Qt understands them) are whole chords, everything
// else is entered character by character ("gg" is g+g like in vim)
static QList<QKeyCombination> vimParseChords(const QString &AVariant)
{
	QList<QKeyCombination> chords;
	QString rest = AVariant;
	while (!rest.isEmpty())
	{
		while (!rest.isEmpty() && rest.at(0).isSpace())
			rest.remove(0,1);
		if (rest.isEmpty())
			break;
		const int spacePos = rest.indexOf(' ');
		const QString chunk = spacePos<0 ? rest : rest.left(spacePos);
		rest.remove(0, spacePos<0 ? chunk.length() : spacePos);

		bool chunkChord = false;
		if (chunk.length()>1)
		{
			const QKeySequence parsed(chunk);
			if (parsed.count()==1 && parsed[0].key()!=Qt::Key_unknown)
			{
				chords.append(QKeyCombination(parsed[0].keyboardModifiers(), parsed[0].key()));
				chunkChord = true;
			}
			// else - the chunk of plain letters like "gg" is intentionally
			// parsed by the characters below, no warning is needed
		}
		if (!chunkChord)
			foreach (const QChar &ch, chunk)
				chords.append(vimChordForCharacter(ch));
	}
	return chords;
}

QString VimPlugin::actionTitle(VimActionId AAction)
{
	switch (AAction)
	{
	case ActionScrollDown:        return tr("Scroll down (chat or roster)");
	case ActionScrollUp:          return tr("Scroll up (chat or roster)");
	case ActionScrollMessageUp:   return tr("Move one message up");
	case ActionScrollMessageDown: return tr("Move one message down");
	case ActionSearchForward:     return tr("Search in chat");
	case ActionFocusRoster:       return tr("Focus contact list");
	case ActionGotoFirstLine:     return tr("Jump to the very top of chat");
	case ActionGotoLastLine:      return tr("Jump to the very bottom of chat");
	case ActionCopyMessage:       return tr("Copy current message");
	case ActionCopyBlock:         return tr("Copy current line");
	case ActionEnterVisual:       return tr("Enter visual mode");
	case ActionEnterInsert:       return tr("Enter insert mode");
	case ActionEditLastMessage:   return tr("Insert mode with last message editing");
	case ActionEnterCommand:      return tr("Enter command mode");
	case ActionNextSearchMatch:   return tr("Next search match");
	case ActionPrevSearchMatch:   return tr("Previous search match");
	case ActionVisualCopy:        return tr("Copy selection (visual mode)");
	case ActionPasteClipboard:    return tr("Paste clipboard and enter insert mode");
	case ActionOpenNotification:  return tr("Open last notification");
	default:                      return QString();
	}
}

QString VimPlugin::actionKeysString(VimMode AMode, VimActionId AAction)
{
	QString value = Options::node(OPV_VIM_KEYS.arg(AAction)).value().toString();
	if (!value.isEmpty())
		return value;
	for (unsigned i=0; i<sizeof(kDefaultKeys)/sizeof(kDefaultKeys[0]); ++i)
		if (kDefaultKeys[i].action == AAction)
			return QString::fromLatin1(AMode==VisualMode ? kDefaultKeys[i].visual : kDefaultKeys[i].normal);
	return QString();
}

void VimPlugin::saveActionKeysString(VimMode AMode, VimActionId AAction, const QString &AKeys)
{
	Q_UNUSED(AMode);
	Options::node(OPV_VIM_KEYS.arg(AAction)).setValue(AKeys);
}

// The global hotkeys are ordinary Shortcuts records declared by the
// mainwindow ("show roster") and notifications ("activate notification")
// plugins. The shortcut manager plugin normally loads and saves them, but it
// conflicts with the vim mode, so the same storage is handled here.
QKeySequence VimPlugin::globalShortcutKey(const QString &AId)
{
	OptionsNode options = Options::node(OPV_SHORTCUTS);
	if (options.hasNode(AId))
		return QKeySequence(options.value(AId).toString());
	return Shortcuts::shortcutDescriptor(AId).activeKey;
}

void VimPlugin::saveGlobalShortcut(const QString &AId, const QKeySequence &AKey)
{
	Shortcuts::updateShortcut(AId, AKey);
	OptionsNode options = Options::node(OPV_SHORTCUTS);
	if (AKey != Shortcuts::shortcutDescriptor(AId).defaultKey)
		options.setValue(AKey.toString(), AId);
	else
		options.removeNode(AId);
}

void VimPlugin::loadGlobalShortcuts()
{
	Shortcuts::setGlobalShortcut(SCT_GLOBAL_SHOWROSTER, true);
	Shortcuts::setGlobalShortcut(SCT_GLOBAL_ACTIVATELASTNOTIFICATION, true);

	OptionsNode options = Options::node(OPV_SHORTCUTS);
	foreach (const QString &shortcutId, QList<QString>() << SCT_GLOBAL_SHOWROSTER << SCT_GLOBAL_ACTIVATELASTNOTIFICATION)
	{
		if (options.hasNode(shortcutId))
			Shortcuts::updateShortcut(shortcutId, QKeySequence(options.value(shortcutId).toString()));
	}
}

QList< QList<QKeyCombination> > VimPlugin::parseKeyString(const QString &AKeys) const
{
	QHash<QString, QList< QList<QKeyCombination> > >::const_iterator cached = FParseCache.constFind(AKeys);
	if (cached != FParseCache.constEnd())
		return cached.value();

	// variants separated by ';', each variant is a vim chord sequence which
	// may hold several chords ("gg" is g+g, "G" is Shift+g)
	QList< QList<QKeyCombination> > variants;
	QStringList variantList = AKeys.split(';', Qt::KeepEmptyParts);
	foreach (const QString &variant, variantList)
	{
		QString key = variant.trimmed();
		if (key.isEmpty())
			continue;
		QList<QKeyCombination> chords;
		foreach (const QKeyCombination &chord, vimParseChords(key))
			if (chord.key()!=Qt::Key_unknown)
				chords.append(chord);
		if (!chords.isEmpty())
			variants.append(chords);
	}
	FParseCache.insert(AKeys, variants);
	return variants;
}

QList< QList<QKeyCombination> > VimPlugin::actionKeys(VimMode AMode, VimActionId AAction) const
{
	QList< QList<QKeyCombination> > keys;
	switch (AMode)
	{
	case InsertMode:
	case CommandMode:
	case SearchPromptMode:
		return keys;
	default:
		break;
	}

	static const QList<VimActionId> normalActions = QList<VimActionId>()
		<< ActionScrollUp << ActionScrollDown << ActionScrollMessageUp << ActionScrollMessageDown
		<< ActionSearchForward << ActionFocusRoster << ActionGotoFirstLine << ActionGotoLastLine
		<< ActionCopyMessage << ActionCopyBlock << ActionEnterVisual << ActionEnterInsert
		<< ActionEditLastMessage << ActionEnterCommand << ActionNextSearchMatch << ActionPrevSearchMatch
		<< ActionPasteClipboard << ActionOpenNotification;
	static const QList<VimActionId> visualActions = QList<VimActionId>()
		<< ActionScrollUp << ActionScrollDown << ActionScrollMessageUp << ActionScrollMessageDown
		<< ActionGotoFirstLine << ActionGotoLastLine << ActionVisualCopy;

	const QList<VimActionId> &allowed = AMode==VisualMode ? visualActions : normalActions;
	if (allowed.contains(AAction))
		keys = parseKeyString(actionKeysString(AMode, AAction));
	return keys;
}

void VimPlugin::loadKeymap()
{
	FKeymap.clear();
	FParseCache.clear();
	for (VimMode mode : { NormalMode, VisualMode })
	{
		for (int action=0; action<Action__Count; ++action)
		{
			const QList< QList<QKeyCombination> > variants = actionKeys(mode, VimActionId(action));
			foreach (const QList<QKeyCombination> &variant, variants)
				FKeymap[VimActionId(action)].append(variant);
		}
	}
}

int VimPlugin::resolveAction(VimMode AMode, const QList<QKeyCombination> &AKeys) const
{
	for (int action=0; action<Action__Count; ++action)
	{
		const QList< QList<QKeyCombination> > variants = actionKeys(AMode, VimActionId(action));
		foreach (const QList<QKeyCombination> &keys, variants)
		{
			if (keys.isEmpty() || keys.size()!=AKeys.size())
				continue;
			bool matches = true;
			for (int i=0; i<keys.size() && matches; ++i)
				matches = keys.at(i).toCombined() == AKeys.at(i).toCombined();
			if (matches)
				return action;
		}
	}
	return -1;
}

bool VimPlugin::actionHasPartialMatch(VimMode AMode, const QList<QKeyCombination> &AKeys) const
{
	for (int action=0; action<Action__Count; ++action)
	{
		const QList< QList<QKeyCombination> > variants = actionKeys(AMode, VimActionId(action));
		foreach (const QList<QKeyCombination> &keys, variants)
		{
			if (keys.isEmpty() || keys.size()<=AKeys.size())
				continue;
			bool matches = true;
			for (int i=0; i<AKeys.size() && matches; ++i)
				matches = keys.at(i).toCombined() == AKeys.at(i).toCombined();
			if (matches)
				return true;
		}
	}
	return false;
}

// =========== Plugin lifecycle ============
VimPlugin::VimPlugin()
{
}

VimPlugin::~VimPlugin()
{
	if (qApp)
		qApp->removeEventFilter(this);
}

void VimPlugin::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Vim mode");
	APluginInfo->description = tr("Vim-like keyboard modes for chats and contact list (NORMAL, INSERT, VISUAL, COMMAND). Double Escape closes the main window. Conflicts with the Shortcut Manager plugin.");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	APluginInfo->conflicts.append(QUuid(SHORTCUTMANAGER_UUID));
}

bool VimPlugin::initConnections(IPluginManager *APluginManager, int &AInitOrder)
{
	Q_UNUSED(AInitOrder);

	FPluginManager = APluginManager;

	IPlugin *plugin = APluginManager->pluginInterface("IMessageWidgets").value(0,NULL);
	if (plugin)
		FMessageWidgets = qobject_cast<IMessageWidgets *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IMainWindowPlugin").value(0,NULL);
	if (plugin)
		FMainWindowPlugin = qobject_cast<IMainWindowPlugin *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IRostersViewPlugin").value(0,NULL);
	if (plugin)
		FRostersViewPlugin = qobject_cast<IRostersViewPlugin *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IOptionsManager").value(0,NULL);
	if (plugin)
		FOptionsManager = qobject_cast<IOptionsManager *>(plugin->instance());

	plugin = APluginManager->pluginInterface("IPresenceManager").value(0,NULL);
	if (plugin)
		FPresenceManager = qobject_cast<IPresenceManager *>(plugin->instance());

	connect(Options::instance(), SIGNAL(optionsOpened()), SLOT(onOptionsOpened()));
	connect(Options::instance(), SIGNAL(optionsClosed()), SLOT(onOptionsClosed()));

	return FMessageWidgets != NULL;
}

bool VimPlugin::initObjects()
{
	return true;
}

bool VimPlugin::initSettings()
{
	Options::setDefaultValue(OPV_VIM_ENABLE, true);
	Options::setDefaultValue(OPV_VIM_COUNTS, true);

	if (FOptionsManager)
	{
		IOptionsDialogNode dnode = { ONO_VIM, OPN_VIM, "", tr("Vim mode") };
		FOptionsManager->insertOptionsDialogNode(dnode);
		FOptionsManager->insertOptionsDialogHolder(this);
	}
	return true;
}

bool VimPlugin::startPlugin()
{
	// The profile options are opened by the options manager plugin during
	// its own start and the plugin start order is not fixed - load the
	// keymap and the global shortcut keys only when the options are
	// already open (a runtime plugin reload), onOptionsOpened() does it
	// in the normal startup flow
	if (!Options::isNull())
	{
		loadKeymap();
		loadGlobalShortcuts();
	}
	LOG_INFO(QString("[VimPlugin] started, messageWidgets=%1, rostersView=%2, presenceManager=%3, options=%4")
		.arg(FMessageWidgets!=NULL).arg(FRostersViewPlugin!=NULL).arg(FPresenceManager!=NULL).arg(FOptionsManager!=NULL));

	if (FMessageWidgets)
	{
		FMessageWidgets->insertStyleHandler(MSHO_VIM, this);
		connect(FMessageWidgets->instance(), SIGNAL(chatWindowCreated(IMessageChatWindow *)),
			SLOT(onChatWindowCreated(IMessageChatWindow *)));
		connect(FMessageWidgets->instance(), SIGNAL(chatWindowDestroyed(IMessageChatWindow *)),
			SLOT(onChatWindowDestroyed(IMessageChatWindow *)));
		foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
		{
			installEditorFilter(window);
			applyModeToWindow(window);
		}
	}
	qApp->installEventFilter(this);
	return true;
}

QMultiMap<int, IOptionsDialogWidget *> VimPlugin::optionsDialogWidgets(const QString &ANodeId, QWidget *AParent)
{
	QMultiMap<int, IOptionsDialogWidget *> widgets;
	if (ANodeId == OPN_VIM)
		widgets.insert(OWO_VIM, new VimOptionsWidget(AParent));
	return widgets;
}

bool VimPlugin::messageStyleContentInsert(int AOrder, QWidget *AWidget, const QString &AHtml, QString &AModifiedHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AOrder);
	Q_UNUSED(AWidget);
	if (AOptions.kind == IMessageStyleContentOptions::KindMessage)
		AModifiedHtml = QStringLiteral("<span>%1</span>").arg(VIM_MESSAGE_MARKER) + AHtml;
	return true;
}

QString VimPlugin::messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AOrder); Q_UNUSED(AWidget); Q_UNUSED(ASlotName); Q_UNUSED(ASlotInTemplate); Q_UNUSED(AOptions);
	return QString();
}

void VimPlugin::installEditorFilter(IMessageChatWindow *AWindow)
{
	IMessageEditWidget *editor = chatEditor(AWindow);
	QWidget *textEdit = editor!=NULL ? editor->textEdit() : NULL;
	if (textEdit!=NULL && !FEditorWidgets.contains(textEdit))
	{
		// object level filters of one widget run in the reverse order and this
		// call happens after the EditWidget has installed its own filter on
		// the editor, so the vim filter fires before the "send" hook and the
		// QShortcut("Enter","Esc") of the editor have any influence
		textEdit->installEventFilter(this);
		FEditorWidgets.append(textEdit);
		LOG_INFO("[VimPlugin] vim filter installed on the chat editor");
	}
}

void VimPlugin::onChatWindowCreated(IMessageChatWindow *AWindow)
{
	installEditorFilter(AWindow);
	if (AWindow!=NULL && AWindow->viewWidget()!=NULL)
	{
		// the document is reloaded by clearContent() and the style widgets
		// are replaced when the style changes - re-apply the mode colors
		// and the cursor on every such change
		connect(AWindow->viewWidget()->instance(), SIGNAL(contentAppended(const QString &, const IMessageStyleContentOptions &)),
			SLOT(onWindowContentAppended()));
		connect(AWindow->viewWidget()->instance(), SIGNAL(messageStyleChanged(IMessageStyle *,const IMessageStyleOptions &)),
			SLOT(onWindowStyleChanged()));
		LOG_INFO(QString("[VimPlugin] chat window opened, viewer class=%1, markers=%2")
			.arg(chatViewer(AWindow)!=NULL ? QString::fromLatin1(chatViewer(AWindow)->metaObject()->className()) : QString("null"))
			.arg(chatViewer(AWindow)!=NULL ? messageMarkerRanges(chatViewer(AWindow)->document()).count() : 0));
	}
	applyModeToWindow(AWindow);
}

void VimPlugin::onChatWindowDestroyed(IMessageChatWindow *AWindow)
{
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (editor!=NULL)
		FEditorWidgets.removeAll(editor->textEdit());
}

// any append (new message, history load) invalidates the document of the
// viewer, refresh the vim cursor and the mode colors
void VimPlugin::onWindowContentAppended()
{
	IMessageChatWindow *window = NULL;
	foreach (IMessageChatWindow *win, FMessageWidgets->chatWindows())
		if (win->viewWidget()!=NULL && win->viewWidget()->instance()==sender())
			window = win;
	placeCursorAtLastMessage(window);
	applyModeToWindow(window);
}

void VimPlugin::onWindowStyleChanged()
{
	IMessageChatWindow *window = NULL;
	foreach (IMessageChatWindow *win, FMessageWidgets->chatWindows())
		if (win->viewWidget()!=NULL && win->viewWidget()->instance()==sender())
			window = win;
	placeCursorAtLastMessage(window);
	applyModeToWindow(window);
}

// place the vim cursor on the first character of the last (newest) message,
// anchor of the transparent '- like in vim G' behaviour after the document
// was reset or grown at the bottom
static QTextCursor visibleCharacterCursor(QTextCursor ACursor, const QList<QPair<int,int> > &ARanges);

void VimPlugin::placeCursorAtLastMessage(IMessageChatWindow *AWindow)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	// NORMAL shows no cursor at all, and a new message must never yank the
	// reader who scrolled up: anchor only in VISUAL and only while the view
	// follows the conversation
	if (viewer==NULL || FMode!=VisualMode)
		return;
	QScrollBar *scroll = viewer->verticalScrollBar();
	if (scroll!=NULL && scroll->value()!=scroll->maximum())
		return;
	const QList< QPair<int,int> > ranges = messageMarkerRanges(viewer->document());
	if (ranges.isEmpty())
		return;
	QTextCursor cursor(viewer->document());
	cursor.movePosition(QTextCursor::End);
	cursor = visibleCharacterCursor(cursor, ranges);
	cursor.clearSelection();
	viewer->setTextCursor(cursor);
}

// ============ helpers ============
IMessageChatWindow *VimPlugin::contextWindow(QWidget *AFocusWidget) const
{
	if (FMessageWidgets==NULL || AFocusWidget==NULL)
		return NULL;
	foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
		if (window->instance()->isAncestorOf(AFocusWidget))
			return window;
	return NULL;
}

IMessageChatWindow *VimPlugin::activeChatWindow() const
{
	if (FMessageWidgets==NULL)
		return NULL;
	foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
		if (window->isActiveTabPage())
			return window;
	return NULL;
}

QTextBrowser *VimPlugin::chatViewer(IMessageChatWindow *AWindow) const
{
	if (AWindow==NULL || AWindow->viewWidget()==NULL)
		return NULL;
	return qobject_cast<QTextBrowser *>(AWindow->viewWidget()->styleWidget());
}

IMessageEditWidget *VimPlugin::chatEditor(IMessageChatWindow *AWindow) const
{
	return AWindow!=NULL ? AWindow->editWidget() : NULL;
}

// hides the main window the same way the plain Esc widget shortcut does it
// (see SCT_ROSTERVIEW_CLOSEWINDOW), the application keeps running in tray
void VimPlugin::closeMainWindow()
{
	IMainWindow *mainWindow = FMainWindowPlugin!=NULL ? FMainWindowPlugin->mainWindow() : NULL;
	if (mainWindow != NULL)
	{
		LOG_INFO("[VimPlugin] closing main window");
		mainWindow->closeWindow();
	}
	else
	{
		LOG_WARNING("[VimPlugin] cannot hide the main window, main window plugin is not available");
	}
}

// ============ modes ============
void VimPlugin::setApplicationMode(VimMode AMode)
{
	FMode = AMode;
	FKeyBuffer.clear();
	FCount = -1;
	if (FMessageWidgets!=NULL)
		foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
			applyModeToWindow(window);
	focusModeWidget(activeChatWindow());
}

void VimPlugin::applyModeToWindow(IMessageChatWindow *AWindow)
{
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (editor==NULL)
		return;

	QTextEdit *textEdit = editor->textEdit();
	switch (FMode)
	{
	case NormalMode:
	case VisualMode:
		editor->setEditEnabled(false);
		if (textEdit!=NULL) textEdit->setStyleSheet(stylesheetForMode(FMode));
		break;
	case CommandMode:
	case SearchPromptMode:
		editor->setEditEnabled(true);
		if (textEdit!=NULL) textEdit->setStyleSheet(stylesheetForMode(FMode));
		break;
	case InsertMode:
		editor->setEditEnabled(true);
		if (textEdit!=NULL) textEdit->setStyleSheet(QString());
		break;
	}

	updateCursor(AWindow);
}

// the vim cursor needs to cover a VISIBLE character: the invisible message
// bounds markers are zero-width (drawn selection would be invisible), the
// caret at the document end covers the paragraph separator and likewise
// draws nothing, so it is anchored on the first character of the last message

// the message window step is the frame scan of
// WidgetManager::scrollDocumentByItems, but the caller owns the scrolling:
// setting the vim cursor right after a scroll calls ensureCursorVisible and
// rolls the viewport back (the trace "4 -> 50 (slider) -> 4 (cursor)"), so
// the cursor must be placed first and the scrollbar set last. The target
// cursor is the target frame's first position - a hit test on the viewport
// top returns the stale position right after the scroll
static QTextCursor vimMessageScrollTarget(QTextEdit *AView, bool APrevious, int &ATargetPosition)
{
	ATargetPosition = -1;
	QScrollBar *scrollBar = AView->verticalScrollBar();
	if (scrollBar==NULL)
		return QTextCursor();
	const int topPosition = scrollBar->value();
	const int epsilon = 5;
	QTextCursor target;
	QTextFrame *rootFrame = AView->document()->rootFrame();
	for (QTextFrame::iterator it=rootFrame->begin(); !it.atEnd(); ++it)
	{
		QTextFrame *childFrame = it.currentFrame();
		if (childFrame)
		{
			QTextCursor cursor = childFrame->firstCursorPosition();
			const int childPosition = AView->cursorRect(cursor).y() + topPosition;
			if (APrevious)
			{
				if (childPosition < topPosition-epsilon && childPosition > ATargetPosition)
				{
					ATargetPosition = childPosition;
					target = cursor;
				}
			}
			else
			{
				if (childPosition > topPosition+epsilon && (ATargetPosition<0 || childPosition < ATargetPosition))
				{
					ATargetPosition = childPosition;
					target = cursor;
				}
			}
		}
	}
	// beyond the last message: the target is the document end
	if (!APrevious && ATargetPosition < 0)
		ATargetPosition = scrollBar->maximum();
	else if (ATargetPosition > scrollBar->maximum())
		ATargetPosition = scrollBar->maximum();
	return target;
}

static QTextCursor visibleCharacterCursor(QTextCursor ACursor, const QList<QPair<int,int> > &ARanges)
{
	// a visual selection is already visible by definition and any move here
	// would collapse it (movePosition/setPosition reset the anchor)
	if (ACursor.hasSelection())
		return ACursor;
	QTextDocument *document = ACursor.document();
	ACursor = normalizeCursor(ACursor);
	if (!ARanges.isEmpty() && ACursor.position() >= ARanges.last().second)
		ACursor.setPosition(ARanges.last().first);
	while (ACursor.position() >= document->characterCount())
	{
		if (ACursor.position() <= 0)
			break;
		ACursor.movePosition(QTextCursor::Left);
	}
	while (ACursor.position() < document->characterCount())
	{
		const QChar ch = document->characterAt(ACursor.position());
		// the message bounds markers are zero width, the frame boundary is
		// not a real character, the paragraph separator ends a block: none of
		// them can be covered by a visible block cursor
		if (ch != VIM_MESSAGE_MARKER && ch != QChar::ParagraphSeparator
			&& ch != QChar::LineSeparator && ch != QChar::ObjectReplacementCharacter
			&& ch != QChar(QChar::Null))
			break;
		if (ACursor.position() <= 0)
			break;
		ACursor.movePosition(QTextCursor::Left);
	}
	return ACursor;
}

void VimPlugin::updateCursor(IMessageChatWindow *AWindow, bool AEnsureVisible)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	if (viewer==NULL)
		return;

	if (FMode==InsertMode || FMode==CommandMode || FMode==SearchPromptMode)
	{
		viewer->setExtraSelections(QList<QTextEdit::ExtraSelection>());
		return;
	}

	// vim NORMAL shows no cursor at all: the view is scrolled with the native
	// cursor keys (j/k are forwarded as Down/Up) and the caret of the read-only
	// viewer is hidden while NORMAL is active
	if (FMode==NormalMode)
	{
		// leaving VISUAL (or any selection made in the viewer) must clear the
		// native selection too, otherwise the highlight stays in NORMAL
		QTextCursor cursor = viewer->textCursor();
		if (cursor.hasSelection())
		{
			cursor.clearSelection();
			viewer->setTextCursor(cursor);
		}
		viewer->setExtraSelections(QList<QTextEdit::ExtraSelection>());
		viewer->setCursorWidth(0);
		return;
	}
	if (viewer->cursorWidth()==0)
		viewer->setCursorWidth(1);

	QTextCursor cursor = visibleCharacterCursor(viewer->textCursor(), messageMarkerRanges(viewer->document()));
	// the document grows at the bottom and the cursor may stay mid document
	// or above the viewport (position=34 while chars=78 in the trace), draw
	// it on the first visible character so it is never off the screen
	if (!cursor.hasSelection())
	{
		const QTextCursor topCursor = viewer->cursorForPosition(QPoint(0,0));
		const QTextCursor bottomCursor = viewer->cursorForPosition(QPoint(viewer->viewport()->width()-1, viewer->viewport()->height()-1));
		const int beforePosition = cursor.position();
		if (cursor.position() < topCursor.position() || cursor.position() > bottomCursor.position())
			cursor = visibleCharacterCursor(topCursor, messageMarkerRanges(viewer->document()));
		static int cursorLogs = 0;
		if (cursorLogs < 8)
		{
			++cursorLogs;
			LOG_INFO(QString("[VimPlugin] cursor update: pos=%1, topVisible=%2, bottomVisible=%3, chars=%4, blockedPos=%5")
				.arg(beforePosition).arg(topCursor.position()).arg(bottomCursor.position())
				.arg(viewer->document()->characterCount()).arg(cursor.position()));
		}
	}
	viewer->setTextCursor(cursor);
	if (!cursor.hasSelection())
	{
		static int cursorLogs = 0;
		if (cursorLogs < 5)
		{
			++cursorLogs;
			LOG_INFO(QString("[VimPlugin] vim cursor drawn at position=%1, chars=%2, markers=%3, viewer=%4")
				.arg(cursor.position())
				.arg(viewer->document()->characterCount())
				.arg(messageMarkerRanges(viewer->document()).count())
				.arg(QString::fromLatin1(viewer->metaObject()->className())));
		}
	}

	QTextEdit::ExtraSelection selection;
	selection.cursor = cursor;
	// no FullWidthSelection property here: it paints the whole line and in
	// the table based styles it covers only the header row of the message
	// which looks like an accidental text selection; a plain one character
	// block always covers a visible character (see visibleCharacterCursor)
	if (FMode==VisualMode && selection.cursor.hasSelection())
	{
		selection.format.setBackground(kColorCursor.darker(130));
		selection.format.setForeground(Qt::white);
	}
	else
	{
		selection.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
		selection.format.setBackground(kColorCursor);
		selection.format.setForeground(Qt::white);
	}
	viewer->setExtraSelections(QList<QTextEdit::ExtraSelection>() << selection);
}

// focus the mode widget of the active window: the viewer for NORMAL and
// VISUAL, the editor for the rest; the prompt modes must own the editor
// even when the tab page check does not confirm the window as active -
// with the focus left on the viewer the Escape is swallowed there and
// the COMMAND/SEARCH prompt never terminates
void VimPlugin::focusModeWidget(IMessageChatWindow *AWindow)
{
	if (AWindow==NULL)
		return;
	QTextBrowser *viewer = chatViewer(AWindow);
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (FMode==NormalMode || FMode==VisualMode)
	{
		if (AWindow->isActiveTabPage() && viewer!=NULL)
			viewer->setFocus();
	}
	else if (editor!=NULL)
	{
		QWidget *textEdit = editor->textEdit();
		if (textEdit!=NULL)
			textEdit->setFocus();
	}
}

// ============ key handling ============
static QList<QKeyCombination> eventToCombinations(QKeyEvent *AEvent);

bool VimPlugin::eventFilter(QObject *watched, QEvent *event)
{
	if (!Options::node(OPV_VIM_ENABLE).value().toBool())
		return false;

	// synthesized scrolling keys go straight to the viewer
	if (FForwardingKey)
		return false;

	QWidget *editorWidget = qobject_cast<QWidget *>(watched);
	if (editorWidget!=NULL && FEditorWidgets.contains(editorWidget))
		return editorEventFilter(editorWidget, event);

	if (event->type() == QEvent::ShortcutOverride)
	{
		// the widget shortcuts (Esc close, Enter send) send ShortcutOverride
		// before they get activated; when no other object filter has eaten
		// the event it reaches the application filter where the vim modes
		// claim the Escape inside the chat windows and on the roster
		QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
		if (keyEvent->key()!=Qt::Key_Escape)
			return false;

		QWidget *focusWidget = QApplication::focusWidget();
		if (contextWindow(focusWidget)!=NULL)
		{
			keyEvent->accept();
			return true;
		}
		if (FRostersViewPlugin!=NULL && FMode==NormalMode)
		{
			QWidget *rosterWidget = FRostersViewPlugin->rostersView()!=NULL ? FRostersViewPlugin->rostersView()->instance() : NULL;
			if (rosterWidget!=NULL && (focusWidget==rosterWidget || rosterWidget->isAncestorOf(focusWidget)))
			{
				keyEvent->accept();
				return true;
			}
		}
		return false;
	}

	// the release of a key that vim has consumed must not reach the widget
	// which the key moved the focus to (the roster search takes the letter
	// on release)
	if (event->type() == QEvent::KeyRelease)
		return FConsumedKeys.remove(static_cast<QKeyEvent *>(event)->key());

	if (event->type() != QEvent::KeyPress)
		return false;

	QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
	if (keyEvent->isAutoRepeat())
	{
		// holding a scrolling key repeats like the native cursor keys; every
		// other key waits for a fresh press (modes, commands, roster walk)
		if (FMode!=NormalMode && FMode!=VisualMode)
			return false;
		QWidget *focusWidget = QApplication::focusWidget();
		IMessageChatWindow *window = contextWindow(focusWidget);
		QTextBrowser *viewer = window!=NULL ? chatViewer(window) : NULL;
		if (viewer==NULL || (focusWidget!=viewer && !viewer->isAncestorOf(focusWidget)))
			return false;
		const int action = resolveAction(FMode, eventToCombinations(keyEvent));
		if (action!=ActionScrollDown && action!=ActionScrollUp)
			return false;
	}

	if (!processKey(keyEvent))
		return false;
	FConsumedKeys.insert(keyEvent->key());
	return true;
}

bool VimPlugin::editorEventFilter(QWidget *editorWidget, QEvent *event)
{
	// vim owns Enter and Escape on the chat editors: Enter must not trigger
	// the "send" widget shortcut and the manual EditWidget hook, Escape must
	// not trigger the "close window" shortcut, and the command line must
	// never send anything to the contact
	if (event->type()!=QEvent::ShortcutOverride && event->type()!=QEvent::KeyPress)
		return false;

	QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
	const bool isEnter = keyEvent->key()==Qt::Key_Return || keyEvent->key()==Qt::Key_Enter;
	const bool isEscape = keyEvent->key()==Qt::Key_Escape;
	// typing anything but Escape breaks the double-Escape gesture
	if (!isEscape)
		FEscapeElapsed.invalidate();
	const bool ownsKey = isEscape || (isEnter && (FMode==CommandMode || FMode==SearchPromptMode));
	if (!ownsKey)
		return false;
	if (keyEvent->isAutoRepeat())
		return false;

	LOG_INFO(QString("[VimPlugin] editor key: key=%1, type=%2, mode=%3")
		.arg(keyEvent->key()).arg(event->type()).arg(FMode));

	if (event->type()==QEvent::ShortcutOverride)
	{
		keyEvent->accept();
		return true;
	}

	// the override was accepted and the KeyPress arrived
	if (isEscape && registerEscapeKey())
	{
		// double Escape closes the main window, the single press keeps
		// its mode switching meaning (see processEditorKey)
		keyEvent->accept();
		closeMainWindow();
		return true;
	}
	processEditorKey(keyEvent);
	return true;
}

// ============ double Escape ============
// true when this Escape is the second one within VIM_DOUBLE_ESCAPE_MSECS;
// the timer restarts on every press, so the gesture always starts over
bool VimPlugin::registerEscapeKey()
{
	if (!FEscapeElapsed.isValid())
	{
		FEscapeElapsed.start();
		return false;
	}
	const bool doubled = FEscapeElapsed.elapsed() <= VIM_DOUBLE_ESCAPE_MSECS;
	FEscapeElapsed.restart();
	return doubled;
}

// the double-Escape hotkey works in the vim territory only: inside the chat
// windows (tabbed or detached) or inside the main window (roster, tab bar,
// tool bars); the Escape pressed in the option dialogs and other popups
// keeps its normal meaning and does not close anything
bool VimPlugin::inMainWindowContext(QWidget *AFocusWidget) const
{
	if (AFocusWidget == NULL)
		return false;
	if (contextWindow(AFocusWidget) != NULL)
		return true;
	QWidget *mainWindowWidget = FMainWindowPlugin!=NULL && FMainWindowPlugin->mainWindow()!=NULL
		? FMainWindowPlugin->mainWindow()->instance() : NULL;
	return mainWindowWidget!=NULL && mainWindowWidget->isAncestorOf(AFocusWidget);
}

static bool s_firstKeyLogged = false;

bool VimPlugin::processKey(QKeyEvent *AEvent)
{
	QWidget *focusWidget = QApplication::focusWidget();
	if (focusWidget==NULL)
		return false;

	if (!s_firstKeyLogged)
	{
		s_firstKeyLogged = true;
		IMessageChatWindow *window = contextWindow(focusWidget);
		bool rosterContext = false;
		if (FRostersViewPlugin!=NULL)
		{
			QWidget *rosterWidget = FRostersViewPlugin->rostersView()!=NULL ? FRostersViewPlugin->rostersView()->instance() : NULL;
			rosterContext = rosterWidget!=NULL && (focusWidget==rosterWidget || rosterWidget->isAncestorOf(focusWidget));
		}
		LOG_INFO(QString("[VimPlugin] first key event: key=%1, mode=%2, chatContext=%3, rosterContext=%4, focusClass=%5")
			.arg(AEvent->key()).arg(FMode).arg(window!=NULL).arg(rosterContext).arg(QString::fromLatin1(focusWidget->metaObject()->className())));
	}

	// the double-Escape gesture: every other key breaks it, two Escapes
	// within VIM_DOUBLE_ESCAPE_MSECS close the main window; the escape
	// outside the chat windows and the main window is not counted at all
	const bool escapeInWindow = AEvent->key()==Qt::Key_Escape && !AEvent->isAutoRepeat()
		&& inMainWindowContext(focusWidget);
	if (escapeInWindow && registerEscapeKey())
	{
		AEvent->accept();
		closeMainWindow();
		return true;
	}
	if (!escapeInWindow)
		FEscapeElapsed.invalidate();

	IMessageChatWindow *window = contextWindow(focusWidget);
	if (AEvent->key()==Qt::Key_Escape)
	{
		LOG_INFO(QString("[VimPlugin] Escape: mode=%1, focusClass=%2, inChatWindow=%3")
			.arg(FMode)
			.arg(QString::fromLatin1(focusWidget->metaObject()->className()))
			.arg(window!=NULL));
	}
	if (window!=NULL)
	{
		QTextBrowser *viewer = chatViewer(window);
		if (viewer!=NULL && (focusWidget==viewer || viewer->isAncestorOf(focusWidget)))
			return processViewerKey(AEvent);

		IMessageEditWidget *editor = chatEditor(window);
		if (editor!=NULL && editor->instance()->isAncestorOf(focusWidget))
			return processEditorKey(AEvent);

		// other widgets inside the chat window (tool bars, info/receivers):
		// Escape must not leak to the close-window shortcut, the prompt
		// modes and visual mode behave like vim and leave to NORMAL
		if (AEvent->key()==Qt::Key_Escape)
		{
			if (FMode!=InsertMode && FMode!=NormalMode)
				setApplicationMode(NormalMode);
			else
				AEvent->accept();
			return true;
		}
		return false;
	}

	if (FRostersViewPlugin!=NULL)
	{
		QWidget *rosterWidget = FRostersViewPlugin->rostersView()->instance();
		if (rosterWidget!=NULL && (focusWidget==rosterWidget || rosterWidget->isAncestorOf(focusWidget)))
			return processRosterKey(AEvent);
	}

	// The main window hides itself on the widget shortcut bound to Escape,
	// catch the strays anywhere else inside the main window (tab bar, status
	// bar, tool bars): vim returns to the buffer instead of hiding
	if (AEvent->key()==Qt::Key_Escape && FMainWindowPlugin!=NULL)
	{
		QWidget *mainWindowWidget = FMainWindowPlugin->mainWindow()!=NULL ? FMainWindowPlugin->mainWindow()->instance() : NULL;
		if (FMode==NormalMode && mainWindowWidget!=NULL && mainWindowWidget->isAncestorOf(focusWidget))
		{
			setApplicationMode(NormalMode);	// resets buffers and refocuses
			return true;
		}
	}
	return false;
}

// The event token for the sequence matcher: printable text has a priority
// and is case-sensitive ("G" is Shift+g), otherwise the key combination is
// used ("Down", "Escape"...)
static QList<QKeyCombination> eventToCombinations(QKeyEvent *AEvent)
{
	QList<QKeyCombination> combinations;
	if (AEvent->text().length()==1 && !(AEvent->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier)))
	{
		const QKeyCombination chord = vimChordForCharacter(AEvent->text().at(0));
		if (chord.key()!=Qt::Key_unknown)
			combinations.append(chord);
	}
	if (combinations.isEmpty())
		combinations.append(AEvent->keyCombination());
	return combinations;
}

bool VimPlugin::processViewerKey(QKeyEvent *AEvent)
{
	IMessageChatWindow *window = contextWindow(QApplication::focusWidget());
	if (window==NULL)
		return false;

	LOG_INFO(QString("[VimPlugin] viewer key: key=%1, mods=%2, text='%3', mode=%4")
		.arg(AEvent->key()).arg(int(AEvent->modifiers())).arg(AEvent->text()).arg(FMode));

	// the focus may stay on the viewer after ":" has opened the prompt on
	// the editor: any non-normal mode leaves to NORMAL from here
	if (AEvent->key()==Qt::Key_Escape && FMode!=NormalMode)
	{
		AEvent->accept();
		setApplicationMode(NormalMode);
		return true;
	}

	// numeric prefixes (5j, 3G)
	if (FMode==NormalMode && Options::node(OPV_VIM_COUNTS).value().toBool())
	{
		const QString text = AEvent->text();
		if (text.length()==1 && text.at(0).isDigit() && text.at(0)!='0'
			&& !(AEvent->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier)))
		{
			FKeyBuffer.clear();
			if (FCount < 0)
				FCount = 0;
			FCount = FCount*10 + text.at(0).digitValue();
			return true;
		}
	}

	QList<QKeyCombination> buffer = FKeyBuffer + eventToCombinations(AEvent);
	const int action = resolveAction(FMode, buffer);
	if (action >= 0)
	{
		const int count = FCount < 1 ? 1 : FCount;
		FKeyBuffer.clear();
		FCount = -1;
		return executeAction(FMode, VimActionId(action), count);
	}
	if (actionHasPartialMatch(FMode, buffer))
	{
		FKeyBuffer = buffer;
		return true;
	}
	FKeyBuffer.clear();
	FCount = -1;
	// the vacuum main window closes (hides) itself on Escape, so in the chat
	// every unhandled Escape is swallowed to keep the vim mode switching sane
	if (AEvent->key()==Qt::Key_Escape)
	{
		AEvent->accept();
		return true;
	}
	// unhandled keys stay with the viewer (page scrolling etc)
	return false;
}

bool VimPlugin::processRosterKey(QKeyEvent *AEvent)
{
	if (AEvent->key()==Qt::Key_Escape)
	{
		// vim often ends the roster navigation with Escape: move the focus
		// back to the chat window, do not hide the whole application window
		IMessageChatWindow *window = activeChatWindow();
		if (window!=NULL && chatViewer(window)!=NULL)
		{
			chatViewer(window)->setFocus();
			updateCursor(window);
			return true;
		}
		return false;	// no chat window - keep the default vacuum Escape
	}

	QList<QKeyCombination> buffer = FKeyBuffer + eventToCombinations(AEvent);
	const int action = resolveAction(FMode, buffer);
	const int count = FCount < 1 ? 1 : FCount;
	FKeyBuffer.clear();
	FCount = -1;
	if (action == ActionScrollDown || action == ActionScrollUp)
	{
		if (FRostersViewPlugin!=NULL)
		{
			QTreeView *view = FRostersViewPlugin->rostersView()->instance();
			if (view!=NULL && view->selectionModel()!=NULL)
			{
				QModelIndex current = view->selectionModel()->currentIndex();
				QModelIndex index = current;
				for (int i=0; i<count && index.isValid(); ++i)
					index = action==ActionScrollDown ? view->indexBelow(index) : view->indexAbove(index);
				if (index.isValid())
				{
					view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
					view->scrollTo(index, QAbstractItemView::EnsureVisible);
				}
			}
			return true;
		}
	}
	if (action == ActionOpenNotification && FMode==NormalMode)
		return executeAction(FMode, ActionOpenNotification, count);
	if (action >= 0 && FMode==NormalMode)
	{
		// roster context supports the chat actions guard: the actions that
		// operate on the chat view are not resolved here, focus the chat
		// window first in the user's own way
		return false;
	}
	if (actionHasPartialMatch(FMode, buffer))
	{
		FKeyBuffer = buffer;
		return true;
	}
	return false;
}

bool VimPlugin::processEditorKey(QKeyEvent *AEvent)
{
	const bool isEnter = AEvent->key()==Qt::Key_Return || AEvent->key()==Qt::Key_Enter;
	const bool isEscape = AEvent->key()==Qt::Key_Escape;

	if (FMode==CommandMode)
	{
		if (isEnter)
		{
			AEvent->accept();
			executeCommand();
			return true;
		}
		if (isEscape)
		{
			AEvent->accept();
			setApplicationMode(NormalMode);
			return true;
		}
		return false;	// typing into the command line
	}
	if (FMode==SearchPromptMode)
	{
		if (isEnter)
		{
			AEvent->accept();
			doSearchNext(false);
			return true;
		}
		if (isEscape)
		{
			AEvent->accept();
			setApplicationMode(NormalMode);
			return true;
		}
		return false;
	}
	if (FMode==InsertMode)
	{
		// the chat is a chat: in INSERT everything is sent to the contact,
		// even if the text looks like a vim command; only the explicit
		// ':' from NORMAL/VISUAL switches to the COMMAND mode
		if (isEscape)
		{
			AEvent->accept();
			setApplicationMode(NormalMode);
			return true;
		}
		return false;
	}

	// the editor is disabled in NORMAL/VISUAL, swallow accidental keystrokes
	AEvent->accept();
	return true;
}

// ============ actions ============
bool VimPlugin::executeAction(VimMode AMode, VimActionId AAction, int ACount)
{
	IMessageChatWindow *window = contextWindow(QApplication::focusWidget());
	QTextBrowser *viewer = window!=NULL ? chatViewer(window) : NULL;

	// NORMAL shows no cursor and the native scrolling keys do not move it:
	// every action that works on the text cursor starts from the visible area
	// instead of a stale position that may be far above or below
	if (AMode==NormalMode && viewer!=NULL
		&& !viewer->viewport()->rect().intersects(viewer->cursorRect(viewer->textCursor())))
	{
		QTextCursor cursor = viewer->cursorForPosition(QPoint(viewer->viewport()->width()/2, viewer->viewport()->height()/2));
		cursor = normalizeCursor(cursor);
		cursor.clearSelection();
		viewer->setTextCursor(cursor);
	}

	switch (AAction)
	{
	case ActionScrollDown:
	case ActionScrollUp:
		{
			const bool visual = (AMode==VisualMode);
			const int blocks = qMax(1,ACount);
			if (viewer==NULL)
				return true;
			if (!visual)
			{
				// NORMAL scrolls the chat exactly like the native cursor keys:
				// the Down/Up press is forwarded to the text browser and Qt
				// does the scrolling, so holding the key repeats like holding
				// the arrow key; a count is sent as that many presses
				const Qt::Key key = ActionScrollDown==AAction ? Qt::Key_Down : Qt::Key_Up;
				for (int i=0; i<blocks; ++i)
				{
					QKeyEvent forward(QEvent::KeyPress, key, Qt::NoModifier, QString(), false);
					FForwardingKey = true;
					QApplication::sendEvent(viewer, &forward);
					FForwardingKey = false;
				}
			}
			else
			{
				// vim moves line by line: the document is a set of table cells,
				// QTextCursor::NextBlock stays inside a cell (a one character
				// step), Down/Up move over the rows of the messages
				QTextCursor cursor = viewer->textCursor();
				cursor.movePosition(ActionScrollDown==AAction ? QTextCursor::Down : QTextCursor::Up,
					QTextCursor::KeepAnchor, blocks);
				viewer->setTextCursor(cursor);
				updateCursor(window, true);
			}
		}
		return true;

	case ActionScrollMessageUp:
	case ActionScrollMessageDown:
		{
			if (viewer==NULL)
				return true;
			const bool previous = (AAction==ActionScrollMessageUp);
			// scroll the viewport message by message, the same way the message
			// window shortcuts (Alt+Up/Alt+Down) do: every message is rendered
			// in its own QTextFrame and the frame boundaries are the targets
			QScrollBar *scroll = viewer->verticalScrollBar();
			const int scrollTop = scroll!=NULL ? scroll->value() : 0;
			int targetPosition = -1;
			const QTextCursor target = vimMessageScrollTarget(viewer, previous, targetPosition);
			// the vim cursor follows the message that will be shown at the top;
			// it is placed BEFORE the scroll, because setTextCursor runs
			// ensureCursorVisible and would drag the viewport back
			if (!target.isNull())
			{
				QTextCursor cursor = visibleCharacterCursor(target, messageMarkerRanges(viewer->document()));
				if (AMode==VisualMode)
				{
					const int anchor = viewer->textCursor().anchor();
					QTextCursor selection(cursor);
					selection.setPosition(anchor, QTextCursor::MoveAnchor);
					selection.setPosition(cursor.position(), QTextCursor::KeepAnchor);
					cursor = selection;
				}
				else
					cursor.clearSelection();
				viewer->setTextCursor(cursor);
			}
			updateCursor(window, false);
			if (scroll!=NULL && targetPosition>=0)
				scroll->setValue(targetPosition);
			const int scrollAfter = scroll!=NULL ? scroll->value() : -1;
			const QTextCursor vimCursor = viewer->textCursor();
			LOG_INFO(QString("[VimPlugin] %1 scroll: %2 -> %3 (target=%4), max=%5, cursor=%6/%7 sel=%8")
				.arg(previous ? "{" : "}")
				.arg(scrollTop).arg(scrollAfter).arg(targetPosition)
				.arg(scroll!=NULL ? scroll->maximum() : -1)
				.arg(vimCursor.position()).arg(vimCursor.anchor())
				.arg(vimCursor.hasSelection() ? 1 : 0));
			if (AMode!=VisualMode && scroll!=NULL && scroll->value()==scrollTop)
				QApplication::beep();	// nothing to move further
		}
		return true;
	case ActionFocusRoster:
		if (FRostersViewPlugin!=NULL)
		{
			QWidget *rosterWidget = FRostersViewPlugin->rostersView()->instance();
			if (rosterWidget!=NULL)
				rosterWidget->setFocus();
		}
		return true;
	case ActionGotoFirstLine:
	case ActionGotoLastLine:
		{
			if (viewer==NULL)
				return true;
			const bool visual = (AMode==VisualMode);
			QTextCursor cursor = viewer->textCursor();
			cursor.movePosition(ActionGotoFirstLine==AAction ? QTextCursor::Start : QTextCursor::End,
				visual ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
			viewer->setTextCursor(normalizeCursor(cursor));
			viewer->ensureCursorVisible();
			updateCursor(window, false);
		}
		return true;
	case ActionCopyMessage:
		doCopyBlock(window, true);
		return true;
	case ActionCopyBlock:
		doCopyBlock(window, false);
		return true;
	case ActionEnterVisual:
		// the cursor was placed on the visible area by the common NORMAL
		// guard above, the selection starts there
		setApplicationMode(VisualMode);
		return true;
	case ActionEnterInsert:
		doEnterInsert(false);
		return true;
	case ActionEditLastMessage:
		doEnterInsert(true);
		return true;
	case ActionPasteClipboard:
		doPasteClipboard();
		return true;
	case ActionOpenNotification:
		// the same path as the global "show received message" hotkey, so
		// both always open the same notification
		Shortcuts::activateShortcut(SCT_GLOBAL_ACTIVATELASTNOTIFICATION, NULL);
		return true;
	case ActionEnterCommand:
		doEnterCommand();
		return true;
	case ActionNextSearchMatch:
		doSearchNext(false);
		return true;
	case ActionPrevSearchMatch:
		doSearchNext(true);
		return true;
	case ActionVisualCopy:
		{
			if (viewer==NULL)
				return true;
			QTextCursor cursor = viewer->textCursor();
			if (cursor.hasSelection())
			{
				QApplication::clipboard()->setText(cursor.selectedText().replace(QChar(0x2029), '\n'));
				setApplicationMode(NormalMode);
			}
			return true;
		}
	default:
		return false;
	}
}

QList< QPair<int,int> > VimPlugin::messageMarkerRanges(QTextDocument *ADocument) const
{
	// [start,end) ranges of every message bound by the VStack markers
	QList< QPair<int,int> > ranges;
	QList<int> markers;
	QString text = ADocument->toPlainText();
	int pos = 0;
	while ((pos = text.indexOf(VIM_MESSAGE_MARKER, pos)) >= 0)
	{
		markers.append(pos);
		pos += 1;
	}
	for (int i=0; i<markers.count(); ++i)
	{
		const int start = markers.at(i)+1;
		const int end = (i+1 < markers.count()) ? markers.at(i+1) : ADocument->characterCount();
		ranges.append(qMakePair(start, end));
	}
	return ranges;
}

void VimPlugin::doCopyBlock(IMessageChatWindow *AWindow, bool AMessage)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	if (viewer==NULL)
		return;
	QTextCursor cursor = viewer->textCursor();
	QString text;
	if (AMessage)
	{
		const QList< QPair<int,int> > ranges = messageMarkerRanges(viewer->document());
		const int cursorPos = cursor.position();
		for (int i=0; i<ranges.count(); ++i)
		{
			const QPair<int,int> &range = ranges.at(i);
			if (cursorPos >= range.first && cursorPos <= range.second)
			{
				QTextCursor messageCursor(cursor);
				messageCursor.setPosition(range.first);
				messageCursor.setPosition(range.second, QTextCursor::KeepAnchor);
				text = messageCursor.selectedText();
				text.replace(QChar(0x2029), '\n');
				break;
			}
		}
	}
	else
	{
		cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor);
		cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
		text = cursor.selectedText();
	}
	if (!text.isEmpty())
		QApplication::clipboard()->setText(text);
}

void VimPlugin::doSearchPrompt()
{
	if (FMode != NormalMode)
		return;
	IMessageChatWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
		return;
	QTextEdit *textEdit2 = editor->textEdit();
	editor->document()->setPlainText("/");
	if (textEdit2!=NULL)
	{
		QTextCursor cursor = textEdit2->textCursor();
		cursor.movePosition(QTextCursor::End);
		textEdit2->setTextCursor(cursor);
	}
	setApplicationMode(SearchPromptMode);
}

void VimPlugin::doSearchNext(bool ABackward)
{
	IMessageChatWindow *window = contextWindow(QApplication::focusWidget());
	if (window==NULL)
		window = activeChatWindow();
	QTextBrowser *viewer = window!=NULL ? chatViewer(window) : NULL;
	if (viewer==NULL)
		return;

	IMessageEditWidget *editor = chatEditor(window);
	if (editor!=NULL)
	{
		QString prompt = editor->document()->toPlainText().trimmed();
		if (prompt.startsWith('/'))
			prompt = prompt.mid(1);
		if (!prompt.isEmpty())
			FSearchString = prompt;
		editor->document()->setPlainText(QString());
	}

	if (FSearchString.isEmpty())
	{
		setApplicationMode(NormalMode);
		return;
	}

	QTextCursor cursor = viewer->textCursor();
	QTextCursor found = viewer->document()->find(FSearchString, cursor,
		ABackward ? QTextDocument::FindBackward : QTextDocument::FindFlag(0));
	if (found.isNull())
	{
		QTextCursor wrap = viewer->textCursor();
		wrap.movePosition(ABackward ? QTextCursor::End : QTextCursor::Start);
		found = viewer->document()->find(FSearchString, wrap,
			ABackward ? QTextDocument::FindBackward : QTextDocument::FindFlag(0));
	}
	if (!found.isNull())
	{
		viewer->setTextCursor(found);
		updateCursor(window);
		setApplicationMode(NormalMode);
	}
	else
	{
		LOG_INFO(QString("[VimPlugin] search '%1' not found").arg(FSearchString));
		QApplication::beep();
		setApplicationMode(NormalMode);
	}
}

void VimPlugin::doEnterInsert(bool AEditLast)
{
	IMessageChatWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
		return;
	setApplicationMode(InsertMode);
	QTextEdit *textEdit = editor->textEdit();
	if (textEdit!=NULL && window!=NULL && window->isActiveTabPage())
	{
		if (AEditLast)
		{
			// clear the editor and trigger the "Up on the empty editor"
			// behavior of the MessageCorrect plugin to start the editing of
			// the last sent message
			editor->document()->setPlainText(QString());
			textEdit->setFocus();
			QCoreApplication::postEvent(textEdit, new QKeyEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier));
		}
		else
		{
					QTextCursor cursor = textEdit->textCursor();
			cursor.movePosition(QTextCursor::End);
			textEdit->setTextCursor(cursor);
			textEdit->setFocus();
		}
	}
}

void VimPlugin::doPasteClipboard()
{
	IMessageChatWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
		return;
	setApplicationMode(InsertMode);
	QTextEdit *textEdit = editor->textEdit();
	if (textEdit==NULL || window==NULL || !window->isActiveTabPage())
		return;
	// vim p: the text is inserted at the cursor (the insert mode entry puts
	// it at the end of the input box) and the user continues in INSERT
	QString text = QApplication::clipboard()->text();
	text.remove(VIM_MESSAGE_MARKER);
	text.replace(QChar(0x2029), '\n');
	QTextCursor cursor = textEdit->textCursor();
	cursor.movePosition(QTextCursor::End);
	textEdit->setTextCursor(cursor);
	if (!text.isEmpty())
		textEdit->insertPlainText(text);
	textEdit->setFocus();
}

void VimPlugin::doEnterCommand()
{
	IMessageChatWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
		return;
	QTextEdit *textEdit2 = editor->textEdit();
	editor->document()->setPlainText(":");
	if (textEdit2!=NULL)
	{
		QTextCursor cursor = textEdit2->textCursor();
		cursor.movePosition(QTextCursor::End);
		textEdit2->setTextCursor(cursor);
	}
	setApplicationMode(CommandMode);
}

void VimPlugin::applyPresenceShow(int AShow, const QString &AStatusText)
{
	if (FPresenceManager==NULL)
	{
		LOG_WARNING("[VimPlugin] presence manager is not available");
		return;
	}
	foreach (IPresence *presence, FPresenceManager->presences())
		if (FPresenceManager->isPresenceActive(presence))
			presence->setPresence(AShow, AStatusText, presence->priority());
}

void VimPlugin::executeCommand()
{
	IMessageChatWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
	{
		setApplicationMode(NormalMode);
		return;
	}

	QString command = editor->document()->toPlainText().trimmed();
	if (command.startsWith(':'))
		command = command.mid(1);
	editor->document()->setPlainText(QString());

	const QString name = command.section(' ', 0, 0).toLower();
	const QString args = command.section(' ', 1).trimmed();
	LOG_INFO(QString("[VimPlugin] command line executed: '%1'").arg(command));

	if (name.isEmpty())
	{
		setApplicationMode(NormalMode);
		return;
	}

	if (name=="quit" || name=="q")
	{
		if (FPluginManager!=NULL)
			FPluginManager->quit();
		return;	// the application is going down
	}
	else if (name=="hide")
	{
		// vim :hide - hide the buffer, here: hide the main window to the
		// tray, the application keeps running (unlike :quit)
		closeMainWindow();
	}
	else if (name=="available" || name=="a" || name=="online")
		applyPresenceShow(IPresence::Online, args);
	else if (name=="notavailable" || name=="na")
		applyPresenceShow(IPresence::ExtendedAway, args);
	else if (name=="offline" || name=="off")
		applyPresenceShow(IPresence::Offline, args);
	else if (name=="away")
		applyPresenceShow(IPresence::Away, args);
	else if (name=="dnd")
		applyPresenceShow(IPresence::DoNotDisturb, args);
	else if (name=="status")
	{
		const QString show = args.section(' ', 0, 0).toLower();
		const QString statusText = args.section(' ', 1).trimmed();
		if (show=="online") applyPresenceShow(IPresence::Online, statusText);
		else if (show=="away") applyPresenceShow(IPresence::Away, statusText);
		else if (show=="xa") applyPresenceShow(IPresence::ExtendedAway, statusText);
		else if (show=="dnd") applyPresenceShow(IPresence::DoNotDisturb, statusText);
		else if (show=="offline" || show=="off") applyPresenceShow(IPresence::Offline, statusText);
		else QApplication::beep();
	}
	else if (name=="send" || name=="w")
	{
		if (!args.isEmpty())
		{
			editor->document()->setPlainText(args);
			editor->sendMessage();
		}
	}
	else if (name=="clear")
	{
		if (window!=NULL && window->viewWidget()!=NULL)
			window->viewWidget()->clearContent();
	}
	else if (name=="close" || name=="tabclose")
	{
		if (window!=NULL)
			window->closeTabPage();
	}
	else if (name=="chat")
	{
		const QString target = args.section(' ', 0, 0);
		const QString body = args.section(' ', 1).trimmed();
		if (!target.isEmpty() && FMessageWidgets!=NULL)
		{
			Jid streamJid;
			if (FPresenceManager!=NULL && !FPresenceManager->presences().isEmpty())
				streamJid = FPresenceManager->presences().first()->streamJid();
			IMessageChatWindow *chat = FMessageWidgets->getChatWindow(streamJid, Jid(target));
			if (chat!=NULL)
			{
				chat->showTabPage();
				if (!body.isEmpty())
				{
					chat->editWidget()->document()->setPlainText(body);
					chat->editWidget()->sendMessage();
				}
				applyModeToWindow(chat);
			}
		}
	}
	else if (name=="join")
	{
		IPlugin *plugin = FPluginManager!=NULL ? FPluginManager->pluginInstance(QUuid(MULTIUSERCHAT_UUID)) : NULL;
		IMultiUserChatManager *manager = qobject_cast<IMultiUserChatManager *>(plugin!=NULL ? plugin->instance() : NULL);
		QString room = args.section(' ', 0, 0);
		QString nick = args.section(' ', 1, 1);
		QString password = args.section(' ', 2).trimmed();
		if (manager!=NULL && !room.isEmpty() && FPresenceManager!=NULL && !FPresenceManager->presences().isEmpty())
		{
			Jid streamJid = FPresenceManager->presences().first()->streamJid();
			manager->getMultiChatWindow(streamJid, Jid(room), nick, password);
		}
		else
		{
			QApplication::beep();
			LOG_INFO("[VimPlugin] :join failed, MUC support or online stream is not available");
		}
	}
	else
	{
		LOG_INFO(QString("[VimPlugin] unknown command '%1'").arg(command));
		QApplication::beep();
	}
	setApplicationMode(NormalMode);
}

void VimPlugin::onOptionsOpened()
{
	// The options may be opened after this plugin has started (the options
	// manager loads the profile during its own start), see startPlugin
	loadKeymap();
	loadGlobalShortcuts();
}

void VimPlugin::onOptionsClosed()
{
	loadKeymap();
}

