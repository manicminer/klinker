#pragma once

#include "core/browsercatalog.h"
#include "core/config.h"

#include <KPageDialog>

class KMessageWidget;
class KPageWidgetItem;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace kbs
{

// Browsers, chooser appearance, routing rules and privacy settings. All edits go to a draft;
// nothing is written or changed elsewhere until Apply or OK succeeds, and
// Cancel discards the draft.
class SettingsDialog : public KPageDialog
{
    Q_OBJECT

public:
    struct Context {
        ConfigStore *store = nullptr; // already loaded; saves go through it
        Config config; // the active configuration
        QString loadError; // set when the file is unusable: saving is disabled
        QList<DetectedBrowser> detected;
        QStringList dataDirs; // searched again by "Rescan"
        SelfIdentity self;
    };

    explicit SettingsDialog(const Context &context, QWidget *parent = nullptr);

    // What Apply would save.
    Config draftConfig() const;
    QList<DetectedBrowser> detectedBrowsers() const;

    // Validates and saves the draft. On failure the error is shown and false
    // returned; nothing is changed.
    bool apply();

    void showPage(const QString &name); // "browsers", "appearance", "rules", "privacy" or "about"

Q_SIGNALS:
    // After a successful save, with the browsers known at that moment.
    void saved(const kbs::Config &config, const QList<kbs::DetectedBrowser> &detected);

public Q_SLOTS:
    void accept() override;

private:
    QWidget *createBrowsersPage();
    QWidget *createAppearancePage();
    QWidget *createRulesPage();
    QWidget *createPrivacyPage();
    QWidget *createAboutPage();

    void refreshBrowsers(const QString &selectId = QString());
    void refreshAppearance();
    void updateSourceRow();
    void refreshRules(int selectRow = -1);
    void updateBrowserButtons();
    void loadRuleEditor();
    void storeRuleEditor();
    void updateRuleProblem();
    void updateTestResult();
    QString selectedBrowserId() const;
    int selectedRule() const;

    void addBrowser();
    void editBrowser();
    void toggleHidden();
    void moveBrowser(int delta);
    void resetBrowser();
    void removeBrowser();
    void rescan();
    void addRule();
    void removeRule();
    void moveRule(int delta);
    void reload();

    void showMessage(const QString &text, int type);
    void setSavingAllowed(bool allowed);
    // Problems that block saving, e.g. an enabled rule with an invalid pattern.
    QStringList blockingProblems() const;

    ConfigStore *m_store;
    BrowserCatalog m_catalog;
    QList<DetectedBrowser> m_detected;
    QList<Rule> m_rules;
    PrivacySettings m_privacy;
    ChooserSettings m_chooser;
    QStringList m_dataDirs;
    SelfIdentity m_self;
    bool m_savingAllowed = true;
    bool m_loadingEditor = false;

    KMessageWidget *m_message = nullptr;
    QMap<QString, KPageWidgetItem *> m_pages;

    QTreeWidget *m_browserTable = nullptr;
    QPushButton *m_editBrowser = nullptr;
    QPushButton *m_hideBrowser = nullptr;
    QPushButton *m_browserUp = nullptr;
    QPushButton *m_browserDown = nullptr;
    QPushButton *m_resetBrowser = nullptr;
    QPushButton *m_removeBrowser = nullptr;

    QComboBox *m_chooserStyle = nullptr;
    QSpinBox *m_iconSize = nullptr;
    QCheckBox *m_showNames = nullptr;
    QCheckBox *m_closeOnFocusLoss = nullptr;

    QTreeWidget *m_ruleTable = nullptr;
    QPushButton *m_removeRule = nullptr;
    QPushButton *m_ruleUp = nullptr;
    QPushButton *m_ruleDown = nullptr;
    QGroupBox *m_ruleEditor = nullptr;
    QLineEdit *m_ruleName = nullptr;
    QCheckBox *m_ruleEnabled = nullptr;
    QComboBox *m_ruleType = nullptr;
    QLineEdit *m_rulePattern = nullptr;
    QComboBox *m_ruleBrowser = nullptr;
    KMessageWidget *m_ruleProblem = nullptr;
    QLineEdit *m_testUrl = nullptr;
    QLabel *m_testResult = nullptr;

    QFormLayout *m_aboutForm = nullptr;
    QLabel *m_legacySource = nullptr;

    QCheckBox *m_stripTracking = nullptr;
    QCheckBox *m_unwrapRedirects = nullptr;
};

} // namespace kbs
