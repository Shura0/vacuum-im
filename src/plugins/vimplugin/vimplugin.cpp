#include "vimplugin.h"

#include "vimoptionswidget.h"
#include "vimhelpdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QScrollBar>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTimer>

#include <definitions/menuicons.h>
#include <definitions/optionvalues.h>
#include <definitions/shortcuts.h>
#include <utils/logger.h>
#include <utils/options.h>
#include <utils/jid.h>
#include <utils/shortcuts.h>
#include <utils/widgetmanager.h>

// invisible zero-width chars used to bind the message body in the viewer
// document: gives "message ranges" for vim motions (`{`/`}`) and for the
// vim copy commands.  START is placed before the message text, END before
// the marks slot (receipts/encryption/correction glyphs), so copying a range
// never includes timestamps, sender/time cells or service marks.
#define VIM_MESSAGE_START       QChar(0x2060)
#define VIM_MESSAGE_END         QChar(0x2061)
// block property that carries the sender name of the message
#define VIM_SENDER_PROPERTY     (QTextFormat::UserProperty + 0x5649)
// block property that carries the message direction (incoming/outgoing):
// the sender name of an outgoing private message is the user's own name, so
// the "interlocutor" copies need an incoming one
#define VIM_DIRECTION_PROPERTY  (QTextFormat::UserProperty + 0x564A)

static bool isVimMessageMarker(QChar AChar)
{
	return AChar==VIM_MESSAGE_START || AChar==VIM_MESSAGE_END;
}

// the style options carry the sender already HTML-escaped (see the message
// handlers), but the clipboard and findUser() need the plain name
static QString plainSenderName(const QString &AName)
{
	if (AName.isEmpty())
		return QString();
	return QTextDocumentFragment::fromHtml(AName).toPlainText().trimmed();
}

// the double-Escape gesture window: the vim modes swallow the single Escape
// in the chat windows and in the main window, two presses inside this
// interval close the main window like the plain Esc does without the plugin
#define VIM_DOUBLE_ESCAPE_MSECS  1000
// a copy leaves the viewer unchanged, so the yank is confirmed by a short
// highlight of the characters that went to the clipboard
#define VIM_COPY_FLASH_MSECS     300

static const QColor kColorNormal  = QColor(0xB4,0xB4,0xB4);	// grey
static const QColor kColorCaret   = QColor(0xD8,0xCF,0xF0);	// light lavender
static const QColor kColorVisual  = QColor(0xA8,0xDC,0xF0);	// light blue
static const QColor kColorCommand = QColor(0xFF,0xD2,0x8A);	// orange
static const QColor kColorSearch  = QColor(0xC0,0xF0,0xC0);	// light green
static const QColor kColorCursor  = QColor(0x64,0x82,0xFF);	// block cursor
// the copy flash reuses the cursor blue, translucent so the text stays readable
static const QColor kColorFlash   = QColor(0x64,0x82,0xFF,0x70);

static QString stylesheetForMode(VimMode AMode)
{
	switch (AMode)
	{
	case NormalMode:      return QStringLiteral("background-color:%1;").arg(kColorNormal.name());
	case CaretMode:       return QStringLiteral("background-color:%1;").arg(kColorCaret.name());
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
	{ ActionMoveLeft,          "h;Left",   "h;Left" },
	{ ActionMoveRight,         "l;Right",  "l;Right" },
	{ ActionScrollMessageUp,   "{",        "{"      },
	{ ActionScrollMessageDown, "}",        "}"      },
	{ ActionSearchForward,     "/",        ""       },
	{ ActionFocusRoster,       "gd",       ""       },
	{ ActionGotoFirstLine,     "gg",       "gg"     },
	{ ActionGotoLastLine,      "G",        "G"      },
	{ ActionCopyMessage,       "yb",       ""       },
	{ ActionCopyBlock,         "yy",       ""       },
	{ ActionEnterVisual,       "v",        "v"      },
	{ ActionEnterInsert,       "i",        ""       },
	{ ActionEditLastMessage,   "c",        ""       },
	{ ActionEnterCommand,      ":",        ""       },
	{ ActionNextSearchMatch,   "n",        ""       },
	{ ActionPrevSearchMatch,   "N",        ""       },
	{ ActionVisualCopy,        "",         "y"      },
	{ ActionPasteClipboard,    "p",        ""       },
	{ ActionPasteQuotation,    "P",        ""       },
	{ ActionOpenNotification,  "gn",       ""       },
	{ ActionReselectVisual,    "gv",       ""       },
	{ ActionSwapSelectionEnds, "",         "o"      },
	{ ActionEnterVisualLine,   "V",        "V"      },
	{ ActionCopyNick,          "yn",       ""       },
	{ ActionCopyJid,           "yj",       ""       }
};

// a linewise VISUAL selection always covers whole displayed lines: the fixed
// end of the selection becomes a line boundary (the end of its line when the
// other end is above, the start of its line when below) and the moving end
// covers the whole line it is on
static QTextCursor linewiseCursor(QTextDocument *ADocument, int AAnchor, int AMoving)
{
	if (ADocument == NULL)
		return QTextCursor();
	QTextCursor anchor(ADocument);
	anchor.setPosition(AAnchor);
	QTextCursor moving(ADocument);
	moving.setPosition(AMoving);
	if (AMoving < AAnchor)
	{
		anchor.movePosition(QTextCursor::EndOfLine);
		moving.movePosition(QTextCursor::StartOfLine);
	}
	else
	{
		anchor.movePosition(QTextCursor::StartOfLine);
		moving.movePosition(QTextCursor::EndOfLine);
	}
	QTextCursor cursor(ADocument);
	cursor.setPosition(anchor.position());
	cursor.setPosition(moving.position(), QTextCursor::KeepAnchor);
	return cursor;
}

// vim cursor may land on the invisible message bounds markers, skip them
// so the drawn block cursor covers a real (visible) character
static QTextCursor normalizeCursor(QTextCursor ACursor)
{
	QTextDocument *document = ACursor.document();
	while (document!=NULL && isVimMessageMarker(document->characterAt(ACursor.position()))
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
	case ActionMoveLeft:          return tr("Move cursor left");
	case ActionMoveRight:         return tr("Move cursor right");
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
	case ActionPasteQuotation:    return tr("Paste clipboard as quotation and enter insert mode");
	case ActionOpenNotification:  return tr("Open last notification");
	case ActionReselectVisual:    return tr("Reselect the last visual selection");
	case ActionSwapSelectionEnds: return tr("Move to the other end of the selection");
	case ActionEnterVisualLine:   return tr("Enter linewise visual mode");
	case ActionCopyNick:          return tr("Copy interlocutor nick");
	case ActionCopyJid:           return tr("Copy interlocutor JID/address");
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

// The actions bound in each mode.  Shared by the key matching and by the
// ":help" page so the two lists cannot drift apart; INSERT and the prompt
// modes have no bindings at all (they type into the editor).
static QList<VimActionId> vimModeActions(VimMode AMode)
{
	static const QList<VimActionId> normalActions = QList<VimActionId>()
		<< ActionScrollUp << ActionScrollDown << ActionMoveLeft << ActionMoveRight
		<< ActionScrollMessageUp << ActionScrollMessageDown
		<< ActionSearchForward << ActionFocusRoster << ActionGotoFirstLine << ActionGotoLastLine
		<< ActionCopyMessage << ActionCopyBlock << ActionEnterVisual << ActionEnterInsert
		<< ActionEditLastMessage << ActionEnterCommand << ActionNextSearchMatch << ActionPrevSearchMatch
		<< ActionPasteClipboard << ActionPasteQuotation << ActionOpenNotification
		<< ActionReselectVisual << ActionEnterVisualLine
		<< ActionCopyNick << ActionCopyJid;
	static const QList<VimActionId> visualActions = QList<VimActionId>()
		<< ActionScrollUp << ActionScrollDown << ActionMoveLeft << ActionMoveRight
		<< ActionScrollMessageUp << ActionScrollMessageDown
		<< ActionGotoFirstLine << ActionGotoLastLine << ActionVisualCopy
		<< ActionSwapSelectionEnds << ActionEnterVisual << ActionEnterVisualLine;
	// CARET moves the cursor without extending a selection; 'v' turns the
	// current position into the anchor of a VISUAL selection.  The copy
	// commands work on the caret like they do on the NORMAL cursor.
	static const QList<VimActionId> caretActions = QList<VimActionId>()
		<< ActionScrollUp << ActionScrollDown << ActionMoveLeft << ActionMoveRight
		<< ActionScrollMessageUp << ActionScrollMessageDown
		<< ActionGotoFirstLine << ActionGotoLastLine
		<< ActionCopyMessage << ActionCopyBlock << ActionEnterVisual << ActionEnterVisualLine
		<< ActionCopyNick << ActionCopyJid;

	switch (AMode)
	{
	case VisualMode: return visualActions;
	case CaretMode:  return caretActions;
	default:         return normalActions;
	}
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

	if (vimModeActions(AMode).contains(AAction))
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
	if (FHelpDialog)
		delete FHelpDialog;
}

void VimPlugin::pluginInfo(IPluginInfo *APluginInfo)
{
	APluginInfo->name = tr("Vim mode");
	APluginInfo->description = tr("Vim-like keyboard modes for chats, conference rooms and the contact list (NORMAL, CARET, INSERT, VISUAL, COMMAND). Double Escape closes the main window. Conflicts with the Shortcut Manager plugin.");
	APluginInfo->version = "1.0";
	APluginInfo->author = "Vacuum-IM";
	APluginInfo->dependences.append(QUuid(MESSAGEWIDGETS_UUID));
	// conference rooms come from the multiuserchat plugin and vim supports
	// them next to the private chat windows
	APluginInfo->dependences.append(QUuid(MULTIUSERCHAT_UUID));
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

	// optional: conference (MUC) windows live in their own manager and are
	// not part of IMessageWidgets::chatWindows()
	plugin = APluginManager->pluginInterface("IMultiUserChatManager").value(0,NULL);
	if (plugin)
		FMultiChatManager = qobject_cast<IMultiUserChatManager *>(plugin->instance());

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
		IOptionsDialogNode dnode = { ONO_VIM, OPN_VIM, MNI_VIM, tr("Vim mode") };
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
	}
	if (FMultiChatManager)
	{
		connect(FMultiChatManager->instance(), SIGNAL(multiChatWindowCreated(IMultiUserChatWindow *)),
			SLOT(onMultiChatWindowCreated(IMultiUserChatWindow *)));
		connect(FMultiChatManager->instance(), SIGNAL(multiChatWindowDestroyed(IMultiUserChatWindow *)),
			SLOT(onMultiChatWindowDestroyed(IMultiUserChatWindow *)));
	}
	foreach (IMessageWindow *window, messageWindows())
		installWindow(window);
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
	{
		// Keep the marks slot outside of the copyable body: it contains the
		// read/delivered/corrected/encrypted service glyphs.
		static const QString marksSlot = QStringLiteral("%contentSlot:marks%");
		const int marksPos = AHtml.indexOf(marksSlot);
		if (marksPos >= 0)
		{
			AModifiedHtml = QStringLiteral("<span>") + VIM_MESSAGE_START + QStringLiteral("</span>")
				+ AHtml.left(marksPos)
				+ QStringLiteral("<span>") + VIM_MESSAGE_END + QStringLiteral("</span>")
				+ AHtml.mid(marksPos);
		}
		else
		{
			AModifiedHtml = QStringLiteral("<span>") + VIM_MESSAGE_START + QStringLiteral("</span>")
				+ AHtml
				+ QStringLiteral("<span>") + VIM_MESSAGE_END + QStringLiteral("</span>");
		}
	}
	return true;
}

QString VimPlugin::messageStyleContentSlot(int AOrder, QWidget *AWidget, const QString &ASlotName, bool ASlotInTemplate, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AOrder); Q_UNUSED(AWidget); Q_UNUSED(ASlotName); Q_UNUSED(ASlotInTemplate); Q_UNUSED(AOptions);
	return QString();
}

void VimPlugin::installEditorFilter(IMessageWindow *AWindow)
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
	installWindow(AWindow);
}

