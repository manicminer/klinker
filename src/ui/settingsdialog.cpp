#include "settingsdialog.h"

#include "browsereditdialog.h"
#include "theme.h"

#include "core/rules.h"
#include "core/urlcleaner.h"

#include <KMessageWidget>
#include <KPageWidget>
#include <KPageWidgetModel>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

enum BrowserColumn {
    BrowserNameColumn,
    BrowserCommandColumn,
    BrowserStatusColumn,
};

enum RuleColumn {
    RuleEnabledColumn,
    RuleTypeColumn,
    RulePatternColumn,
    RuleBrowserColumn,
    RuleProblemColumn,
};

QString ruleTypeLabel(RuleType type)
{
    switch (type) {
    case RuleType::Domain:
        return u"Domain"_s;
    case RuleType::Subdomain:
        return u"Domain and subdomains"_s;
    case RuleType::Glob:
        return u"Wildcard (glob)"_s;
    case RuleType::Regex:
        return u"Regular expression"_s;
    }
    return {};
}

QString ruleTypeHelp(RuleType type)
{
    switch (type) {
    case RuleType::Domain:
        return u"The link's host equals the pattern, ignoring case, port and credentials, e.g. docs.example.org."_s;
    case RuleType::Subdomain:
        return u"The host is the pattern or any subdomain of it: example.org matches example.org and a.b.example.org, "
               u"but not notexample.org."_s;
    case RuleType::Glob:
        return u"Matches the whole link, case-sensitively. * matches anything including /, ? matches one character, \\ "
               u"escapes. Example: https://github.com/my-org/*"_s;
    case RuleType::Regex:
        return u"POSIX extended regular expression searched anywhere in the link; use ^ and $ to anchor. Example: "
               u"^https://(www\\.)?youtube\\.com/"_s;
    }
    return {};
}

// "firefox --new-window {url}" for display only; arguments with spaces are quoted.
QString commandDisplay(const CommandTemplate &command)
{
    QStringList parts{command.program};
    for (const QString &argument : command.arguments) {
        parts.append(argument.contains(u' ') || argument.isEmpty() ? u"“%1”"_s.arg(argument) : argument);
    }
    return parts.join(u' ');
}

QPushButton *sideButton(const QString &objectName, const QString &icon, const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(QIcon::fromTheme(icon), text, parent);
    button->setObjectName(objectName);
    return button;
}

QLabel *descriptionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *subduedLabel(const QString &text, QWidget *parent)
{
    QLabel *label = descriptionLabel(text, parent);
    label->setForegroundRole(QPalette::PlaceholderText); // follows theme changes
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(const Context &context, QWidget *parent)
    : KPageDialog(parent)
    , m_store(context.store)
    , m_catalog(context.detected, context.config.browsers)
    , m_detected(context.detected)
    , m_rules(context.config.rules)
    , m_privacy(context.config.privacy)
    , m_dataDirs(context.dataDirs)
    , m_self(context.self)
{
    setObjectName(u"settingsDialog"_s);
    setWindowTitle(u"Configure — %1"_s.arg(AppDisplayName));
    setWindowIcon(appIcon());
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::apply);

    m_message = new KMessageWidget(this);
    m_message->setObjectName(u"settingsMessage"_s);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setWordWrap(true);
    m_message->setCloseButtonVisible(true);
    m_message->hide();
    auto *reloadAction = new QAction(QIcon::fromTheme(u"view-refresh"_s), u"Reload from Disk"_s, m_message);
    connect(reloadAction, &QAction::triggered, this, &SettingsDialog::reload);
    m_message->addAction(reloadAction);
    // KPageDialog shows its button box as the page footer; keep it under the message.
    auto *footer = new QWidget(this);
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->addWidget(m_message);
    if (QWidget *buttons = pageWidget()->pageFooter()) {
        footerLayout->addWidget(buttons);
    }
    pageWidget()->setPageFooter(footer);

    const auto add = [this](const QString &key, QWidget *page, const QString &name, const QString &header, const QString &icon) {
        KPageWidgetItem *item = addPage(page, name);
        item->setHeader(header);
        const QStringList names = icon.split(u',');
        QIcon themed;
        for (const QString &name : names) {
            if (themed.isNull() && QIcon::hasThemeIcon(name)) {
                themed = QIcon::fromTheme(name);
            }
        }
        item->setIcon(themed);
        m_pages.insert(key, item);
    };
    add(u"browsers"_s, createBrowsersPage(), u"Browsers"_s, u"Browsers in the chooser"_s, u"internet-web-browser"_s);
    add(u"rules"_s, createRulesPage(), u"Routing Rules"_s, u"Open matching links without asking"_s, u"view-filter"_s);
    add(u"privacy"_s, createPrivacyPage(), u"Privacy"_s, u"Link preprocessing"_s, u"preferences-system-privacy,security-high"_s);
    add(u"about"_s, createAboutPage(), u"About"_s, AppDisplayName.toString(), u"help-about"_s);

    refreshBrowsers();
    refreshRules(m_rules.isEmpty() ? -1 : 0);
    resize(900, 680);

    if (!context.loadError.isEmpty()) {
        showMessage(u"%1\nSaving is disabled until the file is fixed or moved away, so that nothing in it is lost."_s.arg(context.loadError),
                    KMessageWidget::Error);
        setSavingAllowed(false);
    }
}

