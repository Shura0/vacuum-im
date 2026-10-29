#include "vimoptionswidget.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "vimplugin.h"

#include <definitions/optionvalues.h>
#include <definitions/shortcuts.h>
#include <utils/options.h>

enum KeysColumn
{
	KC_Normal,
	KC_Visual,
	KC__Count
};

VimOptionsWidget::VimOptionsWidget(QWidget *AParent) : QWidget(AParent)
{
	FEnable = new QCheckBox(tr("Enabled"), this);
	FCounts = new QCheckBox(tr("Digit prefixes (e.g. 5j move 5 lines)"), this);
	connect(FEnable, SIGNAL(toggled(bool)), SIGNAL(modified()));
	connect(FCounts, SIGNAL(toggled(bool)), SIGNAL(modified()));

	QGroupBox *keys = new QGroupBox(tr("Keys"), this);
	FKeyTable = new QTableWidget(Action__Count, KC__Count, keys);
	FKeyTable->horizontalHeader()->setSectionResizeMode(KC_Normal, QHeaderView::ResizeToContents);
	FKeyTable->horizontalHeader()->setSectionResizeMode(KC_Visual, QHeaderView::ResizeToContents);
	FKeyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	FKeyTable->verticalHeader()->setVisible(false);
	FKeyTable->setEditTriggers(QAbstractItemView::AllEditTriggers);
	for (int action = 0; action < Action__Count; ++action)
		createRow(action);

	QPushButton *resetButton = new QPushButton(tr("Reset keys to defaults"), keys);
	connect(resetButton, SIGNAL(clicked(bool)), SLOT(reset()));

	QGroupBox *general = new QGroupBox(tr("General"), this);
	QVBoxLayout *generalLayout = new QVBoxLayout(general);
	generalLayout->addWidget(FEnable);
	generalLayout->addWidget(FCounts);

	// system wide shortcuts: the shortcut manager plugin configures the same
	// Shortcuts records, but it conflicts with the vim mode
	QGroupBox *hotkeys = new QGroupBox(tr("Global hotkeys"), this);
	FShowRosterKey = new QKeySequenceEdit(hotkeys);
	FShowRosterKey->setMaximumSequenceLength(1);
	FShowMessageKey = new QKeySequenceEdit(hotkeys);
	FShowMessageKey->setMaximumSequenceLength(1);
	connect(FShowRosterKey, SIGNAL(keySequenceChanged(const QKeySequence &)), SIGNAL(modified()));
	connect(FShowMessageKey, SIGNAL(keySequenceChanged(const QKeySequence &)), SIGNAL(modified()));
	QLabel *hotkeysHelp = new QLabel(tr("These hotkeys work when the application window is not focused. "
		"Press the key combination to assign it, press Backspace to clear."), hotkeys);
	hotkeysHelp->setWordWrap(true);
	QFormLayout *hotkeysLayout = new QFormLayout(hotkeys);
	hotkeysLayout->addRow(tr("Open roster:"), FShowRosterKey);
	hotkeysLayout->addRow(tr("Show received message:"), FShowMessageKey);
	hotkeysLayout->addRow(hotkeysHelp);

	QLabel *help = new QLabel(tr("Keys are set as QKeySequence notations (\"j\", \"gg\", \"Down\"). "
	"Several variants for one action are separated by ';', e.g. \"j;Down\"."), keys);
	help->setWordWrap(true);
	QVBoxLayout *keysLayout = new QVBoxLayout(keys);
	keysLayout->addWidget(help);
	keysLayout->addWidget(FKeyTable);
	keysLayout->addWidget(resetButton);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(general);
	layout->addWidget(hotkeys);
	layout->addWidget(keys);
	layout->addStretch();

	reset();
}

void VimOptionsWidget::createRow(int AAction)
{
	const int row = (int)AAction;
	QString title = VimPlugin::actionTitle(VimActionId(AAction));
	QTableWidgetItem *nameItem = new QTableWidgetItem(title);
	nameItem->setFlags(Qt::ItemIsEnabled);
	FKeyTable->setItem(row, KC_Normal, nameItem);

	for (int column = KC_Normal; column < KC__Count; ++column)
	{
		QTableWidgetItem *item = new QTableWidgetItem();
		FKeyTable->setItem(row, column, item);
	}
}

int VimOptionsWidget::keyRow(int AAction) const
{
	return AAction;  // table rows are keyed by actions
}

bool VimOptionsWidget::hasVisualColumn(int AAction) const
{
	Q_UNUSED(AAction);
	return true;
}

void VimOptionsWidget::apply()
{
	Options::node(OPV_VIM_ENABLE).setValue(FEnable->isChecked());
	Options::node(OPV_VIM_COUNTS).setValue(FCounts->isChecked());
	for (int action = 0; action < Action__Count; ++action)
	{
		for (int column = KC_Normal; column < KC__Count; ++column)
		{
			const int row = keyRow(action);
			QTableWidgetItem *item = FKeyTable->item(row, column);
			if (item != NULL)
			{
				const QString keys = item->text().simplified();
				VimPlugin::saveActionKeysString(
					column==KC_Normal ? NormalMode : VisualMode,
					VimActionId(action), keys);
			}
		}
	}
	VimPlugin::saveGlobalShortcut(SCT_GLOBAL_SHOWROSTER, FShowRosterKey->keySequence());
	VimPlugin::saveGlobalShortcut(SCT_GLOBAL_ACTIVATELASTNOTIFICATION, FShowMessageKey->keySequence());
	emit modified();
}

void VimOptionsWidget::reset()
{
	FEnable->setChecked(Options::node(OPV_VIM_ENABLE).value().toBool());
	FCounts->setChecked(Options::node(OPV_VIM_COUNTS).value().toBool());
	FShowRosterKey->setKeySequence(VimPlugin::globalShortcutKey(SCT_GLOBAL_SHOWROSTER));
	FShowMessageKey->setKeySequence(VimPlugin::globalShortcutKey(SCT_GLOBAL_ACTIVATELASTNOTIFICATION));
	// values in the options store hold the *effective* keys (defaults are
	// re-derived by the plugin on read)
	for (int action = 0; action < Action__Count; ++action)
	{
		FKeyTable->item(keyRow(action), KC_Normal)->setText(
			VimPlugin::actionKeysString(NormalMode, VimActionId(action)));
		FKeyTable->item(keyRow(action), KC_Visual)->setText(
			VimPlugin::actionKeysString(VisualMode, VimActionId(action)));
	}
	emit modified();
}