void VimPlugin::onChatWindowDestroyed(IMessageChatWindow *AWindow)
{
	uninstallWindow(AWindow);
}

// conference windows are a parallel kind of message window, created and
// destroyed by the multiuserchat manager
void VimPlugin::onMultiChatWindowCreated(IMultiUserChatWindow *AWindow)
{
	installWindow(AWindow);
}

void VimPlugin::onMultiChatWindowDestroyed(IMultiUserChatWindow *AWindow)
{
	uninstallWindow(AWindow);
}

void VimPlugin::installWindow(IMessageWindow *AWindow)
{
	if (AWindow == NULL)
		return;
	installEditorFilter(AWindow);
	if (AWindow->viewWidget() != NULL)
	{
		// the document is reloaded by clearContent() and the style widgets
		// are replaced when the style changes - re-apply the mode colors
		// and the cursor on every such change
		connect(AWindow->viewWidget()->instance(), SIGNAL(contentAppended(const QString &, const IMessageStyleContentOptions &)),
			SLOT(onWindowContentAppended(const QString &, const IMessageStyleContentOptions &)), Qt::UniqueConnection);
		connect(AWindow->viewWidget()->instance(), SIGNAL(messageStyleChanged(IMessageStyle *,const IMessageStyleOptions &)),
			SLOT(onWindowStyleChanged()), Qt::UniqueConnection);
		LOG_INFO(QString("[VimPlugin] message window opened, viewer class=%1, markers=%2")
			.arg(chatViewer(AWindow)!=NULL ? QString::fromLatin1(chatViewer(AWindow)->metaObject()->className()) : QString("null"))
			.arg(chatViewer(AWindow)!=NULL ? messageMarkerRanges(chatViewer(AWindow)->document()).count() : 0));
	}
	// both ChatWindow and MultiUserChatWindow focus their editor when the
	// page is activated, but in the viewer modes the editor is disabled and
	// vim owns the viewer: put the focus there like setApplicationMode does
	// on a mode switch
	connect(AWindow->instance(), SIGNAL(tabPageActivated()),
		SLOT(onWindowTabPageActivated()), Qt::UniqueConnection);
	applyModeToWindow(AWindow);
}

void VimPlugin::uninstallWindow(IMessageWindow *AWindow)
{
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (editor != NULL)
		FEditorWidgets.removeAll(editor->textEdit());
}

// any append (new message, history load) invalidates the document of the
// viewer, refresh the vim cursor and the mode colors
void VimPlugin::onWindowContentAppended(const QString &AHtml, const IMessageStyleContentOptions &AOptions)
{
	Q_UNUSED(AHtml);
	IMessageWindow *window = NULL;
	foreach (IMessageWindow *win, messageWindows())
		if (win->viewWidget()!=NULL && win->viewWidget()->instance()==sender())
			window = win;

	if (window!=NULL && AOptions.kind==IMessageStyleContentOptions::KindMessage)
	{
		QTextBrowser *viewer = chatViewer(window);
		if (viewer != NULL)
		{
			QTextDocument *document = viewer->document();
			const QString plainText = document->toPlainText();
			const int markerPos = plainText.lastIndexOf(VIM_MESSAGE_START);
			if (markerPos >= 0)
			{
				// Consecutive messages use the NextContent template without a
				// sender field; inherit the previous sender so copied text
				// still has a nickname.
				QString sender = AOptions.senderName;
				if (sender.isEmpty())
				{
					const int previousPos = markerPos>0 ? plainText.lastIndexOf(VIM_MESSAGE_START,markerPos-1) : -1;
					if (previousPos >= 0)
						sender = document->findBlock(previousPos).blockFormat().property(VIM_SENDER_PROPERTY).toString();
				}
				QTextBlock block = document->findBlock(markerPos);
				QTextBlockFormat format = block.blockFormat();
				format.setProperty(VIM_SENDER_PROPERTY,sender);
				format.setProperty(VIM_DIRECTION_PROPERTY,AOptions.direction);
				QTextCursor cursor(block);
				cursor.setBlockFormat(format);
			}
		}
	}

	placeCursorAtLastMessage(window);
	applyModeToWindow(window);
}

void VimPlugin::onWindowStyleChanged()
{
	IMessageWindow *window = NULL;
	foreach (IMessageWindow *win, messageWindows())
		if (win->viewWidget()!=NULL && win->viewWidget()->instance()==sender())
			window = win;
	placeCursorAtLastMessage(window);
	applyModeToWindow(window);
}