QWidget *SettingsDialog::createBrowsersPage()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(subduedLabel(u"The chooser lists browsers in this order and numbers the first nine. Hidden browsers are "
                                 u"left out of the chooser but can still be used by routing rules."_s,
                                 page));

    auto *row = new QHBoxLayout;
    m_browserTable = new QTreeWidget(page);
    m_browserTable->setObjectName(u"browserTable"_s);
    m_browserTable->setAccessibleName(u"Browsers"_s);
    m_browserTable->setRootIsDecorated(false);
    m_browserTable->setUniformRowHeights(true);
    m_browserTable->setAlternatingRowColors(true);
    m_browserTable->setIconSize(QSize(24, 24));
    m_browserTable->setHeaderLabels({u"Browser"_s, u"Command"_s, u"Status"_s});
    m_browserTable->header()->setSectionResizeMode(BrowserNameColumn, QHeaderView::ResizeToContents);
    m_browserTable->header()->setSectionResizeMode(BrowserCommandColumn, QHeaderView::Stretch);
    m_browserTable->header()->setSectionResizeMode(BrowserStatusColumn, QHeaderView::ResizeToContents);
    connect(m_browserTable, &QTreeWidget::currentItemChanged, this, &SettingsDialog::updateBrowserButtons);
    connect(m_browserTable, &QTreeWidget::itemActivated, this, &SettingsDialog::editBrowser);
    row->addWidget(m_browserTable, 1);

    auto *buttons = new QVBoxLayout;
    auto *add = sideButton(u"addBrowserButton"_s, u"list-add"_s, u"Add…"_s, page);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addBrowser);
    buttons->addWidget(add);
    m_editBrowser = sideButton(u"editBrowserButton"_s, u"document-edit"_s, u"Edit…"_s, page);
    connect(m_editBrowser, &QPushButton::clicked, this, &SettingsDialog::editBrowser);
    buttons->addWidget(m_editBrowser);
    m_hideBrowser = sideButton(u"hideBrowserButton"_s, u"view-hidden"_s, u"Hide"_s, page);
    connect(m_hideBrowser, &QPushButton::clicked, this, &SettingsDialog::toggleHidden);
    buttons->addWidget(m_hideBrowser);
    buttons->addSpacing(12);
    m_browserUp = sideButton(u"moveBrowserUpButton"_s, u"go-up"_s, u"Move Up"_s, page);
    connect(m_browserUp, &QPushButton::clicked, this, [this] {
        moveBrowser(-1);
    });
    buttons->addWidget(m_browserUp);
    m_browserDown = sideButton(u"moveBrowserDownButton"_s, u"go-down"_s, u"Move Down"_s, page);
    connect(m_browserDown, &QPushButton::clicked, this, [this] {
        moveBrowser(1);
    });
    buttons->addWidget(m_browserDown);
    buttons->addSpacing(12);
    m_resetBrowser = sideButton(u"resetBrowserButton"_s, u"edit-reset"_s, u"Reset"_s, page);
    m_resetBrowser->setToolTip(u"Use the name, icon and command from the browser's desktop file again"_s);
    connect(m_resetBrowser, &QPushButton::clicked, this, &SettingsDialog::resetBrowser);
    buttons->addWidget(m_resetBrowser);
    m_removeBrowser = sideButton(u"removeBrowserButton"_s, u"list-remove"_s, u"Remove…"_s, page);
    connect(m_removeBrowser, &QPushButton::clicked, this, &SettingsDialog::removeBrowser);
    buttons->addWidget(m_removeBrowser);
    buttons->addStretch(1);
    auto *rescanButton = sideButton(u"rescanButton"_s, u"view-refresh"_s, u"Rescan"_s, page);
    rescanButton->setToolTip(u"Look for installed browsers again; your order and changes are kept"_s);
    connect(rescanButton, &QPushButton::clicked, this, &SettingsDialog::rescan);
    buttons->addWidget(rescanButton);
    row->addLayout(buttons);
    root->addLayout(row, 1);
    return page;
}

QWidget *SettingsDialog::createRulesPage()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(subduedLabel(u"Rules are checked from the top against the link after privacy preprocessing. The first "
                                 u"enabled rule that matches opens the link without showing the chooser."_s,
                                 page));

    auto *row = new QHBoxLayout;
    m_ruleTable = new QTreeWidget(page);
    m_ruleTable->setObjectName(u"ruleTable"_s);
    m_ruleTable->setAccessibleName(u"Routing rules"_s);
    m_ruleTable->setRootIsDecorated(false);
    m_ruleTable->setUniformRowHeights(true);
    m_ruleTable->setHeaderLabels({u"On"_s, u"Type"_s, u"Pattern"_s, u"Browser"_s, u"Problem"_s});
    m_ruleTable->header()->setSectionResizeMode(RuleEnabledColumn, QHeaderView::ResizeToContents);
    m_ruleTable->header()->setSectionResizeMode(RuleTypeColumn, QHeaderView::ResizeToContents);
    m_ruleTable->header()->setSectionResizeMode(RulePatternColumn, QHeaderView::Stretch);
    m_ruleTable->header()->setSectionResizeMode(RuleBrowserColumn, QHeaderView::ResizeToContents);
    m_ruleTable->header()->setSectionResizeMode(RuleProblemColumn, QHeaderView::ResizeToContents);
    connect(m_ruleTable, &QTreeWidget::currentItemChanged, this, [this] {
        if (!m_loadingEditor) {
            loadRuleEditor();
        }
    });
    connect(m_ruleTable, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        const int row = m_ruleTable->indexOfTopLevelItem(item);
        if (m_loadingEditor || column != RuleEnabledColumn || row < 0) {
            return;
        }
        m_rules[row].enabled = item->checkState(RuleEnabledColumn) == Qt::Checked;
        if (row == selectedRule()) {
            loadRuleEditor();
        }
        updateTestResult();
    });
    row->addWidget(m_ruleTable, 1);

    auto *buttons = new QVBoxLayout;
    auto *add = sideButton(u"addRuleButton"_s, u"list-add"_s, u"Add"_s, page);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addRule);
    buttons->addWidget(add);
    m_removeRule = sideButton(u"removeRuleButton"_s, u"list-remove"_s, u"Remove"_s, page);
    connect(m_removeRule, &QPushButton::clicked, this, &SettingsDialog::removeRule);
    buttons->addWidget(m_removeRule);
    buttons->addSpacing(12);
    m_ruleUp = sideButton(u"moveRuleUpButton"_s, u"go-up"_s, u"Move Up"_s, page);
    connect(m_ruleUp, &QPushButton::clicked, this, [this] {
        moveRule(-1);
    });
    buttons->addWidget(m_ruleUp);
    m_ruleDown = sideButton(u"moveRuleDownButton"_s, u"go-down"_s, u"Move Down"_s, page);
    connect(m_ruleDown, &QPushButton::clicked, this, [this] {
        moveRule(1);
    });
    buttons->addWidget(m_ruleDown);
    buttons->addStretch(1);
    row->addLayout(buttons);
    root->addLayout(row, 1);

    m_ruleEditor = new QGroupBox(u"Selected rule"_s, page);
    m_ruleEditor->setObjectName(u"ruleEditor"_s);
    auto *form = new QFormLayout(m_ruleEditor);
    m_ruleName = new QLineEdit(m_ruleEditor);
    m_ruleName->setObjectName(u"ruleNameEdit"_s);
    m_ruleName->setPlaceholderText(u"Optional label"_s);
    form->addRow(u"&Name:"_s, m_ruleName);
    m_ruleEnabled = new QCheckBox(u"&Enabled"_s, m_ruleEditor);
    m_ruleEnabled->setObjectName(u"ruleEnabledCheck"_s);
    form->addRow(QString(), m_ruleEnabled);
    m_ruleType = new QComboBox(m_ruleEditor);
    m_ruleType->setObjectName(u"ruleTypeCombo"_s);
    for (RuleType type : {RuleType::Domain, RuleType::Subdomain, RuleType::Glob, RuleType::Regex}) {
        m_ruleType->addItem(ruleTypeLabel(type), ruleTypeToString(type));
    }
    form->addRow(u"&Match:"_s, m_ruleType);
    m_rulePattern = new QLineEdit(m_ruleEditor);
    m_rulePattern->setObjectName(u"rulePatternEdit"_s);
    form->addRow(u"&Pattern:"_s, m_rulePattern);
    auto *typeHelp = subduedLabel(QString(), m_ruleEditor);
    typeHelp->setObjectName(u"ruleTypeHelp"_s);
    // Room for two lines, so switching types does not make the form jump.
    typeHelp->setMinimumHeight(typeHelp->fontMetrics().lineSpacing() * 2 + 2);
    typeHelp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum); // use the full field width
    form->addRow(QString(), typeHelp);
    m_ruleBrowser = new QComboBox(m_ruleEditor);
    m_ruleBrowser->setObjectName(u"ruleBrowserCombo"_s);
    m_ruleBrowser->setAccessibleName(u"Target browser"_s);
    form->addRow(u"&Open with:"_s, m_ruleBrowser);
    m_ruleProblem = new KMessageWidget(m_ruleEditor);
    m_ruleProblem->setObjectName(u"ruleProblemMessage"_s);
    m_ruleProblem->setTextFormat(Qt::PlainText);
    m_ruleProblem->setWordWrap(true);
    m_ruleProblem->setCloseButtonVisible(false);
    m_ruleProblem->setMessageType(KMessageWidget::Error);
    m_ruleProblem->hide();
    form->addRow(m_ruleProblem);
    root->addWidget(m_ruleEditor);

    const auto edited = [this, typeHelp] {
        typeHelp->setText(ruleTypeHelp(ruleTypeFromString(m_ruleType->currentData().toString()).value_or(RuleType::Domain)));
        storeRuleEditor();
    };
    connect(m_ruleName, &QLineEdit::textChanged, this, edited);
    connect(m_ruleEnabled, &QCheckBox::toggled, this, edited);
    connect(m_ruleType, &QComboBox::currentIndexChanged, this, edited);
    connect(m_rulePattern, &QLineEdit::textChanged, this, edited);
    connect(m_ruleBrowser, &QComboBox::currentIndexChanged, this, edited);

    auto *test = new QGroupBox(u"Test a link"_s, page);
    auto *testLayout = new QVBoxLayout(test);
    m_testUrl = new QLineEdit(test);
    m_testUrl->setObjectName(u"testUrlEdit"_s);
    m_testUrl->setPlaceholderText(u"https://example.org/page"_s);
    m_testUrl->setClearButtonEnabled(true);
    m_testUrl->setAccessibleName(u"Link to test"_s);
    testLayout->addWidget(m_testUrl);
    m_testResult = new QLabel(test);
    m_testResult->setObjectName(u"testResultLabel"_s);
    m_testResult->setTextFormat(Qt::PlainText);
    m_testResult->setWordWrap(true);
    m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    testLayout->addWidget(m_testResult);
    connect(m_testUrl, &QLineEdit::textChanged, this, &SettingsDialog::updateTestResult);
    root->addWidget(test);
    return page;
}