void VimPlugin::onWindowTabPageActivated()
{
	IMessageWindow *window = contextWindow(qobject_cast<QWidget *>(sender()));
	if (window != NULL)
		focusModeWidget(window);
}

// place the vim cursor on the first character of the last (newest) message,
// anchor of the transparent '- like in vim G' behaviour after the document
// was reset or grown at the bottom
static QTextCursor visibleCharacterCursor(QTextCursor ACursor, const QList<QPair<int,int> > &ARanges);

void VimPlugin::placeCursorAtLastMessage(IMessageWindow *AWindow)
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
// all message windows vim works with: private chats are IMessageChatWindow,
// conference rooms are IMultiUserChatWindow (a different interface tree)
QList<IMessageWindow *> VimPlugin::messageWindows() const
{
	QList<IMessageWindow *> windows;
	if (FMessageWidgets != NULL)
		foreach (IMessageChatWindow *window, FMessageWidgets->chatWindows())
			windows.append(window);
	if (FMultiChatManager != NULL)
		foreach (IMultiUserChatWindow *window, FMultiChatManager->multiChatWindows())
			windows.append(window);
	return windows;
}

IMessageWindow *VimPlugin::contextWindow(QWidget *AFocusWidget) const
{
	if (FMessageWidgets==NULL || AFocusWidget==NULL)
		return NULL;
	foreach (IMessageWindow *window, messageWindows())
		if (window->instance()->isAncestorOf(AFocusWidget))
			return window;
	return NULL;
}

IMessageWindow *VimPlugin::activeChatWindow() const
{
	if (FMessageWidgets==NULL)
		return NULL;
	foreach (IMessageWindow *window, messageWindows())
		if (window->isActiveTabPage())
			return window;
	return NULL;
}

QTextBrowser *VimPlugin::chatViewer(IMessageWindow *AWindow) const
{
	if (AWindow==NULL || AWindow->viewWidget()==NULL)
		return NULL;
	return qobject_cast<QTextBrowser *>(AWindow->viewWidget()->styleWidget());
}

IMessageEditWidget *VimPlugin::chatEditor(IMessageWindow *AWindow) const
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
	// the previous VISUAL selection is still in the viewer: remember it for
	// "gv" before the mode switch clears the native selection
	if (FMode==VisualMode && AMode!=VisualMode)
	{
		rememberVisualSelection();
		FVisualLine = false;
	}
	FMode = AMode;
	FKeyBuffer.clear();
	FCount = -1;
	if (FMessageWidgets!=NULL)
		foreach (IMessageWindow *window, messageWindows())
			applyModeToWindow(window);
	focusModeWidget(activeChatWindow());
}

void VimPlugin::applyModeToWindow(IMessageWindow *AWindow)
{
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (editor==NULL)
		return;

	QTextEdit *textEdit = editor->textEdit();
	switch (FMode)
	{
	case NormalMode:
	case CaretMode:
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
		if (!isVimMessageMarker(ch) && ch != QChar::ParagraphSeparator
			&& ch != QChar::LineSeparator && ch != QChar::ObjectReplacementCharacter
			&& ch != QChar(QChar::Null))
			break;
		if (ACursor.position() <= 0)
			break;
		ACursor.movePosition(QTextCursor::Left);
	}
	return ACursor;
}