QWidget *SettingsDialog::createPrivacyPage()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    m_stripTracking = new QCheckBox(u"&Remove tracking parameters from links"_s, page);
    m_stripTracking->setObjectName(u"stripTrackingCheck"_s);
    m_stripTracking->setChecked(m_privacy.stripTrackingParameters);
    connect(m_stripTracking, &QCheckBox::toggled, this, [this](bool on) {
        m_privacy.stripTrackingParameters = on;
        updateTestResult();
    });
    root->addWidget(m_stripTracking);
    auto *stripHelp = subduedLabel(u"Removes known tracking parameters such as utm_source, fbclid, gclid and msclkid. It "
                                   u"happens before rules are checked, before every redirect request and before the browser "
                                   u"is started. All other parts of the link are kept exactly as they were."_s,
                                   page);
    stripHelp->setContentsMargins(24, 0, 0, 12);
    root->addWidget(stripHelp);

    m_unwrapRedirects = new QCheckBox(u"&Follow redirects before choosing a browser"_s, page);
    m_unwrapRedirects->setObjectName(u"unwrapRedirectsCheck"_s);
    m_unwrapRedirects->setChecked(m_privacy.unwrapRedirects);
    connect(m_unwrapRedirects, &QCheckBox::toggled, this, [this](bool on) {
        m_privacy.unwrapRedirects = on;
    });
    root->addWidget(m_unwrapRedirects);
    auto *notice = descriptionLabel(
        u"Privacy notice: when this is on, the link's server is contacted in the background before you choose a browser, "
        u"which tells it that the link was opened. At most three redirects are followed; each request times out after 5 "
        u"seconds and the whole check after 15. Only response headers are read, cookies and stored credentials are not "
        u"sent, and links are only followed over http and https without downgrading from https. The chooser can skip the "
        u"check at any time; if it fails, the last safe link is offered."_s,
        page);
    notice->setObjectName(u"privacyNotice"_s);
    notice->setContentsMargins(24, 0, 0, 0);
    root->addWidget(notice);
    root->addStretch(1);
    return page;
}

QWidget *SettingsDialog::createAboutPage()
{
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel(page);
    icon->setPixmap(appIcon().pixmap(QSize(64, 64), devicePixelRatioF()));
    header->addWidget(icon);
    auto *title = new QLabel(u"%1 %2"_s.arg(AppDisplayName, QStringLiteral(KBS_VERSION)), page);
    QFont font = title->font();
    font.setPointSizeF(font.pointSizeF() * 1.4);
    font.setWeight(QFont::DemiBold);
    title->setFont(font);
    header->addWidget(title, 1);
    root->addLayout(header);
    root->addWidget(descriptionLabel(u"Opens links in the browser chosen by your rules, or asks which one to use."_s, page));

    auto *form = new QFormLayout;
    const auto value = [page](const QString &text) {
        auto *label = new QLabel(text, page);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    form->addRow(u"Configuration file:"_s, value(m_store ? m_store->path() : QString()));
    form->addRow(u"Desktop file:"_s, value(AppId.toString() + u".desktop"_s));
    form->addRow(u"Command:"_s, value(u"kde-browser-switcher [--settings] [--choose] [--config FILE] [URL…]"_s));
    root->addLayout(form);
    root->addWidget(subduedLabel(u"No browsing history is stored and nothing is sent anywhere unless redirect following is "
                                 u"turned on."_s,
                                 page));
    root->addStretch(1);
    return page;
}

// Records every browser the dialog shows, so newly found ones keep the position
// they were shown at. Browsers without settings are always listed after those
// with settings, so appending them keeps the order, and settings of browsers
// that are not installed (and removal markers) stay where they were.
Config SettingsDialog::draftConfig() const
{
    QList<BrowserSetting> browsers = m_catalog.settings();
    for (const Browser &browser : m_catalog.browsers()) {
        if (std::ranges::find(browsers, browser.id, &BrowserSetting::id) == browsers.cend()) {
            BrowserSetting setting;
            setting.id = browser.id;
            browsers.append(setting);
        }
    }
    return {browsers, m_rules, m_privacy};
}

QList<DetectedBrowser> SettingsDialog::detectedBrowsers() const
{
    return m_detected;
}

QStringList SettingsDialog::blockingProblems() const
{
    QStringList problems;
    for (qsizetype i = 0; i < m_rules.size(); ++i) {
        const Rule &rule = m_rules[i];
        if (!rule.enabled) {
            continue;
        }
        const RuleMatcher matcher(rule);
        if (!matcher.isValid()) {
            problems.append(u"Rule %1: %2 Fix the pattern or disable the rule."_s.arg(i + 1).arg(matcher.error()));
        } else if (rule.browserId.isEmpty()) {
            problems.append(u"Rule %1: no browser is selected. Choose one or disable the rule."_s.arg(i + 1));
        }
    }
    return problems;
}

bool SettingsDialog::apply()
{
    if (!m_savingAllowed || !m_store) {
        if (!m_message->isVisible()) {
            showMessage(u"Saving is disabled."_s, KMessageWidget::Error);
        }
        return false;
    }
    const QStringList problems = blockingProblems();
    if (!problems.isEmpty()) {
        showMessage(problems.join(u'\n'), KMessageWidget::Error);
        return false;
    }
    const Config config = draftConfig();
    const ConfigStore::SaveResult result = m_store->save(config);
    if (!result.ok) {
        showMessage(result.error, KMessageWidget::Error);
        return false;
    }
    // Later rescans start from the order just saved, as if the dialog had been reopened.
    m_catalog = BrowserCatalog(m_detected, config.browsers);
    showMessage(u"Settings saved."_s, KMessageWidget::Positive);
    Q_EMIT saved(config, m_detected);
    return true;
}

void SettingsDialog::accept()
{
    if (apply()) {
        KPageDialog::accept();
    }
}

void SettingsDialog::showPage(const QString &name)
{
    if (KPageWidgetItem *item = m_pages.value(name)) {
        setCurrentPage(item);
    }
}

void SettingsDialog::showMessage(const QString &text, int type)
{
    m_message->setMessageType(KMessageWidget::MessageType(type));
    m_message->setText(text);
    // Reloading only makes sense when the file on disk is the problem.
    const bool fileProblem = type == KMessageWidget::Error && m_store;
    for (QAction *action : m_message->actions()) {
        action->setVisible(fileProblem);
    }
    m_message->show();
}

void SettingsDialog::setSavingAllowed(bool allowed)
{
    m_savingAllowed = allowed;
    button(QDialogButtonBox::Ok)->setEnabled(allowed);
    button(QDialogButtonBox::Apply)->setEnabled(allowed);
}

void SettingsDialog::reload()
{
    if (!m_store) {
        return;
    }
    const ConfigStore::LoadResult loaded = m_store->load();
    m_catalog = BrowserCatalog(m_detected, loaded.config.browsers);
    m_rules = loaded.config.rules;
    m_privacy = loaded.config.privacy;
    m_stripTracking->setChecked(m_privacy.stripTrackingParameters);
    m_unwrapRedirects->setChecked(m_privacy.unwrapRedirects);
    refreshBrowsers();
    refreshRules(m_rules.isEmpty() ? -1 : 0);
    if (loaded.status == ConfigStore::LoadStatus::Invalid) {
        showMessage(u"%1\nSaving is disabled until the file is fixed or moved away, so that nothing in it is lost."_s.arg(loaded.error),
                    KMessageWidget::Error);
        setSavingAllowed(false);
    } else {
        setSavingAllowed(true);
        showMessage(u"Reloaded the configuration file; unsaved changes were discarded."_s, KMessageWidget::Information);
    }
}

// Browsers page

QString SettingsDialog::selectedBrowserId() const
{
    const QTreeWidgetItem *item = m_browserTable->currentItem();
    return item ? item->data(BrowserNameColumn, Qt::UserRole).toString() : QString();
}

void SettingsDialog::refreshBrowsers(const QString &selectId)
{
    const QString keep = selectId.isEmpty() ? selectedBrowserId() : selectId;
    const QSignalBlocker blocker(m_browserTable);
    m_browserTable->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const Browser &browser : m_catalog.browsers()) {
        auto *item = new QTreeWidgetItem(m_browserTable);
        item->setData(BrowserNameColumn, Qt::UserRole, browser.id);
        item->setText(BrowserNameColumn, browser.name);
        item->setIcon(BrowserNameColumn, browserIcon(browser.icon));
        item->setText(BrowserCommandColumn, commandDisplay(browser.command));
        item->setToolTip(BrowserCommandColumn, commandDisplay(browser.command).toHtmlEscaped());

        QStringList status;
        if (browser.hidden) {
            status.append(u"Hidden"_s);
        }
        if (browser.source == BrowserSource::Manual) {
            status.append(u"Added manually"_s);
        } else if (browser.customized) {
            status.append(u"Customised"_s);
        }
        const CommandValidation validation = validateCommand(browser.command, m_self);
        const QStringList problems = validation.errors + validation.warnings;
        status += problems;
        item->setText(BrowserStatusColumn, status.join(u" · "_s));
        item->setToolTip(BrowserStatusColumn, status.join(u'\n').toHtmlEscaped());
        if (!problems.isEmpty()) {
            item->setIcon(BrowserStatusColumn, QIcon::fromTheme(validation.ok() ? u"dialog-warning"_s : u"dialog-error"_s));
        } else if (browser.hidden) {
            item->setIcon(BrowserStatusColumn, QIcon::fromTheme(u"view-hidden"_s));
        }
        if (browser.hidden) {
            item->setForeground(BrowserNameColumn, subduedTextColor(palette()));
        }
        if (browser.id == keep) {
            selected = item;
        }
    }
    if (!selected && m_browserTable->topLevelItemCount() > 0) {
        selected = m_browserTable->topLevelItem(0);
    }
    m_browserTable->setCurrentItem(selected);
    updateBrowserButtons();
    // Rule targets show browser names.
    if (m_ruleTable) {
        refreshRules(selectedRule());
    }
}