void VimPlugin::updateCursor(IMessageWindow *AWindow, bool AEnsureVisible)
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

	QTextCursor sourceCursor = viewer->textCursor();
	// CARET is a pure cursor mode: whatever selection was left behind (a
	// mouse drag or a stale VISUAL selection) must not survive, the caret
	// is always a single visible block
	if (FMode==CaretMode && sourceCursor.hasSelection())
		sourceCursor.clearSelection();
	QTextCursor cursor = visibleCharacterCursor(sourceCursor, messageMarkerRanges(viewer->document()));
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
void VimPlugin::focusModeWidget(IMessageWindow *AWindow)
{
	if (AWindow==NULL)
		return;
	QTextBrowser *viewer = chatViewer(AWindow);
	IMessageEditWidget *editor = chatEditor(AWindow);
	if (FMode==NormalMode || FMode==CaretMode || FMode==VisualMode)
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

		// while the help window is shown the Escape belongs to it: consuming
		// the override keeps the main window's "close window" shortcut from
		// hiding the main window and taking the parented help window along
		QWidget *focusWidget = QApplication::focusWidget();
		if (inHelpContext(focusWidget))
		{
			keyEvent->accept();
			return true;
		}

		if (contextWindow(focusWidget)!=NULL)
		{
			keyEvent->accept();
			return true;
		}
		if (FMode==NormalMode && inRosterContext(focusWidget))
		{
			keyEvent->accept();
			return true;
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

	// the Escape closes the help window whenever the focus is in it or in the
	// main window (chat log, roster, chrome); the chat editors and other
	// dialogs keep their usual Escape handling.  This also keeps the vim
	// double-Escape gesture from hiding the main window while the help is up.
	if (keyEvent->key()==Qt::Key_Escape && !keyEvent->isAutoRepeat()
		&& inHelpContext(QApplication::focusWidget()))
	{
		FHelpDialog->close();
		keyEvent->accept();
		return true;
	}

	if (keyEvent->isAutoRepeat())
	{
		// holding a scrolling key repeats like the native cursor keys; every
		// other key waits for a fresh press (modes, commands, roster walk)
		if (FMode!=NormalMode && FMode!=CaretMode && FMode!=VisualMode)
			return false;
		QWidget *focusWidget = QApplication::focusWidget();
		IMessageWindow *window = contextWindow(focusWidget);
		QTextBrowser *viewer = window!=NULL ? chatViewer(window) : NULL;
		if (viewer==NULL || (focusWidget!=viewer && !viewer->isAncestorOf(focusWidget)))
			return false;
		const int action = resolveAction(FMode, QList<QKeyCombination>() << eventCombination(FMode,keyEvent));
		if (action!=ActionScrollDown && action!=ActionScrollUp
			&& action!=ActionMoveLeft && action!=ActionMoveRight)
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

// true when the focus is inside the contact list (the roster has its own
// j/k/Enter/Esc handling)
bool VimPlugin::inRosterContext(QWidget *AFocusWidget) const
{
	if (FRostersViewPlugin==NULL || AFocusWidget==NULL)
		return false;
	QWidget *rosterWidget = FRostersViewPlugin->rostersView()!=NULL ? FRostersViewPlugin->rostersView()->instance() : NULL;
	return rosterWidget!=NULL && (AFocusWidget==rosterWidget || rosterWidget->isAncestorOf(AFocusWidget));
}

// true while the ":help" window is shown and the focus belongs to it or to
// the main window (its parent).  The plugin must take the Escape in that
// area, otherwise the main window's "close window" shortcut hides the main
// window and the parented help window along with it.
bool VimPlugin::inHelpContext(QWidget *AFocusWidget) const
{
	if (FHelpDialog==NULL || !FHelpDialog->isVisible() || AFocusWidget==NULL)
		return false;
	if (AFocusWidget==FHelpDialog || FHelpDialog->isAncestorOf(AFocusWidget))
		return true;
	QWidget *mainWindowWidget = FMainWindowPlugin!=NULL && FMainWindowPlugin->mainWindow()!=NULL
		? FMainWindowPlugin->mainWindow()->instance() : NULL;
	return mainWindowWidget!=NULL && mainWindowWidget->isAncestorOf(AFocusWidget);
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
		IMessageWindow *window = contextWindow(focusWidget);
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

	IMessageWindow *window = contextWindow(focusWidget);
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

	if (inRosterContext(focusWidget))
		return processRosterKey(AEvent);

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

// The vim commands are bound to characters, but a non-Latin layout (Cyrillic,
// Greek, ...) produces characters that have no matching Qt::Key: Qt maps them
// to their Unicode value, so a binding like "j" can never match.  Fall back to
// the physical key position and translate it to the US QWERTY character, so
// the same keys work in any layout.
static QChar vimUsCharForKeyPosition(quint32 ACode)
{
#if defined(Q_OS_MACOS)
	// macOS virtual key codes
	switch (ACode)
	{
	case 0x00: return QLatin1Char('a');
	case 0x01: return QLatin1Char('s');
	case 0x02: return QLatin1Char('d');
	case 0x03: return QLatin1Char('f');
	case 0x04: return QLatin1Char('h');
	case 0x05: return QLatin1Char('g');
	case 0x06: return QLatin1Char('z');
	case 0x07: return QLatin1Char('x');
	case 0x08: return QLatin1Char('c');
	case 0x09: return QLatin1Char('v');
	case 0x0B: return QLatin1Char('b');
	case 0x0C: return QLatin1Char('q');
	case 0x0D: return QLatin1Char('w');
	case 0x0E: return QLatin1Char('e');
	case 0x0F: return QLatin1Char('r');
	case 0x10: return QLatin1Char('y');
	case 0x11: return QLatin1Char('t');
	case 0x12: return QLatin1Char('1');
	case 0x13: return QLatin1Char('2');
	case 0x14: return QLatin1Char('3');
	case 0x15: return QLatin1Char('4');
	case 0x16: return QLatin1Char('6');
	case 0x17: return QLatin1Char('5');
	case 0x18: return QLatin1Char('=');
	case 0x19: return QLatin1Char('9');
	case 0x1A: return QLatin1Char('7');
	case 0x1B: return QLatin1Char('-');
	case 0x1C: return QLatin1Char('8');
	case 0x1D: return QLatin1Char('0');
	case 0x1E: return QLatin1Char(']');
	case 0x1F: return QLatin1Char('o');
	case 0x20: return QLatin1Char('u');
	case 0x21: return QLatin1Char('[');
	case 0x22: return QLatin1Char('i');
	case 0x23: return QLatin1Char('p');
	case 0x25: return QLatin1Char('l');
	case 0x26: return QLatin1Char('j');
	case 0x27: return QLatin1Char('\'');
	case 0x28: return QLatin1Char('k');
	case 0x29: return QLatin1Char(';');
	case 0x2A: return QLatin1Char('\\');
	case 0x2B: return QLatin1Char(',');
	case 0x2C: return QLatin1Char('/');
	case 0x2D: return QLatin1Char('n');
	case 0x2E: return QLatin1Char('m');
	case 0x2F: return QLatin1Char('.');
	case 0x32: return QLatin1Char('`');
	}
	return QChar();
#else
	// Windows set 1 scan codes and Linux evdev codes are the same set of
	// numbers; X11/Wayland key codes are the evdev code plus 8
#if !defined(Q_OS_WIN)
	const quint32 code = ACode>=8 ? ACode-8 : ACode;
#else
	const quint32 code = ACode;
#endif
	switch (code)
	{
	case 2:  return QLatin1Char('1');
	case 3:  return QLatin1Char('2');
	case 4:  return QLatin1Char('3');
	case 5:  return QLatin1Char('4');
	case 6:  return QLatin1Char('5');
	case 7:  return QLatin1Char('6');
	case 8:  return QLatin1Char('7');
	case 9:  return QLatin1Char('8');
	case 10: return QLatin1Char('9');
	case 11: return QLatin1Char('0');
	case 12: return QLatin1Char('-');
	case 13: return QLatin1Char('=');
	case 16: return QLatin1Char('q');
	case 17: return QLatin1Char('w');
	case 18: return QLatin1Char('e');
	case 19: return QLatin1Char('r');
	case 20: return QLatin1Char('t');
	case 21: return QLatin1Char('y');
	case 22: return QLatin1Char('u');
	case 23: return QLatin1Char('i');
	case 24: return QLatin1Char('o');
	case 25: return QLatin1Char('p');
	case 26: return QLatin1Char('[');
	case 27: return QLatin1Char(']');
	case 30: return QLatin1Char('a');
	case 31: return QLatin1Char('s');
	case 32: return QLatin1Char('d');
	case 33: return QLatin1Char('f');
	case 34: return QLatin1Char('g');
	case 35: return QLatin1Char('h');
	case 36: return QLatin1Char('j');
	case 37: return QLatin1Char('k');
	case 38: return QLatin1Char('l');
	case 39: return QLatin1Char(';');
	case 40: return QLatin1Char('\'');
	case 41: return QLatin1Char('`');
	case 43: return QLatin1Char('\\');
	case 44: return QLatin1Char('z');
	case 45: return QLatin1Char('x');
	case 46: return QLatin1Char('c');
	case 47: return QLatin1Char('v');
	case 48: return QLatin1Char('b');
	case 49: return QLatin1Char('n');
	case 50: return QLatin1Char('m');
	case 51: return QLatin1Char(',');
	case 52: return QLatin1Char('.');
	case 53: return QLatin1Char('/');
	}
	return QChar();
#endif
}

// the US shift level of a positional character: letters become upper case
// (vim's Shift notation), symbol keys get the character painted above them
static QChar vimUsShiftedChar(QChar AChar)
{
	switch (AChar.unicode())
	{
	case '`':  return QLatin1Char('~');
	case '1':  return QLatin1Char('!');
	case '2':  return QLatin1Char('@');
	case '3':  return QLatin1Char('#');
	case '4':  return QLatin1Char('$');
	case '5':  return QLatin1Char('%');
	case '6':  return QLatin1Char('^');
	case '7':  return QLatin1Char('&');
	case '8':  return QLatin1Char('*');
	case '9':  return QLatin1Char('(');
	case '0':  return QLatin1Char(')');
	case '-':  return QLatin1Char('_');
	case '=':  return QLatin1Char('+');
	case '[':  return QLatin1Char('{');
	case ']':  return QLatin1Char('}');
	case '\\': return QLatin1Char('|');
	case ';':  return QLatin1Char(':');
	case '\'': return QLatin1Char('"');
	case ',':  return QLatin1Char('<');
	case '.':  return QLatin1Char('>');
	case '/':  return QLatin1Char('?');
	default:   return AChar.toUpper();
	}
}

static QKeyCombination vimChordForKeyPosition(quint32 ACode, bool AShifted)
{
	const QChar character = vimUsCharForKeyPosition(ACode);
	if (character.isNull())
		return QKeyCombination();
	return vimChordForCharacter(AShifted ? vimUsShiftedChar(character) : character);
}

// The event token for the sequence matcher.  Printable text has a priority
// and is case-sensitive ("G" is Shift+g), but that only works while the
// layout produces the characters the keymap is written in.  A non-Latin
// layout (Cyrillic, Greek...) produces characters with no matching binding
// and some layouts move the punctuation, so the physical key position is
// used whenever the produced character does not name a vim key itself.
QKeyCombination VimPlugin::eventCombination(VimMode AMode, QKeyEvent *AEvent) const
{
	const Qt::KeyboardModifiers modifiers = AEvent->modifiers();
	// Ctrl/Alt combinations keep their normal meaning
	if (modifiers & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))
		return AEvent->keyCombination();

	const QChar textChar = AEvent->text().length()==1 ? AEvent->text().at(0) : QChar();

	QKeyCombination textChord;
	if (!textChar.isNull())
	{
		textChord = vimChordForCharacter(textChar);
		if (textChord.key()!=Qt::Key_unknown)
		{
			QList<QKeyCombination> single;
			single.append(textChord);
			// A produced vim key (or the start of a multi-key binding) wins.
			// Latin letters keep their character meaning even when they are
			// not commands (y/z on a German layout, Dvorak...), while other
			// characters fall back to the physical position: this is what
			// moves the punctuation of non-Latin layouts back to the US keys.
			const bool latinLetter = textChar.unicode()<0x80 && textChar.isLetter();
			if (latinLetter || resolveAction(AMode,single)>=0 || actionHasPartialMatch(AMode,single))
				return textChord;
		}
	}

	// the produced character does not name a vim key (non-Latin layout or a
	// moved punctuation key): use the physical key position, so the same
	// keys work in any layout
	bool shifted = modifiers & Qt::ShiftModifier;
	if (!shifted && !textChar.isNull())
	{
		// Caps Lock reports an upper case character without Shift
		shifted = textChar.isLetter() && textChar.isUpper();
	}
	const QKeyCombination positionChord = vimChordForKeyPosition(AEvent->nativeScanCode(),shifted);
	if (positionChord.key()!=Qt::Key_unknown)
		return positionChord;

	// no position mapping (e.g. a synthesized event): keep the character
	if (textChord.key()!=Qt::Key_unknown)
		return textChord;
	return AEvent->keyCombination();
}

// The plain vim key character of an event, with the same layout fallback as
// eventCombination(): ASCII letters keep their character meaning, everything
// else is looked up by the physical key position (a Cyrillic layout still
// produces 'j', 'k', 'q'...).  Used by the help window pager.
QChar VimPlugin::keyChar(QKeyEvent *AEvent)
{
	const QChar textChar = AEvent->text().length()==1 ? AEvent->text().at(0) : QChar();
	if (!textChar.isNull() && textChar.unicode()<0x80 && textChar.isLetter())
		return textChar;

	bool shifted = AEvent->modifiers() & Qt::ShiftModifier;
	if (!shifted && !textChar.isNull())
		shifted = textChar.isLetter() && textChar.isUpper();
	const QChar character = vimUsCharForKeyPosition(AEvent->nativeScanCode());
	if (!character.isNull())
		return shifted ? vimUsShiftedChar(character) : character;
	return textChar;
}

bool VimPlugin::processViewerKey(QKeyEvent *AEvent)
{
	IMessageWindow *window = contextWindow(QApplication::focusWidget());
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

	// numeric prefixes (5j, 3G) in the cursor moving modes
	if ((FMode==NormalMode || FMode==CaretMode) && Options::node(OPV_VIM_COUNTS).value().toBool())
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

	QList<QKeyCombination> buffer = FKeyBuffer;
	buffer.append(eventCombination(FMode,AEvent));
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
		IMessageWindow *window = activeChatWindow();
		if (window!=NULL && chatViewer(window)!=NULL)
		{
			chatViewer(window)->setFocus();
			updateCursor(window);
			return true;
		}
		return false;	// no chat window - keep the default vacuum Escape
	}

	QList<QKeyCombination> buffer = FKeyBuffer;
	buffer.append(eventCombination(FMode,AEvent));
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

	// the editor is disabled in NORMAL/CARET/VISUAL, swallow accidental keystrokes
	AEvent->accept();
	return true;
}

// ============ actions ============
bool VimPlugin::executeAction(VimMode AMode, VimActionId AAction, int ACount)
{
	IMessageWindow *window = contextWindow(QApplication::focusWidget());
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
	case ActionMoveLeft:
	case ActionMoveRight:
		{
			const bool visual = (AMode==VisualMode);
			const bool caret = (AMode==CaretMode);
			const int steps = qMax(1,ACount);
			if (viewer==NULL)
				return true;
			if (!visual && !caret)
			{
				// NORMAL scrolls the chat exactly like the native cursor keys:
				// the press is forwarded to the text browser and Qt does the
				// scrolling, so holding the key repeats like holding the
				// arrow key; a count is sent as that many presses
				Qt::Key key = Qt::Key_Down;
				if (AAction==ActionScrollUp)
					key = Qt::Key_Up;
				else if (AAction==ActionMoveLeft)
					key = Qt::Key_Left;
				else if (AAction==ActionMoveRight)
					key = Qt::Key_Right;
				for (int i=0; i<steps; ++i)
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
				// step), Down/Up move over the rows of the messages.
				// VISUAL drags the selection anchor along, CARET only walks
				// the block cursor so the next 'v' can anchor there.
				QTextCursor::MoveOperation operation = QTextCursor::Down;
				if (AAction==ActionScrollUp)
					operation = QTextCursor::Up;
				else if (AAction==ActionMoveLeft)
					operation = QTextCursor::Left;
				else if (AAction==ActionMoveRight)
					operation = QTextCursor::Right;
				// linewise VISUAL has no horizontal motion: h/l neither
				// shrink the selection to a character nor leave whole lines
				if (visual && FVisualLine && (operation==QTextCursor::Left || operation==QTextCursor::Right))
					return true;
				QTextCursor cursor = viewer->textCursor();
				cursor.movePosition(operation, visual ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor, steps);
				if (visual && FVisualLine)
					cursor = linewiseCursor(viewer->document(), cursor.anchor(), cursor.position());
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
					if (FVisualLine)
						cursor = linewiseCursor(viewer->document(), anchor, cursor.position());
					else
					{
						QTextCursor selection(cursor);
						selection.setPosition(anchor, QTextCursor::MoveAnchor);
						selection.setPosition(cursor.position(), QTextCursor::KeepAnchor);
						cursor = selection;
					}
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
			if (visual && FVisualLine)
				cursor = linewiseCursor(viewer->document(), cursor.anchor(), cursor.position());
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
	case ActionCopyNick:
		doCopyAuthor(window, false);
		return true;
	case ActionCopyJid:
		doCopyAuthor(window, true);
		return true;
	case ActionEnterVisual:
		if (AMode==VisualMode)
		{
			// like vim: 'v' in linewise VISUAL drops back to charwise
			// (keeping the selected area), 'v' in charwise leaves VISUAL
			if (FVisualLine)
			{
				FVisualLine = false;
				updateCursor(window, false);
			}
			else
				setApplicationMode(NormalMode);
			return true;
		}
		FVisualLine = false;
		if (AMode==CaretMode)
		{
			// 'v' in CARET turns the block cursor position into the anchor
			// of a VISUAL selection; the first motion extends it
			setApplicationMode(VisualMode);
		}
		else
		{
			// NORMAL enters CARET when there is nothing selected yet (the
			// cursor was placed on the visible area by the common guard
			// above) and goes straight to VISUAL when the mouse left a
			// selection in the viewer
			QTextCursor cursor = viewer!=NULL ? viewer->textCursor() : QTextCursor();
			setApplicationMode(cursor.hasSelection() ? VisualMode : CaretMode);
		}
		return true;
	case ActionEnterVisualLine:
		doEnterVisualLine(window);
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
	case ActionPasteQuotation:
		doPasteQuotation();
		return true;
	case ActionReselectVisual:
		doReselectVisual(window);
		return true;
	case ActionSwapSelectionEnds:
		{
			// vim "o" in VISUAL: the block cursor jumps to the opposite end
			// of the selection, so the following motions extend it from there
			if (viewer==NULL)
				return true;
			QTextCursor cursor = viewer->textCursor();
			if (!cursor.hasSelection())
				return true;
			const int anchor = cursor.anchor();
			const int position = cursor.position();
			cursor.setPosition(position);
			cursor.setPosition(anchor, QTextCursor::KeepAnchor);
			viewer->setTextCursor(cursor);
			updateCursor(window, false);
			viewer->ensureCursorVisible();
		}
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
				const int selStart = qMin(cursor.anchor(), cursor.position());
				const int selEnd = qMax(cursor.anchor(), cursor.position());
				QString text = visualSelectionText(viewer);
				if (text.isEmpty())
				{
					// A selection that covers no message body (a status line,
					// date separator) keeps the native selection as a fallback.
					text = cursor.selectedText();
					text.replace(QChar(0x2029), '\n');
					text.replace(QChar(0x2028), '\n');
					text.remove(VIM_MESSAGE_START);
					text.remove(VIM_MESSAGE_END);
				}
				QApplication::clipboard()->setText(text);
				setApplicationMode(NormalMode);
				// NORMAL hides the selection, so confirm the yank with the flash
				if (!text.isEmpty())
					flashCopiedRange(window,selStart,selEnd);
			}
			return true;
		}
	default:
		return false;
	}
}

QList< QPair<int,int> > VimPlugin::messageMarkerRanges(QTextDocument *ADocument) const
{
	// [start,end) ranges of every message body bound by the Vim markers.
	// The end marker is placed before the marks slot, so the ranges contain
	// neither the sender/time wrapper nor the service glyphs.
	QList< QPair<int,int> > ranges;
	QString text = ADocument->toPlainText();
	int start = -1;
	for (int pos=0; pos<text.size(); ++pos)
	{
		const QChar ch = text.at(pos);
		if (ch == VIM_MESSAGE_START)
		{
			if (start >= 0)
				ranges.append(qMakePair(start+1,pos));
			start = pos;
		}
		else if (ch == VIM_MESSAGE_END && start >= 0)
		{
			ranges.append(qMakePair(start+1,pos));
			start = -1;
		}
	}
	if (start >= 0)
		ranges.append(qMakePair(start+1,text.size()));
	return ranges;
}

// the message under APosition, or the nearest body (preferring the following
// one: the sender/time cells of a message precede its body), used by the
// copy commands when the caret sits on a cell or on the bounds markers
QPair<int,int> VimPlugin::nearestMessageRange(QTextDocument *ADocument, int APosition) const
{
	const QList< QPair<int,int> > ranges = messageMarkerRanges(ADocument);
	for (int i=0; i<ranges.count(); ++i)
		if (APosition >= ranges.at(i).first && APosition <= ranges.at(i).second)
			return ranges.at(i);
	if (!ranges.isEmpty())
	{
		QPair<int,int> range = ranges.last();
		for (int i=0; i<ranges.count(); ++i)
			if (APosition <= ranges.at(i).first)
				return ranges.at(i);
		return range;
	}
	return qMakePair(-1,-1);
}

QString VimPlugin::messageCopyText(QTextDocument *ADocument, int AStart, int AEnd) const
{
	if (ADocument==NULL || AStart>=AEnd)
		return QString();

	QTextCursor cursor(ADocument);
	cursor.setPosition(AStart);
	cursor.setPosition(AEnd,QTextCursor::KeepAnchor);
	QString body = cursor.selectedText();
	body.replace(QChar(0x2029), '\n');
	body.replace(QChar(0x2028), '\n');
	body.remove(VIM_MESSAGE_START);
	body.remove(VIM_MESSAGE_END);

	// Style marks can also be rendered as inline objects (emoji-like
	// service glyphs).  They are outside of the body range, but strip
	// replacement characters defensively.
	body.remove(QChar::ObjectReplacementCharacter);

	const QString sender = plainSenderName(ADocument->findBlock(AStart).blockFormat().property(VIM_SENDER_PROPERTY).toString());
	if (!sender.isEmpty())
		return sender + QStringLiteral(": ") + body;
	return body;
}

QString VimPlugin::visualSelectionText(QTextBrowser *AViewer) const
{
	if (AViewer == NULL)
		return QString();

	const QTextCursor selection = AViewer->textCursor();
	const int selStart = qMin(selection.anchor(),selection.position());
	const int selEnd = qMax(selection.anchor(),selection.position());
	if (selStart == selEnd)
		return QString();

	QStringList parts;
	const QList< QPair<int,int> > ranges = messageMarkerRanges(AViewer->document());
	for (const QPair<int,int> &range : ranges)
	{
		if (selEnd<=range.first || selStart>=range.second)
			continue;
		const int start = qMax(selStart,range.first);
		const int end = qMin(selEnd,range.second);
		const QString text = messageCopyText(AViewer->document(),start,end);
		if (!text.isEmpty())
			parts.append(text);
	}
	return parts.join(QChar('\n'));
}

// remembers the selection that VISUAL leaves behind, so "gv" can bring it
// back (vim keeps the last visual area even after the selection is dropped)
void VimPlugin::rememberVisualSelection()
{
	IMessageWindow *window = contextWindow(QApplication::focusWidget());
	if (window == NULL)
		window = activeChatWindow();
	QTextBrowser *viewer = window!=NULL ? chatViewer(window) : NULL;
	if (viewer == NULL)
		return;
	const QTextCursor cursor = viewer->textCursor();
	if (!cursor.hasSelection())
		return;
	FLastVisualViewer = viewer;
	FLastVisualAnchor = cursor.anchor();
	FLastVisualPosition = cursor.position();
	FLastVisualLine = FVisualLine;
}

// "V": linewise VISUAL.  From NORMAL/CARET it selects the whole line under
// the cursor, in charwise VISUAL it expands the area to whole lines, and in
// linewise VISUAL it leaves the mode (vim's 'V' toggles back to NORMAL)
void VimPlugin::doEnterVisualLine(IMessageWindow *AWindow)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	if (viewer==NULL || viewer->document()==NULL)
		return;

	if (FMode==VisualMode)
	{
		if (FVisualLine)
		{
			setApplicationMode(NormalMode);
			return;
		}
		const QTextCursor cursor = viewer->textCursor();
		FVisualLine = true;
		viewer->setTextCursor(linewiseCursor(viewer->document(), cursor.anchor(), cursor.position()));
		updateCursor(AWindow, false);
		return;
	}

	FVisualLine = true;
	setApplicationMode(VisualMode);
	const QTextCursor cursor = viewer->textCursor();
	viewer->setTextCursor(linewiseCursor(viewer->document(), cursor.position(), cursor.position()));
	updateCursor(AWindow, true);
	viewer->ensureCursorVisible();
}