void SettingsDialog::updateBrowserButtons()
{
    const auto browser = m_catalog.browser(selectedBrowserId());
    const int row = m_browserTable->currentItem() ? m_browserTable->indexOfTopLevelItem(m_browserTable->currentItem()) : -1;
    m_editBrowser->setEnabled(browser.has_value());
    m_hideBrowser->setEnabled(browser.has_value());
    m_hideBrowser->setText(browser && browser->hidden ? u"Show"_s : u"Hide"_s);
    m_hideBrowser->setIcon(QIcon::fromTheme(browser && browser->hidden ? u"view-visible"_s : u"view-hidden"_s));
    m_browserUp->setEnabled(row > 0);
    m_browserDown->setEnabled(row >= 0 && row < m_browserTable->topLevelItemCount() - 1);
    m_resetBrowser->setEnabled(browser && browser->source == BrowserSource::Detected && browser->customized);
    m_removeBrowser->setEnabled(browser.has_value());
}

void SettingsDialog::addBrowser()
{
    auto *editor = new BrowserEditDialog({QString(), QString(), {QString(), {UrlPlaceholder.toString()}}}, m_self, this);
    connect(editor, &QDialog::finished, editor, &QObject::deleteLater);
    connect(editor, &QDialog::accepted, this, [this, editor] {
        refreshBrowsers(m_catalog.addManual(editor->fields()));
    });
    editor->open();
}

void SettingsDialog::editBrowser()
{
    const QString id = selectedBrowserId();
    const auto browser = m_catalog.browser(id);
    if (!browser) {
        return;
    }
    auto *editor = new BrowserEditDialog({browser->name, browser->icon, browser->command}, m_self, this);
    connect(editor, &QDialog::finished, editor, &QObject::deleteLater);
    connect(editor, &QDialog::accepted, this, [this, editor, id] {
        m_catalog.edit(id, editor->fields());
        refreshBrowsers(id);
    });
    editor->open();
}

void SettingsDialog::toggleHidden()
{
    const QString id = selectedBrowserId();
    if (const auto browser = m_catalog.browser(id)) {
        m_catalog.setHidden(id, !browser->hidden);
        refreshBrowsers(id);
    }
}

void SettingsDialog::moveBrowser(int delta)
{
    const QString id = selectedBrowserId();
    QStringList ids;
    for (const Browser &browser : m_catalog.browsers()) {
        ids.append(browser.id);
    }
    const qsizetype from = ids.indexOf(id);
    const qsizetype to = from + delta;
    if (from < 0 || to < 0 || to >= ids.size()) {
        return;
    }
    ids.swapItemsAt(from, to);
    m_catalog.reorder(ids);
    refreshBrowsers(id);
}

void SettingsDialog::resetBrowser()
{
    const QString id = selectedBrowserId();
    m_catalog.resetToDefaults(id);
    refreshBrowsers(id);
}