// "gv": restore the last VISUAL selection in the same viewer and re-enter
// VISUAL, so the area can be copied or extended again
void VimPlugin::doReselectVisual(IMessageWindow *AWindow)
{
	QTextBrowser *viewer = AWindow!=NULL ? chatViewer(AWindow) : NULL;
	if (viewer==NULL || FLastVisualViewer.isNull() || FLastVisualViewer.data()!=viewer)
		return;
	QTextDocument *document = viewer->document();
	if (document == NULL)
		return;
	const int lastPosition = qMax(0, document->characterCount()-1);
	const int anchor = qBound(0, FLastVisualAnchor, lastPosition);
	const int position = qBound(0, FLastVisualPosition, lastPosition);
	if (anchor == position)
		return;
	QTextCursor cursor(document);
	cursor.setPosition(anchor);
	cursor.setPosition(position, QTextCursor::KeepAnchor);
	viewer->setTextCursor(cursor);
	FVisualLine = FLastVisualLine;
	setApplicationMode(VisualMode);
	viewer->ensureCursorVisible();
}

// A copy action does not change the viewer, so give the yank a short visible
// confirmation: the copied characters are painted with a translucent extra
// selection for a moment.  Extra selections do not touch the text cursor or
// the native selection, so the clipboard, the mode cursor and any pending key
// sequence stay intact; the mode refresh removes the highlight afterwards.
void VimPlugin::flashCopiedRange(IMessageWindow *AWindow, int AStart, int AEnd)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	if (viewer == NULL)
		return;
	QTextDocument *document = viewer->document();
	if (document == NULL)
		return;

	const int lastPosition = qMax(0, document->characterCount()-1);
	AStart = qBound(0, AStart, lastPosition);
	AEnd = qBound(AStart, AEnd, lastPosition);
	if (AStart >= AEnd)
		return;

	QTextEdit::ExtraSelection flash;
	flash.cursor = QTextCursor(document);
	flash.cursor.setPosition(AStart);
	flash.cursor.setPosition(AEnd, QTextCursor::KeepAnchor);
	flash.format.setBackground(kColorFlash);
	viewer->setExtraSelections(QList<QTextEdit::ExtraSelection>() << flash);

	const quint64 generation = ++FFlashGeneration;
	QPointer<QTextBrowser> guard(viewer);
	QTimer::singleShot(VIM_COPY_FLASH_MSECS, this, [this,generation,guard]() {
		if (guard.isNull() || generation != FFlashGeneration)
			return;
		IMessageWindow *window = contextWindow(guard.data());
		if (window != NULL)
			updateCursor(window, false);	// NORMAL clears, other modes repaint
		else
			guard->setExtraSelections(QList<QTextEdit::ExtraSelection>());
	});
}

void VimPlugin::doCopyBlock(IMessageWindow *AWindow, bool AMessage)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	if (viewer==NULL)
		return;
	QTextCursor cursor = viewer->textCursor();
	QString text;
	int copyStart = -1;
	int copyEnd = -1;
	if (AMessage)
	{
		const QPair<int,int> range = nearestMessageRange(viewer->document(), cursor.position());
		if (range.first>=0)
		{
			text = messageCopyText(viewer->document(),range.first,range.second);
			copyStart = range.first;
			copyEnd = range.second;
		}
	}
	else
	{
		// yy is the vim "yank line": one displayed line, not the whole
		// message body (yb) and not the entire text block of a table-based
		// style, so a long wrapped message is copied line by line
		cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::MoveAnchor);
		copyStart = cursor.position();
		cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
		copyEnd = cursor.position();
		text = cursor.selectedText();
		text.remove(VIM_MESSAGE_START);
		text.remove(VIM_MESSAGE_END);
	}
	if (!text.isEmpty())
	{
		QApplication::clipboard()->setText(text);
		flashCopiedRange(AWindow,copyStart,copyEnd);
	}
}

// the COMMAND "nick"/"jid" copies target the author of the message under the
// vim cursor (the name shown in the chat); when the log has no message at the
// cursor they fall back to the window itself: the contact of a private chat,
// the room of a conference.  In a room the JID is the occupant's real JID in
// non-anonymous rooms and the room/nick occupant address otherwise.
void VimPlugin::doCopyAuthor(IMessageWindow *AWindow, bool AJid)
{
	QTextBrowser *viewer = chatViewer(AWindow);
	QTextDocument *document = viewer!=NULL ? viewer->document() : NULL;

	QString author;
	if (document != NULL)
	{
		const QPair<int,int> range = nearestMessageRange(document, viewer->textCursor().position());
		if (range.first >= 0)
			author = plainSenderName(document->findBlock(range.first).blockFormat().property(VIM_SENDER_PROPERTY).toString());
	}

	IMultiUserChatWindow *mucWindow = AWindow!=NULL ? qobject_cast<IMultiUserChatWindow *>(AWindow->instance()) : NULL;
	QString text;
	if (AJid)
	{
		if (mucWindow != NULL)
		{
			IMultiUserChat *muc = mucWindow->multiUserChat();
			IMultiUser *user = muc!=NULL && !author.isEmpty() ? muc->findUser(author) : NULL;
			if (user != NULL)
			{
				const Jid realJid = user->realJid();
				text = realJid.isValid() ? realJid.bare() : user->userJid().full();
			}
			if (text.isEmpty() && muc != NULL)
				text = muc->roomJid().bare();
		}
		else if (AWindow!=NULL && AWindow->address()!=NULL)
		{
			// a private chat opened from a room is addressed as room/nick
			// when the room is anonymous; a normal contact keeps bare()
			const Jid contactJid = AWindow->address()->contactJid();
			const bool roomOccupant = contactJid.hasResource() && FMultiChatManager!=NULL
				&& FMultiChatManager->findMultiChatWindow(AWindow->address()->streamJid(),contactJid.bare())!=NULL;
			text = roomOccupant ? contactJid.full() : contactJid.bare();
		}
	}
	else if (mucWindow != NULL)
	{
		// in a room the nick is the author of the message under the cursor
		text = author;
		if (text.isEmpty() && mucWindow->multiUserChat()!=NULL)
		{
			IMultiUserChat *muc = mucWindow->multiUserChat();
			text = muc->roomName().isEmpty() ? muc->roomJid().node() : muc->roomName();
		}
	}
	else
	{
		// a private chat has one interlocutor: the contact, not the author of
		// the message under the cursor (that may be our own outgoing message).
		// The displayed name comes from the newest incoming message.
		if (document != NULL)
		{
			const QList< QPair<int,int> > ranges = messageMarkerRanges(document);
			for (int i=ranges.count()-1; i>=0 && text.isEmpty(); --i)
			{
				const QTextBlock block = document->findBlock(ranges.at(i).first);
				if (block.blockFormat().property(VIM_DIRECTION_PROPERTY).toInt()==IMessageStyleContentOptions::DirectionIn)
					text = plainSenderName(block.blockFormat().property(VIM_SENDER_PROPERTY).toString());
			}
		}
		if (text.isEmpty() && AWindow!=NULL && AWindow->address()!=NULL)
		{
			const Jid contactJid = AWindow->address()->contactJid();
			text = contactJid.node().isEmpty() ? contactJid.bare() : contactJid.node();
		}
	}

	if (text.isEmpty())
	{
		QApplication::beep();
		LOG_INFO("[VimPlugin] cannot resolve the author for the copy command");
		return;
	}

	QApplication::clipboard()->setText(text);
	LOG_INFO(QString("[VimPlugin] copied %1: '%2'").arg(AJid ? "jid" : "nick").arg(text));
}