void SettingsDialog::removeBrowser()
{
    const QString id = selectedBrowserId();
    const auto browser = m_catalog.browser(id);
    if (!browser) {
        return;
    }
    const auto users = std::ranges::count(m_rules, id, &Rule::browserId);
    QString text = browser->source == BrowserSource::Detected
        ? u"Remove “%1”? It will not come back when browsers are rescanned. To only take it out of the chooser, hide it instead."_s.arg(browser->name)
        : u"Remove “%1”?"_s.arg(browser->name);
    if (users > 0) {
        text += u"\n\n%1 routing rule(s) use this browser and will stop working until they are given another one."_s.arg(users);
    }
    auto *box = new QMessageBox(QMessageBox::Question, u"Remove Browser"_s, text, QMessageBox::NoButton, this);
    box->setTextFormat(Qt::PlainText);
    QPushButton *remove = box->addButton(u"Remove"_s, QMessageBox::DestructiveRole);
    remove->setObjectName(u"confirmRemoveButton"_s);
    remove->setIcon(QIcon::fromTheme(u"edit-delete"_s));
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QDialog::finished, box, &QObject::deleteLater);
    connect(box, &QMessageBox::buttonClicked, this, [this, remove, id](QAbstractButton *clicked) {
        if (clicked == remove) {
            m_catalog.remove(id);
            refreshBrowsers();
        }
    });
    box->open();
}

void SettingsDialog::rescan()
{
    const DiscoveryResult result = discoverBrowsers(m_dataDirs, m_self);
    m_detected = result.browsers;
    m_catalog.setDetected(m_detected);
    refreshBrowsers();
    QString text = u"Found %1 installed browser(s)."_s.arg(result.browsers.size());
    if (!result.warnings.isEmpty()) {
        text += u" %1 desktop entr%2 could not be used:\n%3"_s.arg(result.warnings.size())
                    .arg(result.warnings.size() == 1 ? u"y"_s : u"ies"_s, result.warnings.join(u'\n'));
    }
    showMessage(text, KMessageWidget::Information);
}

// Rules page

int SettingsDialog::selectedRule() const
{
    QTreeWidgetItem *item = m_ruleTable->currentItem();
    return item ? m_ruleTable->indexOfTopLevelItem(item) : -1;
}

void SettingsDialog::refreshRules(int selectRow)
{
    m_loadingEditor = true;
    m_ruleTable->clear();
    const RuleEngine engine(m_rules, m_catalog.browserIds());
    for (qsizetype i = 0; i < m_rules.size(); ++i) {
        const Rule &rule = m_rules[i];
        auto *item = new QTreeWidgetItem(m_ruleTable);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(RuleEnabledColumn, rule.enabled ? Qt::Checked : Qt::Unchecked);
        item->setText(RuleTypeColumn, ruleTypeLabel(rule.type));
        item->setText(RulePatternColumn, rule.pattern);
        item->setToolTip(RulePatternColumn, rule.name.toHtmlEscaped());
        if (const auto browser = m_catalog.browser(rule.browserId)) {
            item->setText(RuleBrowserColumn, browser->hidden ? u"%1 (hidden)"_s.arg(browser->name) : browser->name);
            item->setIcon(RuleBrowserColumn, browserIcon(browser->icon));
        } else {
            item->setText(RuleBrowserColumn, rule.browserId.isEmpty() ? u"None"_s : u"%1 (missing)"_s.arg(rule.browserId));
        }
        const QString problem = engine.ruleError(i);
        item->setText(RuleProblemColumn, problem);
        item->setToolTip(RuleProblemColumn, problem.toHtmlEscaped());
        if (!problem.isEmpty()) {
            item->setIcon(RuleProblemColumn, QIcon::fromTheme(rule.enabled ? u"dialog-error"_s : u"dialog-warning"_s));
        }
    }
    if (selectRow >= 0 && selectRow < m_ruleTable->topLevelItemCount()) {
        m_ruleTable->setCurrentItem(m_ruleTable->topLevelItem(selectRow));
    }
    m_loadingEditor = false;
    loadRuleEditor();
}

void SettingsDialog::loadRuleEditor()
{
    const int row = selectedRule();
    const bool valid = row >= 0 && row < m_rules.size();
    m_ruleEditor->setEnabled(valid);
    m_removeRule->setEnabled(valid);
    m_ruleUp->setEnabled(valid && row > 0);
    m_ruleDown->setEnabled(valid && row < m_rules.size() - 1);

    m_loadingEditor = true;
    const Rule rule = valid ? m_rules[row] : Rule();
    m_ruleName->setText(rule.name);
    m_ruleEnabled->setChecked(rule.enabled);
    m_ruleType->setCurrentIndex(std::max(0, m_ruleType->findData(ruleTypeToString(rule.type))));
    m_rulePattern->setText(rule.pattern);

    m_ruleBrowser->clear();
    if (rule.browserId.isEmpty()) {
        m_ruleBrowser->addItem(u"Choose a browser…"_s, QString());
    }
    for (const Browser &browser : m_catalog.browsers()) {
        m_ruleBrowser->addItem(browserIcon(browser.icon), browser.hidden ? u"%1 (hidden)"_s.arg(browser.name) : browser.name, browser.id);
    }
    if (!rule.browserId.isEmpty() && m_ruleBrowser->findData(rule.browserId) < 0) {
        // Keep showing the real target instead of silently picking another browser.
        m_ruleBrowser->addItem(QIcon::fromTheme(u"dialog-warning"_s), u"Missing browser: %1"_s.arg(rule.browserId), rule.browserId);
    }
    m_ruleBrowser->setCurrentIndex(std::max(0, m_ruleBrowser->findData(rule.browserId)));
    m_loadingEditor = false;

    if (auto *help = findChild<QLabel *>(u"ruleTypeHelp"_s)) {
        help->setText(ruleTypeHelp(rule.type));
    }
    updateRuleProblem();
    updateTestResult();
}