void VimPlugin::doSearchPrompt()
{
	if (FMode != NormalMode)
		return;
	IMessageWindow *window = activeChatWindow();
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
	IMessageWindow *window = contextWindow(QApplication::focusWidget());
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
	IMessageWindow *window = activeChatWindow();
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
	IMessageWindow *window = activeChatWindow();
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
	text.remove(VIM_MESSAGE_START);
	text.remove(VIM_MESSAGE_END);
	text.replace(QChar(0x2029), '\n');
	QTextCursor cursor = textEdit->textCursor();
	cursor.movePosition(QTextCursor::End);
	textEdit->setTextCursor(cursor);
	if (!text.isEmpty())
		textEdit->insertPlainText(text);
	textEdit->setFocus();
}

// vim P: paste the clipboard into the input box as a quoted reply like the
// "Quote selected text" action does, then continue in INSERT
void VimPlugin::doPasteQuotation()
{
	IMessageWindow *window = activeChatWindow();
	IMessageEditWidget *editor = chatEditor(window);
	if (editor==NULL)
		return;
	setApplicationMode(InsertMode);
	QTextEdit *textEdit = editor->textEdit();
	if (textEdit==NULL || window==NULL || !window->isActiveTabPage())
		return;
	QString text = QApplication::clipboard()->text();
	text.remove(VIM_MESSAGE_START);
	text.remove(VIM_MESSAGE_END);
	text.replace(QChar(0x2029), '\n');
	text.replace(QChar(0x2028), '\n');
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace('\r', '\n');
	if (text.isEmpty())
		return;
	QStringList lines = text.split('\n');
	for (int i=0; i<lines.count(); ++i)
		lines[i] = QStringLiteral("> ") + lines.at(i);
	QTextCursor cursor = textEdit->textCursor();
	cursor.movePosition(QTextCursor::End);
	textEdit->setTextCursor(cursor);
	if (!cursor.atBlockStart())
		textEdit->insertPlainText(QStringLiteral("\n"));
	textEdit->insertPlainText(lines.join('\n'));
	textEdit->setFocus();
}

void VimPlugin::doEnterCommand()
{
	IMessageWindow *window = activeChatWindow();
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
	IMessageWindow *window = activeChatWindow();
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
	else if (name=="help" || name=="h")
	{
		// leave COMMAND before showing the window: focusModeWidget() would
		// otherwise take the focus back to the chat viewer
		setApplicationMode(NormalMode);
		showHelp(args.section(' ', 0, 0));
		return;
	}
	else
	{
		LOG_INFO(QString("[VimPlugin] unknown command '%1'").arg(command));
		QApplication::beep();
	}
	setApplicationMode(NormalMode);
}

// ============ help ============
// The COMMAND mode commands.  Kept next to the ":help" page because it shows
// them (and the future command name completion will read the same table).
struct VimHelpCommand
{
	const char *names;	// aliases separated by ';', the canonical one first
	const char *args;
	const char *summary;
};

static const VimHelpCommand kHelpCommands[] =
{
	{ "q;quit",              "",           QT_TR_NOOP("Quit Vacuum") },
	{ "hide",                "",           QT_TR_NOOP("Hide the main window to the tray") },
	{ "a;available;online",  "[status]",   QT_TR_NOOP("Set the online status") },
	{ "na;notavailable",     "[status]",   QT_TR_NOOP("Set the extended away status") },
	{ "away",                "[status]",   QT_TR_NOOP("Set the away status") },
	{ "dnd",                 "[status]",   QT_TR_NOOP("Set the do-not-disturb status") },
	{ "off;offline",         "[status]",   QT_TR_NOOP("Go offline") },
	{ "status",              "<show> [status]", QT_TR_NOOP("Set a presence show (online, away, xa, dnd, offline) and a status text") },
	{ "w;send",              "<text>",     QT_TR_NOOP("Send the text as a message") },
	{ "clear",               "",           QT_TR_NOOP("Clear the chat log") },
	{ "close;tabclose",      "",           QT_TR_NOOP("Close the current chat tab (the last tab of a tab window is kept)") },
	{ "chat",                "<jid> [text]", QT_TR_NOOP("Open a chat with the contact and optionally send a message") },
	{ "join",                "<room> [nick] [password]", QT_TR_NOOP("Join a conference room") },
	{ "help;h",              "[topic]",    QT_TR_NOOP("Show this help (topics: modes, normal, visual, command, search, global)") }
};

// the anchors of the page, ":help <topic>" scrolls to one of them
static QString vimHelpAnchorForTopic(const QString &ATopic)
{
	const QString topic = ATopic.toLower();
	if (topic=="mode" || topic=="modes")
		return QString("modes");
	if (topic=="normal" || topic=="keys" || topic=="caret")
		return QString("normal");
	if (topic=="visual" || topic=="v")
		return QString("visual");
	if (topic=="command" || topic=="commands" || topic=="cmd")
		return QString("command");
	if (topic=="search")
		return QString("search");
	if (topic=="global" || topic=="hotkeys")
		return QString("global");
	return QString();
}

// the key string as it is shown in the help: the variants of one binding are
// separated by ';' in the option, "j;Down" becomes "j / Down"
static QString vimHelpKeys(const QString &AKeys)
{
	if (AKeys.trimmed().isEmpty())
		return QString("&mdash;");
	return AKeys.split(';').join(" / ").toHtmlEscaped();
}

void VimPlugin::showHelp(const QString &ATopic)
{
	if (FHelpDialog.isNull())
	{
		QWidget *parent = FMainWindowPlugin!=NULL && FMainWindowPlugin->mainWindow()!=NULL
			? FMainWindowPlugin->mainWindow()->instance() : NULL;
		FHelpDialog = new VimHelpDialog(parent);
	}
	FHelpDialog->setHtml(buildHelpHtml(), vimHelpAnchorForTopic(ATopic));
	FHelpDialog->show();
	FHelpDialog->raise();
	FHelpDialog->activateWindow();
	FHelpDialog->focusBrowser();
	// a Wayland compositor may apply the activation asynchronously, ask once
	// more after the window is mapped
	QTimer::singleShot(0, FHelpDialog.data(), [this]()
	{
		if (FHelpDialog!=NULL && FHelpDialog->isVisible())
		{
			FHelpDialog->raise();
			FHelpDialog->activateWindow();
			FHelpDialog->focusBrowser();
		}
	});
}

QString VimPlugin::buildHelpHtml() const
{
	QString html;
	html += "<html><body>";
	html += QString("<h1>%1</h1>").arg(tr("Vim mode"));
	html += QString("<p><b>q</b> / <b>Esc</b> %1</p>")
		.arg(tr("closes this window. <b>j</b>/<b>k</b> scroll, <b>Ctrl-D</b>/<b>Ctrl-U</b> by half a page, "
			"<b>Ctrl-F</b>/<b>Ctrl-B</b> by a page, <b>gg</b>/<b>G</b> jump to the top/bottom."));
	html += QString("<p>%1</p>").arg(tr("The bindings below are the current ones; they can be changed in "
		"<b>Settings -&gt; Vim mode</b>."));

	// modes
	html += QString("<h2><a name=\"modes\"></a>%1</h2>").arg(tr("Modes"));
	html += "<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"4\">";
	const struct { const char *mode; const char *enter; const char *what; } kModes[] = {
		{ "NORMAL",  QT_TR_NOOP("default, or Esc"),                        QT_TR_NOOP("Moves a block cursor over the chat log and runs the keys below") },
		{ "CARET",   QT_TR_NOOP("v before anything is selected"),          QT_TR_NOOP("NORMAL with a visible cursor: a motion moves it, v starts a selection") },
		{ "VISUAL",  QT_TR_NOOP("v (charwise) / V (linewise)"),            QT_TR_NOOP("Selects characters or whole messages; y copies the selection") },
		{ "INSERT",  QT_TR_NOOP("i, or c to edit the last message"),       QT_TR_NOOP("The input box is a normal chat editor; Esc returns to NORMAL") },
		{ "COMMAND", QT_TR_NOOP(":"),                                      QT_TR_NOOP("The input box is the command line; Enter runs the command, Esc cancels") },
		{ "SEARCH",  QT_TR_NOOP("/"),                                      QT_TR_NOOP("Pattern prompt; Enter searches, Esc cancels, n/N repeat from NORMAL") }
	};
	for (unsigned i=0; i<sizeof(kModes)/sizeof(kModes[0]); ++i)
	{
		html += QString("<tr><td width=\"15%\" valign=\"top\"><b>%1</b></td><td width=\"28%\" valign=\"top\">%2</td><td valign=\"top\">%3</td></tr>")
			.arg(tr(kModes[i].mode)).arg(tr(kModes[i].enter)).arg(tr(kModes[i].what));
	}
	html += "</table>";

	auto appendActions = [this,&html](VimMode AMode, const QList<VimActionId> &AActions)
	{
		html += "<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"4\">";
		foreach (VimActionId action, AActions)
		{
			html += QString("<tr><td width=\"22%\" valign=\"top\"><b>%1</b></td><td>%2</td></tr>")
				.arg(vimHelpKeys(actionKeysString(AMode,action)))
				.arg(actionTitle(action).toHtmlEscaped());
		}
		html += "</table>";
	};

	// NORMAL
	html += QString("<h2><a name=\"normal\"></a>%1</h2>").arg(tr("NORMAL keys"));
	appendActions(NormalMode, vimModeActions(NormalMode));
	html += QString("<p>%1</p>").arg(tr("<b>CARET</b> keeps the motions, the copy commands, v/V and the counts; "
		"the prompt, paste, notification and roster keys are NORMAL only."));

	// VISUAL
	html += QString("<h2><a name=\"visual\"></a>%1</h2>").arg(tr("VISUAL keys"));
	appendActions(VisualMode, vimModeActions(VisualMode));
	html += QString("<p>%1</p>").arg(tr("<b>v</b> selects characters, <b>V</b> whole messages (linewise): vertical motions "
		"extend a linewise selection by whole lines and h/l are ignored. <b>v</b>/<b>V</b> switch between the two kinds "
		"and leave VISUAL to NORMAL, <b>gv</b> restores the last selection."));

	// SEARCH
	html += QString("<h2><a name=\"search\"></a>%1</h2>").arg(tr("Search"));
	html += QString("<p>%1</p>").arg(tr("<b>/</b> opens the pattern prompt, Enter searches forward from the current "
		"position, <b>n</b> and <b>N</b> repeat the search. An invalid pattern beeps."));

	// COMMAND
	html += QString("<h2><a name=\"command\"></a>%1</h2>").arg(tr("COMMAND mode"));
	html += QString("<p>%1</p>").arg(tr("Press <b>:</b> in NORMAL, type a command and press Enter. "
		"Missing arguments are taken from the rest of the line."));
	html += "<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"4\">";
	for (unsigned i=0; i<sizeof(kHelpCommands)/sizeof(kHelpCommands[0]); ++i)
	{
		QStringList aliases;
		foreach (const QString &alias, QString::fromLatin1(kHelpCommands[i].names).split(';'))
			aliases.append(":" + alias.toHtmlEscaped());
		QString command = aliases.join(", ");
		const QString args = QString::fromLatin1(kHelpCommands[i].args).trimmed();
		if (!args.isEmpty())
			command += " " + args.toHtmlEscaped();
		html += QString("<tr><td width=\"32%\" valign=\"top\"><b>%1</b></td><td>%2</td></tr>")
			.arg(command).arg(tr(kHelpCommands[i].summary));
	}
	html += "</table>";

	// global hotkeys
	html += QString("<h2><a name=\"global\"></a>%1</h2>").arg(tr("Global hotkeys"));
	html += "<table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"4\">";
	const struct { const char *id; const char *title; } kGlobalKeys[] = {
		{ SCT_GLOBAL_SHOWROSTER,              QT_TR_NOOP("Show or hide the main window") },
		{ SCT_GLOBAL_ACTIVATELASTNOTIFICATION, QT_TR_NOOP("Activate the last notification") }
	};
	for (unsigned i=0; i<sizeof(kGlobalKeys)/sizeof(kGlobalKeys[0]); ++i)
	{
		const QKeySequence key = globalShortcutKey(kGlobalKeys[i].id);
		html += QString("<tr><td width=\"22%\" valign=\"top\"><b>%1</b></td><td>%2</td></tr>")
			.arg(key.isEmpty() ? QString("&mdash;") : key.toString(QKeySequence::NativeText).toHtmlEscaped())
			.arg(tr(kGlobalKeys[i].title));
	}
	html += "</table>";

	// notes
	html += QString("<h2>%1</h2>").arg(tr("Notes"));
	html += "<ul>";
	html += QString("<li>%1</li>").arg(tr("Counts prefix the motions: <b>5j</b> scrolls five lines, <b>3}</b> three messages."));
	html += QString("<li>%1</li>").arg(tr("Two Escapes within a second close the main window, a single Escape only leaves the current mode."));
	html += QString("<li>%1</li>").arg(tr("The keys work in any keyboard layout: when the produced character is not a vim key, "
		"the physical key position is used."));
	html += "</ul>";

	html += "</body></html>";
	return html;
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