void SettingsDialog::storeRuleEditor()
{
    const int row = selectedRule();
    if (m_loadingEditor || row < 0 || row >= m_rules.size()) {
        return;
    }
    Rule &rule = m_rules[row];
    rule.name = m_ruleName->text();
    rule.enabled = m_ruleEnabled->isChecked();
    rule.type = ruleTypeFromString(m_ruleType->currentData().toString()).value_or(RuleType::Domain);
    rule.pattern = m_rulePattern->text();
    rule.browserId = m_ruleBrowser->currentData().toString();

    // Update the row in place so editing keeps focus and selection.
    m_loadingEditor = true;
    QTreeWidgetItem *item = m_ruleTable->topLevelItem(row);
    item->setCheckState(RuleEnabledColumn, rule.enabled ? Qt::Checked : Qt::Unchecked);
    item->setText(RuleTypeColumn, ruleTypeLabel(rule.type));
    item->setText(RulePatternColumn, rule.pattern);
    item->setToolTip(RulePatternColumn, rule.name.toHtmlEscaped());
    if (const auto browser = m_catalog.browser(rule.browserId)) {
        item->setText(RuleBrowserColumn, browser->hidden ? u"%1 (hidden)"_s.arg(browser->name) : browser->name);
        item->setIcon(RuleBrowserColumn, browserIcon(browser->icon));
    } else {
        item->setText(RuleBrowserColumn, rule.browserId.isEmpty() ? u"None"_s : u"%1 (missing)"_s.arg(rule.browserId));
        item->setIcon(RuleBrowserColumn, QIcon());
    }
    m_loadingEditor = false;
    updateRuleProblem();
    updateTestResult();
}

void SettingsDialog::updateRuleProblem()
{
    const RuleEngine engine(m_rules, m_catalog.browserIds());
    for (int i = 0; i < m_ruleTable->topLevelItemCount(); ++i) {
        const QString problem = engine.ruleError(i);
        QTreeWidgetItem *item = m_ruleTable->topLevelItem(i);
        item->setText(RuleProblemColumn, problem);
        item->setToolTip(RuleProblemColumn, problem.toHtmlEscaped());
        item->setIcon(RuleProblemColumn, problem.isEmpty() ? QIcon()
                                                           : QIcon::fromTheme(m_rules[i].enabled ? u"dialog-error"_s : u"dialog-warning"_s));
    }
    const int row = selectedRule();
    const QString problem = row >= 0 ? engine.ruleError(row) : QString();
    if (problem.isEmpty()) {
        m_ruleProblem->hide();
    } else {
        m_ruleProblem->setMessageType(m_rules[row].enabled ? KMessageWidget::Error : KMessageWidget::Warning);
        m_ruleProblem->setText(m_rules[row].enabled ? problem : problem + u" The rule is disabled, so it is ignored."_s);
        m_ruleProblem->show();
    }
}

void SettingsDialog::updateTestResult()
{
    if (!m_testResult) {
        return;
    }
    QString url = m_testUrl->text().trimmed();
    if (url.isEmpty()) {
        m_testResult->setText(u"Enter a link to see which rule handles it."_s);
        return;
    }
    if (m_privacy.stripTrackingParameters) {
        url = stripTrackingParameters(url).url;
    }
    const RouteResult route = RuleEngine(m_rules, m_catalog.browserIds()).route(url);
    QString text;
    if (route.matched()) {
        const auto browser = m_catalog.browser(route.browserId);
        const QString name = m_rules[route.ruleIndex].name;
        text = u"Rule %1%2 opens it in %3."_s.arg(route.ruleIndex + 1)
                   .arg(name.isEmpty() ? QString() : u" (“%1”)"_s.arg(name), browser ? browser->name : route.browserId);
    } else {
        text = u"No rule matches: the chooser will be shown."_s;
    }
    if (m_privacy.unwrapRedirects) {
        text += u" Redirects are not followed when testing."_s;
    }
    m_testResult->setText(text);
}

void SettingsDialog::addRule()
{
    Rule rule;
    const QList<Browser> visible = m_catalog.visibleBrowsers();
    if (!visible.isEmpty()) {
        rule.browserId = visible.first().id;
    }
    m_rules.append(rule);
    refreshRules(int(m_rules.size()) - 1);
    showPage(u"rules"_s);
    m_rulePattern->setFocus();
}

void SettingsDialog::removeRule()
{
    const int row = selectedRule();
    if (row < 0 || row >= m_rules.size()) {
        return;
    }
    m_rules.removeAt(row);
    refreshRules(std::min(row, int(m_rules.size()) - 1));
}

void SettingsDialog::moveRule(int delta)
{
    const int row = selectedRule();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_rules.size()) {
        return;
    }
    m_rules.swapItemsAt(row, to);
    refreshRules(to);
}

} // namespace kbs
