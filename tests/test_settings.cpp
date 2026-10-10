#include "testsupport.h"
#include "ui/browsereditdialog.h"
#include "ui/settingsdialog.h"

#include "core/browserdiscovery.h"
#include "core/rules.h"

#include <KMessageWidget>
#include <KPageWidgetModel>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <memory>

using namespace Qt::StringLiterals;
using klinker::Config;
using klinker::ConfigStore;
using klinker::Rule;
using klinker::RuleType;
using klinker::SettingsDialog;

namespace
{

const QString Fixture = QStringLiteral(KLINKER_CAPTURE_FIXTURE);

template<typename T>
T *child(QObject *parent, const QString &name)
{
    T *object = parent->findChild<T *>(name);
    if (!object) {
        qWarning() << "missing child" << name;
    }
    return object;
}

template<typename T>
T *visibleChild(QObject *parent)
{
    for (T *candidate : parent->findChildren<T *>()) {
        if (candidate->isVisible()) {
            return candidate;
        }
    }
    return nullptr;
}

QStringList tableIds(QTreeWidget *table)
{
    QStringList ids;
    for (int i = 0; i < table->topLevelItemCount(); ++i) {
        ids.append(table->topLevelItem(i)->data(0, Qt::UserRole).toString());
    }
    return ids;
}

void select(QTreeWidget *table, int row)
{
    table->setCurrentItem(table->topLevelItem(row));
}

void click(QObject *parent, const QString &name)
{
    auto *button = child<QPushButton>(parent, name);
    QVERIFY2(button, qPrintable(name));
    QVERIFY2(button->isEnabled(), qPrintable(name + u" is disabled"_s));
    button->click();
}

struct SettingsFixture {
    QTemporaryDir dir;
    QString dataDir;
    QString configPath;
    klinker::SelfIdentity self{{u"io.github.manicminer.Klinker.desktop"_s}, {u"klinker"_s}, {}};
    std::unique_ptr<ConfigStore> store;
    std::unique_ptr<SettingsDialog> dialog;

    explicit SettingsFixture(const QByteArray &configJson = {})
    {
        dataDir = dir.path() + u"/data"_s;
        configPath = dir.path() + u"/config/config.json"_s;
        klinker::test::writeBrowserDesktopFile(dataDir, u"alpha.desktop"_s, u"Alpha"_s, Fixture + u" %u"_s, u"firefox"_s);
        klinker::test::writeBrowserDesktopFile(dataDir, u"beta.desktop"_s, u"Beta"_s, u"/nonexistent/beta-browser %u"_s, u"chromium"_s);
        klinker::test::writeBrowserDesktopFile(dataDir, u"gamma.desktop"_s, u"Gamma"_s, Fixture + u" --gamma %u"_s);
        if (!configJson.isEmpty()) {
            writeConfig(configJson);
        }
        open();
    }

    void writeConfig(const QByteArray &json) const
    {
        QDir().mkpath(QFileInfo(configPath).absolutePath());
        QFile file(configPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(json);
    }

    void open()
    {
        dialog.reset();
        store = std::make_unique<ConfigStore>(configPath);
        const ConfigStore::LoadResult loaded = store->load();
        SettingsDialog::Context context;
        context.store = store.get();
        context.config = loaded.config;
        context.loadError = loaded.status == ConfigStore::LoadStatus::Invalid ? loaded.error : QString();
        context.detected = klinker::discoverBrowsers({dataDir}, self).browsers;
        context.dataDirs = {dataDir};
        context.self = self;
        dialog = std::make_unique<SettingsDialog>(context);
        dialog->show();
    }

    QByteArray fileBytes() const
    {
        QFile file(configPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    Config savedConfig() const
    {
        const auto parsed = klinker::configFromJson(fileBytes());
        if (!parsed.config) {
            qWarning() << "saved config does not parse:" << parsed.error;
            return {};
        }
        return *parsed.config;
    }

    QTreeWidget *browsers() const
    {
        return child<QTreeWidget>(dialog.get(), u"browserTable"_s);
    }

    QTreeWidget *rules() const
    {
        return child<QTreeWidget>(dialog.get(), u"ruleTable"_s);
    }

    void showRule(int row) const
    {
        dialog->showPage(u"rules"_s);
        select(rules(), row);
    }

    KMessageWidget *message() const
    {
        return child<KMessageWidget>(dialog.get(), u"settingsMessage"_s);
    }

    QPushButton *button(QDialogButtonBox::StandardButton which) const
    {
        return dialog->findChild<QDialogButtonBox *>()->button(which);
    }

    bool canApply() const
    {
        return button(QDialogButtonBox::Apply)->isEnabled();
    }
};

// Fills and accepts the browser editor that the settings dialog opened.
void fillEditor(SettingsDialog *dialog, const QString &name, const QString &program, const QString &arguments,
                const QString &icon = QString())
{
    QPointer<klinker::BrowserEditDialog> editor;
    QTRY_VERIFY((editor = visibleChild<klinker::BrowserEditDialog>(dialog)));
    child<QLineEdit>(editor, u"nameEdit"_s)->setText(name);
    child<QLineEdit>(editor, u"programEdit"_s)->setText(program);
    child<QPlainTextEdit>(editor, u"argumentsEdit"_s)->setPlainText(arguments);
    child<QLineEdit>(editor, u"iconEdit"_s)->setText(icon);
    QPushButton *ok = child<QDialogButtonBox>(editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok);
    QVERIFY(ok->isEnabled());
    ok->click();
    QTRY_VERIFY(!editor || !editor->isVisible());
}

void confirmRemoval(SettingsDialog *dialog)
{
    QMessageBox *box = nullptr;
    QTRY_VERIFY((box = visibleChild<QMessageBox>(dialog)));
    QPushButton *remove = child<QPushButton>(box, u"confirmRemoveButton"_s);
    QVERIFY(remove);
    remove->click();
}

const QByteArray TwoRules = "{\n"
    "    \"version\": 1,\n"
    "    \"browsers\": [ { \"id\": \"gamma.desktop\" }, { \"id\": \"alpha.desktop\" }, { \"id\": \"beta.desktop\", \"hidden\": true } ],\n"
    "    \"rules\": [\n"
    "        { \"name\": \"Work\", \"enabled\": true, \"type\": \"subdomain\", \"pattern\": \"corp.example\", \"browser\": \"beta.desktop\" },\n"
    "        { \"enabled\": true, \"type\": \"glob\", \"pattern\": \"https://github.com/*\", \"browser\": \"alpha.desktop\" }\n"
    "    ]\n"
    "}";

} // namespace

class TestSettings : public QObject
{
    Q_OBJECT

public:
    static void initMain()
    {
        klinker::test::isolateEnvironment();
    }

private Q_SLOTS:
    void initTestCase()
    {
        klinker::test::applyTheme(false);
    }

    void hasTheExpectedPages()
    {
        SettingsFixture f;
        QStringList names;
        auto *model = qobject_cast<KPageWidgetModel *>(f.dialog->findChild<KPageWidgetModel *>());
        QVERIFY(model);
        for (int i = 0; i < model->rowCount(); ++i) {
            names.append(model->index(i, 0).data(Qt::DisplayRole).toString());
        }
        QCOMPARE(names, (QStringList{u"Browsers"_s, u"Appearance"_s, u"Routing Rules"_s, u"Privacy"_s, u"About"_s}));
        QCOMPARE(f.dialog->windowTitle(), u"Configure — Klinker"_s);
        QStringList about;
        for (const QLabel *label : f.dialog->findChildren<QLabel *>()) {
            about.append(label->text());
        }
        QVERIFY2(about.contains(u"klinker [--settings] [--choose] [--config FILE] [URL…]"_s), qPrintable(about.join(u'\n')));
        QVERIFY(about.contains(u"io.github.manicminer.Klinker.desktop"_s));
        QVERIFY(about.contains(f.configPath));
        QVERIFY2(!about.contains(u"Read from:"_s), qPrintable(about.join(u'\n')));
    }

    void browsersFollowTheConfiguredOrderAndShowProblems()
    {
        SettingsFixture f(TwoRules);
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"alpha.desktop"_s, u"beta.desktop"_s}));
        QTreeWidgetItem *beta = f.browsers()->topLevelItem(2);
        QVERIFY(beta->text(2).contains(u"Hidden"_s));
        // beta's executable does not exist: the warning is visible.
        QVERIFY(beta->text(2).contains(u"not exist"_s) || beta->text(2).contains(u"not found"_s));
        QVERIFY(!beta->icon(2).isNull());
        QVERIFY(f.browsers()->topLevelItem(1)->text(2).isEmpty() || !f.browsers()->topLevelItem(1)->text(2).contains(u"Hidden"_s));
    }

    void addManualBrowserAndApply()
    {
        SettingsFixture f;
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        click(f.dialog.get(), u"addBrowserButton"_s);
        fillEditor(f.dialog.get(), u"Work Profile"_s, Fixture, u"--profile\nwork with spaces\n{url}"_s, u"firefox"_s);
        QCOMPARE(f.browsers()->topLevelItemCount(), 4);
        QCOMPARE(f.browsers()->topLevelItem(3)->text(0), u"Work Profile"_s);

        QVERIFY(f.fileBytes().isEmpty()); // nothing written before Apply
        f.button(QDialogButtonBox::Apply)->click();
        QCOMPARE(saved.size(), 1);
        const Config config = f.savedConfig();
        const auto manual = std::ranges::find_if(config.browsers, &klinker::BrowserSetting::manual);
        QVERIFY(manual != config.browsers.cend());
        QCOMPARE(manual->name, std::optional<QString>(u"Work Profile"_s));
        QCOMPARE(manual->program, std::optional<QString>(Fixture));
        QCOMPARE(manual->arguments, std::optional<QStringList>({u"--profile"_s, u"work with spaces"_s, u"{url}"_s}));
        QCOMPARE(saved.first().at(0).value<Config>(), config);
    }

    void editorRejectsUnusableEntries_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("program");
        QTest::addColumn<QString>("arguments");
        QTest::newRow("blank name") << u"  "_s << Fixture << u"{url}"_s;
        QTest::newRow("blank program") << u"X"_s << u""_s << u"{url}"_s;
        QTest::newRow("itself") << u"X"_s << u"klinker"_s << u"{url}"_s;
        QTest::newRow("itself behind env") << u"X"_s << u"env"_s << u"FOO=1\nklinker\n{url}"_s;
        QTest::newRow("shell code") << u"X"_s << u"sh"_s << u"-c\nfirefox {url}"_s;
    }

    void editorRejectsUnusableEntries()
    {
        QFETCH(QString, name);
        QFETCH(QString, program);
        QFETCH(QString, arguments);
        const klinker::SelfIdentity self{{u"io.github.manicminer.Klinker.desktop"_s}, {u"klinker"_s}, {}};
        klinker::BrowserEditDialog editor({}, self);
        editor.show();
        child<QLineEdit>(&editor, u"nameEdit"_s)->setText(name);
        child<QLineEdit>(&editor, u"programEdit"_s)->setText(program);
        child<QPlainTextEdit>(&editor, u"argumentsEdit"_s)->setPlainText(arguments);
        QVERIFY(!editor.errors().isEmpty());
        QVERIFY(!child<QDialogButtonBox>(&editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok)->isEnabled());
        auto *problems = child<KMessageWidget>(&editor, u"problemsMessage"_s);
        QVERIFY(problems->isVisible());
        QCOMPARE(problems->messageType(), KMessageWidget::Error);
    }

    void editorWarnsAboutAMissingExecutableButAllowsIt()
    {
        const klinker::SelfIdentity self{{u"io.github.manicminer.Klinker.desktop"_s}, {u"klinker"_s}, {}};
        klinker::BrowserEditDialog editor({}, self);
        editor.show();
        child<QLineEdit>(&editor, u"nameEdit"_s)->setText(u"Later"_s);
        child<QLineEdit>(&editor, u"programEdit"_s)->setText(u"/opt/not-installed-yet/browser"_s);
        QVERIFY(editor.errors().isEmpty());
        QVERIFY(!editor.warnings().isEmpty());
        QVERIFY(child<QDialogButtonBox>(&editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok)->isEnabled());
        auto *problems = child<KMessageWidget>(&editor, u"problemsMessage"_s);
        QVERIFY(problems->isVisible());
        QCOMPARE(problems->messageType(), KMessageWidget::Warning);
    }

    void editDetectedStoresOnlyTheOverride()
    {
        SettingsFixture f;
        select(f.browsers(), 0); // Alpha
        click(f.dialog.get(), u"editBrowserButton"_s);
        auto *editor = visibleChild<klinker::BrowserEditDialog>(f.dialog.get());
        QVERIFY(editor);
        QCOMPARE(child<QLineEdit>(editor, u"nameEdit"_s)->text(), u"Alpha"_s);
        QCOMPARE(child<QLineEdit>(editor, u"programEdit"_s)->text(), Fixture);
        QCOMPARE(child<QPlainTextEdit>(editor, u"argumentsEdit"_s)->toPlainText(), u"{url}"_s);
        fillEditor(f.dialog.get(), u"Alpha Renamed"_s, Fixture, u"{url}"_s, u"firefox"_s);
        QVERIFY(f.browsers()->topLevelItem(0)->text(2).contains(u"Customised"_s));
        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        const auto alpha = std::ranges::find(config.browsers, u"alpha.desktop"_s, &klinker::BrowserSetting::id);
        QCOMPARE(alpha->name, std::optional<QString>(u"Alpha Renamed"_s));
        QCOMPARE(alpha->program, std::nullopt);
        QCOMPARE(alpha->arguments, std::nullopt);
    }

    // Empty and blank arguments cannot be written one per line; changing other
    // fields must keep them exactly.
    void editorKeepsUntouchedArgumentsExactly()
    {
        const klinker::SelfIdentity self{{u"io.github.manicminer.Klinker.desktop"_s}, {u"klinker"_s}, {}};
        const QStringList arguments{u"--profile"_s, u""_s, u"   "_s, u"{url}"_s};
        klinker::BrowserEditDialog editor({u"Alpha"_s, u"firefox"_s, {Fixture, arguments}}, self);
        editor.show();
        child<QLineEdit>(&editor, u"nameEdit"_s)->setText(u"Alpha Work"_s);
        child<QLineEdit>(&editor, u"iconEdit"_s)->setText(u"chromium"_s);
        child<QLineEdit>(&editor, u"programEdit"_s)->setText(Fixture + u"  "_s);
        QCOMPARE(editor.fields().name, u"Alpha Work"_s);
        QCOMPARE(editor.fields().icon, u"chromium"_s);
        QCOMPARE(editor.fields().command.program, Fixture);
        QCOMPARE(editor.fields().command.arguments, arguments);
        QVERIFY(child<QDialogButtonBox>(&editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok)->isEnabled());

        // Editing the arguments themselves follows the documented one-per-line
        // rule, and the editor says that the empty arguments are dropped.
        auto *argumentsEdit = child<QPlainTextEdit>(&editor, u"argumentsEdit"_s);
        argumentsEdit->setPlainText(argumentsEdit->toPlainText() + u"\n--new-window"_s);
        QCOMPARE(editor.fields().command.arguments, (QStringList{u"--profile"_s, u"{url}"_s, u"--new-window"_s}));
        auto *problems = child<KMessageWidget>(&editor, u"problemsMessage"_s);
        QVERIFY(problems->isVisible());
        QVERIFY2(problems->text().contains(u"empty"_s), qPrintable(problems->text()));
    }

    void renamingKeepsEmptyArguments_data()
    {
        QTest::addColumn<bool>("manual");
        QTest::newRow("detected") << false;
        QTest::newRow("manual") << true;
    }

    void renamingKeepsEmptyArguments()
    {
        QFETCH(bool, manual);
        SettingsFixture f(manual ? QByteArray("{\"version\":1,\"browsers\":[{\"id\":\"manual-1\",\"manual\":true,\"name\":\"Mine\","
                                              "\"program\":\"" + Fixture.toUtf8() + "\",\"arguments\":[\"--profile\",\"\",\" \",\"{url}\"]}]}")
                                 : QByteArray());
        if (!manual) {
            // Desktop entry quoting allows an empty argument.
            klinker::test::writeBrowserDesktopFile(f.dataDir, u"alpha.desktop"_s, u"Alpha"_s, Fixture + u" --profile \"\" %u"_s, u"firefox"_s);
            f.open();
        }
        const QString id = manual ? u"manual-1"_s : u"alpha.desktop"_s;
        const QStringList expected = manual ? QStringList{u"--profile"_s, u""_s, u" "_s, u"{url}"_s}
                                            : QStringList{u"--profile"_s, u""_s, u"{url}"_s};
        select(f.browsers(), int(tableIds(f.browsers()).indexOf(id)));
        click(f.dialog.get(), u"editBrowserButton"_s);
        QPointer<klinker::BrowserEditDialog> editor;
        QTRY_VERIFY((editor = visibleChild<klinker::BrowserEditDialog>(f.dialog.get())));
        child<QLineEdit>(editor, u"nameEdit"_s)->setText(u"Renamed"_s);
        child<QDialogButtonBox>(editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!editor || !editor->isVisible());

        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        const auto saved = std::ranges::find(config.browsers, id, &klinker::BrowserSetting::id);
        QVERIFY(saved != config.browsers.cend());
        QCOMPARE(saved->name, std::optional<QString>(u"Renamed"_s));
        if (manual) {
            QCOMPARE(saved->arguments, std::optional<QStringList>(expected));
        } else {
            QCOMPARE(saved->arguments, std::nullopt); // still follows the desktop file
        }
        const klinker::BrowserCatalog catalog(klinker::discoverBrowsers({f.dataDir}, f.self).browsers, config.browsers);
        QCOMPARE(catalog.browser(id)->command.arguments, expected);
    }

    void resetRestoresDetectedDefaults()
    {
        SettingsFixture f("{\"version\":1,\"browsers\":[{\"id\":\"alpha.desktop\",\"name\":\"Mine\",\"icon\":\"vivaldi\"}]}");
        QCOMPARE(f.browsers()->topLevelItem(0)->text(0), u"Mine"_s);
        select(f.browsers(), 1);
        QVERIFY(!child<QPushButton>(f.dialog.get(), u"resetBrowserButton"_s)->isEnabled());
        select(f.browsers(), 0);
        click(f.dialog.get(), u"resetBrowserButton"_s);
        QCOMPARE(f.browsers()->topLevelItem(0)->text(0), u"Alpha"_s);
        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        const auto alpha = std::ranges::find(config.browsers, u"alpha.desktop"_s, &klinker::BrowserSetting::id);
        QCOMPARE(alpha->name, std::nullopt);
        QCOMPARE(alpha->icon, std::nullopt);
    }

    void hideAndShowAgain()
    {
        SettingsFixture f;
        select(f.browsers(), 1);
        click(f.dialog.get(), u"hideBrowserButton"_s);
        QVERIFY(f.browsers()->topLevelItem(1)->text(2).contains(u"Hidden"_s));
        QCOMPARE(child<QPushButton>(f.dialog.get(), u"hideBrowserButton"_s)->text(), u"Show"_s);
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().browsers.at(1).hidden, true);

        click(f.dialog.get(), u"hideBrowserButton"_s);
        QVERIFY(!f.browsers()->topLevelItem(1)->text(2).contains(u"Hidden"_s));
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().browsers.at(1).hidden, false);
    }

    void reorderWithUpAndDown()
    {
        SettingsFixture f;
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"alpha.desktop"_s, u"beta.desktop"_s, u"gamma.desktop"_s}));
        select(f.browsers(), 2);
        click(f.dialog.get(), u"moveBrowserUpButton"_s);
        click(f.dialog.get(), u"moveBrowserUpButton"_s);
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"alpha.desktop"_s, u"beta.desktop"_s}));
        QVERIFY(!child<QPushButton>(f.dialog.get(), u"moveBrowserUpButton"_s)->isEnabled());
        click(f.dialog.get(), u"moveBrowserDownButton"_s);
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"alpha.desktop"_s, u"gamma.desktop"_s, u"beta.desktop"_s}));
        QCOMPARE(f.browsers()->currentItem()->data(0, Qt::UserRole).toString(), u"gamma.desktop"_s);
        QVERIFY(f.dialog->apply());
        QStringList saved;
        for (const auto &s : f.savedConfig().browsers) {
            saved.append(s.id);
        }
        QCOMPARE(saved, (QStringList{u"alpha.desktop"_s, u"gamma.desktop"_s, u"beta.desktop"_s}));
    }

    void removingATargetMakesTheRuleErrorVisible()
    {
        SettingsFixture f(TwoRules);
        select(f.browsers(), 1); // alpha, target of the glob rule
        click(f.dialog.get(), u"removeBrowserButton"_s);
        confirmRemoval(f.dialog.get());
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s}));

        // The rule keeps its target ID and says it is missing.
        QTreeWidgetItem *rule = f.rules()->topLevelItem(1);
        QVERIFY(rule->text(3).contains(u"alpha.desktop"_s));
        QVERIFY(rule->text(4).contains(u"not available"_s));
        f.showRule(1);
        auto *combo = child<QComboBox>(f.dialog.get(), u"ruleBrowserCombo"_s);
        QCOMPARE(combo->currentData().toString(), u"alpha.desktop"_s);
        QVERIFY(combo->currentText().contains(u"Missing"_s));

        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        QCOMPARE(config.rules.at(1).browserId, u"alpha.desktop"_s);
        const auto alpha = std::ranges::find(config.browsers, u"alpha.desktop"_s, &klinker::BrowserSetting::id);
        QVERIFY(alpha->removed);
        // Routing skips it.
        klinker::BrowserCatalog catalog(klinker::discoverBrowsers({f.dataDir}, f.self).browsers, config.browsers);
        QVERIFY(!klinker::RuleEngine(config.rules, catalog.browserIds()).route(u"https://github.com/kde"_s).matched());
    }

    void removeCanBeDeclined()
    {
        SettingsFixture f;
        select(f.browsers(), 0);
        click(f.dialog.get(), u"removeBrowserButton"_s);
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = visibleChild<QMessageBox>(f.dialog.get())));
        box->reject();
        QCOMPARE(f.browsers()->topLevelItemCount(), 3);
    }

    void cancelHasNoSideEffects()
    {
        SettingsFixture f(TwoRules);
        const QByteArray before = f.fileBytes();
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        select(f.browsers(), 0);
        click(f.dialog.get(), u"hideBrowserButton"_s);
        click(f.dialog.get(), u"addRuleButton"_s);
        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        f.button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!f.dialog->isVisible());
        QCOMPARE(saved.size(), 0);
        QCOMPARE(f.fileBytes(), before);
        QVERIFY(!QFile::exists(f.configPath + u".lock"_s));
    }

    void cancelWithoutAFileCreatesNothing()
    {
        SettingsFixture f;
        click(f.dialog.get(), u"addRuleButton"_s);
        f.dialog->reject();
        QVERIFY(!QFile::exists(f.configPath));
        QVERIFY(!QDir(QFileInfo(f.configPath).absolutePath()).exists());
    }

    void okSavesAndCloses()
    {
        SettingsFixture f;
        child<QCheckBox>(f.dialog.get(), u"unwrapRedirectsCheck"_s)->setChecked(true);
        f.button(QDialogButtonBox::Ok)->click();
        QVERIFY(!f.dialog->isVisible());
        QCOMPARE(f.dialog->result(), int(QDialog::Accepted));
        QCOMPARE(f.savedConfig().privacy.unwrapRedirects, true);
    }

    void externalChangesAreNotOverwritten()
    {
        SettingsFixture f(TwoRules);
        const QByteArray external = "{\"version\":1,\"privacy\":{\"stripTrackingParameters\":true}}";
        f.writeConfig(external);
        child<QCheckBox>(f.dialog.get(), u"unwrapRedirectsCheck"_s)->setChecked(true);
        f.button(QDialogButtonBox::Ok)->click();
        QVERIFY(f.dialog->isVisible());
        QVERIFY(f.message()->isVisible());
        QCOMPARE(f.message()->messageType(), KMessageWidget::Error);
        QVERIFY(f.message()->text().contains(u"changed"_s));
        QCOMPARE(f.fileBytes(), external);
    }

    void unusableConfigDisablesSaving()
    {
        SettingsFixture f("{ not json");
        QVERIFY(f.message()->isVisible());
        QCOMPARE(f.message()->messageType(), KMessageWidget::Error);
        QVERIFY(!f.button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!f.button(QDialogButtonBox::Apply)->isEnabled());
        QVERIFY(!f.dialog->apply());
        QCOMPARE(f.fileBytes(), QByteArray("{ not json"));

        // Changes do not make it savable.
        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        click(f.dialog.get(), u"addRuleButton"_s);
        QVERIFY(!f.button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!f.canApply());
        QCOMPARE(f.fileBytes(), QByteArray("{ not json"));
    }

    void rulesCanBeCreatedEditedOrderedAndRemoved()
    {
        SettingsFixture f;
        click(f.dialog.get(), u"addRuleButton"_s);
        click(f.dialog.get(), u"addRuleButton"_s);
        QCOMPARE(f.rules()->topLevelItemCount(), 2);

        f.showRule(0);
        child<QLineEdit>(f.dialog.get(), u"ruleNameEdit"_s)->setText(u"Docs"_s);
        auto *type = child<QComboBox>(f.dialog.get(), u"ruleTypeCombo"_s);
        type->setCurrentIndex(type->findData(u"domain"_s));
        child<QLineEdit>(f.dialog.get(), u"rulePatternEdit"_s)->setText(u"Docs.Example.org"_s);
        auto *target = child<QComboBox>(f.dialog.get(), u"ruleBrowserCombo"_s);
        target->setCurrentIndex(target->findData(u"gamma.desktop"_s));

        f.showRule(1);
        type->setCurrentIndex(type->findData(u"regex"_s));
        child<QLineEdit>(f.dialog.get(), u"rulePatternEdit"_s)->setText(u"^https://(www\\.)?youtube\\.com/"_s);
        target->setCurrentIndex(target->findData(u"beta.desktop"_s));
        child<QCheckBox>(f.dialog.get(), u"ruleEnabledCheck"_s)->setChecked(false);
        QCOMPARE(f.rules()->topLevelItem(1)->checkState(0), Qt::Unchecked);

        click(f.dialog.get(), u"moveRuleUpButton"_s);
        QCOMPARE(f.rules()->topLevelItem(0)->text(2), u"^https://(www\\.)?youtube\\.com/"_s);

        // The table's check box toggles too.
        f.rules()->topLevelItem(0)->setCheckState(0, Qt::Checked);
        QVERIFY(child<QCheckBox>(f.dialog.get(), u"ruleEnabledCheck"_s)->isChecked());

        QVERIFY(f.dialog->apply());
        QList<Rule> expected{
            {QString(), true, RuleType::Regex, u"^https://(www\\.)?youtube\\.com/"_s, u"beta.desktop"_s},
            {u"Docs"_s, true, RuleType::Domain, u"Docs.Example.org"_s, u"gamma.desktop"_s},
        };
        QCOMPARE(f.savedConfig().rules, expected);

        f.showRule(0);
        click(f.dialog.get(), u"removeRuleButton"_s);
        QCOMPARE(f.rules()->topLevelItemCount(), 1);
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().rules, QList<Rule>{expected.at(1)});
    }

    void invalidPatternsCannotBeSavedAsEffectiveRules()
    {
        SettingsFixture f;
        click(f.dialog.get(), u"addRuleButton"_s);
        auto *type = child<QComboBox>(f.dialog.get(), u"ruleTypeCombo"_s);
        type->setCurrentIndex(type->findData(u"regex"_s));
        child<QLineEdit>(f.dialog.get(), u"rulePatternEdit"_s)->setText(u"(unclosed"_s);
        auto *problem = child<KMessageWidget>(f.dialog.get(), u"ruleProblemMessage"_s);
        QVERIFY(problem->isVisible());
        QVERIFY(!problem->text().isEmpty());
        QVERIFY(!f.rules()->topLevelItem(0)->text(4).isEmpty());

        f.button(QDialogButtonBox::Ok)->click();
        QVERIFY(f.dialog->isVisible());
        QVERIFY(f.message()->isVisible());
        QVERIFY(f.message()->text().contains(u"Rule 1"_s));
        QVERIFY(!QFile::exists(f.configPath));
        QVERIFY(f.canApply()); // still not saved

        // Disabled, the rule may be kept for later.
        child<QCheckBox>(f.dialog.get(), u"ruleEnabledCheck"_s)->setChecked(false);
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().rules.at(0).enabled, false);
    }

    void ruleTargetsAreIdsIncludingHiddenBrowsers()
    {
        SettingsFixture f(TwoRules);
        f.showRule(0);
        auto *combo = child<QComboBox>(f.dialog.get(), u"ruleBrowserCombo"_s);
        QCOMPARE(combo->currentData().toString(), u"beta.desktop"_s);
        QVERIFY(combo->currentText().contains(u"hidden"_s));
        QCOMPARE(combo->count(), 3);

        // Renaming and reordering browsers does not retarget the rule.
        select(f.browsers(), 2);
        click(f.dialog.get(), u"editBrowserButton"_s);
        fillEditor(f.dialog.get(), u"Beta Work"_s, u"/nonexistent/beta-browser"_s, u"{url}"_s, u"chromium"_s);
        click(f.dialog.get(), u"moveBrowserUpButton"_s);
        f.showRule(0);
        QCOMPARE(combo->currentData().toString(), u"beta.desktop"_s);
        QVERIFY(combo->currentText().contains(u"Beta Work"_s));
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().rules.at(0).browserId, u"beta.desktop"_s);
    }

    void testUrlShowsTheWinningRule()
    {
        SettingsFixture f(TwoRules);
        auto *url = child<QLineEdit>(f.dialog.get(), u"testUrlEdit"_s);
        auto *result = child<QLabel>(f.dialog.get(), u"testResultLabel"_s);
        url->setText(u"https://git.corp.example/x"_s);
        QVERIFY2(result->text().contains(u"Rule 1"_s), qPrintable(result->text()));
        QVERIFY(result->text().contains(u"Beta"_s));
        url->setText(u"https://github.com/kde/<b>"_s);
        QVERIFY(result->text().contains(u"Rule 2"_s));
        QCOMPARE(result->textFormat(), Qt::PlainText);
        url->setText(u"https://kde.org/"_s);
        QVERIFY(result->text().contains(u"chooser"_s));
    }

    void privacyTogglesAreOffByDefaultAndIndependent()
    {
        SettingsFixture f;
        auto *strip = child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s);
        auto *unwrap = child<QCheckBox>(f.dialog.get(), u"unwrapRedirectsCheck"_s);
        QVERIFY(!strip->isChecked());
        QVERIFY(!unwrap->isChecked());
        auto *notice = child<QLabel>(f.dialog.get(), u"privacyNotice"_s);
        QVERIFY(notice->text().contains(u"three"_s));
        QVERIFY(notice->text().contains(u"before you choose"_s));

        strip->setChecked(true);
        QVERIFY(!unwrap->isChecked());
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().privacy, (klinker::PrivacySettings{true, false}));
        strip->setChecked(false);
        unwrap->setChecked(true);
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().privacy, (klinker::PrivacySettings{false, true}));
    }

    void onlyTheConfigurationFileIsUsed()
    {
        // Another application's configuration next to it is neither read nor written.
        SettingsFixture f;
        const QString otherPath = f.dir.path() + u"/other-app/config.json"_s;
        QDir().mkpath(QFileInfo(otherPath).absolutePath());
        QFile other(otherPath);
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write(TwoRules);
        other.close();
        f.open();
        QCOMPARE(f.store->path(), f.configPath);
        QCOMPARE(f.rules()->topLevelItemCount(), 0);
        QVERIFY(!QFile::exists(f.configPath));

        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(f.dialog->apply());
        QVERIFY(f.savedConfig().rules.isEmpty());
        QVERIFY(f.savedConfig().privacy.stripTrackingParameters);
        QVERIFY(other.open(QIODevice::ReadOnly));
        QCOMPARE(other.readAll(), TwoRules);
        QCOMPARE(QDir(QFileInfo(otherPath).absolutePath()).entryList(QDir::Files | QDir::Hidden), QStringList{u"config.json"_s});
    }

    // Appearance page

    void appearanceDefaultsKeepTheVerticalList()
    {
        SettingsFixture f;
        f.dialog->showPage(u"appearance"_s);
        auto *style = child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s);
        auto *size = child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s);
        auto *names = child<QCheckBox>(f.dialog.get(), u"showNamesCheck"_s);
        auto *focus = child<QCheckBox>(f.dialog.get(), u"closeOnFocusLossCheck"_s);
        QVERIFY(style && size && names && focus);
        QVERIFY(style->isVisible());
        QCOMPARE(style->count(), 2);
        QCOMPARE(style->currentData().toString(), u"vertical"_s);
        QCOMPARE(style->itemData(1).toString(), u"horizontal"_s);
        QCOMPARE(size->value(), 48);
        QCOMPARE(size->minimum(), 24);
        QCOMPARE(size->maximum(), 128);
        QVERIFY(names->isChecked());
        QVERIFY(!focus->isChecked());
        // Only the horizontal chooser uses the icon size and names.
        QVERIFY(!size->isEnabled());
        QVERIFY(!names->isEnabled());
        QVERIFY(focus->isEnabled());
        QCOMPARE(f.dialog->draftConfig().chooser, klinker::ChooserSettings());
        // Each control can be found by its label.
        for (QWidget *control : std::initializer_list<QWidget *>{style, size}) {
            const auto labels = f.dialog->findChildren<QLabel *>();
            QVERIFY(std::ranges::any_of(labels, [control](QLabel *label) {
                return label->buddy() == control && !label->text().isEmpty();
            }));
        }
    }

    void horizontalOptionsAreRememberedWhileVertical()
    {
        SettingsFixture f;
        auto *style = child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s);
        auto *size = child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s);
        auto *names = child<QCheckBox>(f.dialog.get(), u"showNamesCheck"_s);
        style->setCurrentIndex(style->findData(u"horizontal"_s));
        QVERIFY(size->isEnabled());
        QVERIFY(names->isEnabled());
        size->setValue(96);
        names->setChecked(false);
        QCOMPARE(f.dialog->draftConfig().chooser, (klinker::ChooserSettings{klinker::ChooserStyle::Horizontal, 96, false, false}));

        style->setCurrentIndex(style->findData(u"vertical"_s));
        QVERIFY(!size->isEnabled());
        QVERIFY(!names->isEnabled());
        QCOMPARE(size->value(), 96);
        QVERIFY(!names->isChecked());
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().chooser, (klinker::ChooserSettings{klinker::ChooserStyle::Vertical, 96, false, false}));

        // Out-of-range input is bounded by the control.
        size->setValue(500);
        QCOMPARE(size->value(), 128);
        size->setValue(1);
        QCOMPARE(size->value(), 24);
    }

    void appearanceIsLoadedAndSavedWithOk()
    {
        SettingsFixture f("{\"version\":1,\"chooser\":{\"style\":\"horizontal\",\"iconSize\":64,\"showBrowserNames\":false,"
                          "\"closeOnFocusLoss\":true}}");
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        auto *style = child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s);
        auto *size = child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s);
        auto *names = child<QCheckBox>(f.dialog.get(), u"showNamesCheck"_s);
        auto *focus = child<QCheckBox>(f.dialog.get(), u"closeOnFocusLossCheck"_s);
        QCOMPARE(style->currentData().toString(), u"horizontal"_s);
        QCOMPARE(size->value(), 64);
        QVERIFY(size->isEnabled());
        QVERIFY(!names->isChecked());
        QVERIFY(focus->isChecked());

        focus->setChecked(false);
        size->setValue(32);
        f.button(QDialogButtonBox::Ok)->click();
        QVERIFY(!f.dialog->isVisible());
        QCOMPARE(saved.size(), 1);
        const klinker::ChooserSettings expected{klinker::ChooserStyle::Horizontal, 32, false, false};
        QCOMPARE(saved.first().at(0).value<Config>().chooser, expected);
        QCOMPARE(f.savedConfig().chooser, expected);
    }

    void appearanceCancelChangesNothing()
    {
        SettingsFixture f(TwoRules);
        const QByteArray before = f.fileBytes();
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        auto *style = child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s);
        style->setCurrentIndex(style->findData(u"horizontal"_s));
        child<QCheckBox>(f.dialog.get(), u"closeOnFocusLossCheck"_s)->setChecked(true);
        f.button(QDialogButtonBox::Cancel)->click();
        QVERIFY(!f.dialog->isVisible());
        QCOMPARE(saved.size(), 0);
        QCOMPARE(f.fileBytes(), before);
    }

    void reloadShowsTheAppearanceFromDisk()
    {
        SettingsFixture f(TwoRules);
        f.writeConfig("{\"version\":1,\"chooser\":{\"style\":\"horizontal\",\"iconSize\":112}}");
        child<QCheckBox>(f.dialog.get(), u"closeOnFocusLossCheck"_s)->setChecked(true);
        QVERIFY(!f.dialog->apply());
        for (QAction *action : f.message()->actions()) {
            if (action->text().contains(u"Reload"_s)) {
                action->trigger();
            }
        }
        QCOMPARE(child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s)->currentData().toString(), u"horizontal"_s);
        QCOMPARE(child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s)->value(), 112);
        QVERIFY(child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s)->isEnabled());
        QVERIFY(!child<QCheckBox>(f.dialog.get(), u"closeOnFocusLossCheck"_s)->isChecked());
        QVERIFY(f.dialog->apply());
        QCOMPARE(f.savedConfig().chooser, (klinker::ChooserSettings{klinker::ChooserStyle::Horizontal, 112, true, false}));
    }

    void rescanMergesWithoutLosingSettings()
    {
        SettingsFixture f("{\"version\":1,\"browsers\":[{\"id\":\"gamma.desktop\"},{\"id\":\"beta.desktop\",\"name\":\"Beta Mine\"},{\"id\":\"alpha.desktop\",\"removed\":true}]}");
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s}));
        klinker::test::writeBrowserDesktopFile(f.dataDir, u"delta.desktop"_s, u"Delta"_s, Fixture + u" %u"_s);
        click(f.dialog.get(), u"rescanButton"_s);
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s, u"delta.desktop"_s}));
        QCOMPARE(f.browsers()->topLevelItem(1)->text(0), u"Beta Mine"_s);
        QVERIFY(f.message()->isVisible());
        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        const auto alpha = std::ranges::find(config.browsers, u"alpha.desktop"_s, &klinker::BrowserSetting::id);
        QVERIFY(alpha != config.browsers.cend() && alpha->removed);
    }

    // Saving records the order the dialog shows, so newly found browsers are
    // merged into the configuration even when only other settings changed.
    void savingRecordsDiscoveredBrowsers()
    {
        SettingsFixture f;
        const QStringList shown = tableIds(f.browsers());
        QCOMPARE(shown.size(), 3);
        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(f.dialog->apply());
        const Config config = f.savedConfig();
        QStringList saved;
        for (const klinker::BrowserSetting &setting : config.browsers) {
            saved.append(setting.id);
            QCOMPARE(setting, klinker::BrowserSetting{.id = setting.id}); // nothing but the position
        }
        QCOMPARE(saved, shown);
        QVERIFY(config.privacy.stripTrackingParameters);
    }

    void rescanThenApplyRecordsNewBrowsers()
    {
        SettingsFixture f("{\"version\":1,\"browsers\":["
                          "{\"id\":\"gone.desktop\",\"hidden\":true,\"name\":\"Uninstalled\"},"
                          "{\"id\":\"gamma.desktop\",\"hidden\":true},"
                          "{\"id\":\"beta.desktop\",\"name\":\"Beta Mine\"},"
                          "{\"id\":\"alpha.desktop\",\"removed\":true}]}");
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s}));
        klinker::test::writeBrowserDesktopFile(f.dataDir, u"delta.desktop"_s, u"Delta"_s, Fixture + u" %u"_s);
        click(f.dialog.get(), u"rescanButton"_s);
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s, u"delta.desktop"_s}));
        QVERIFY(f.dialog->apply());

        const Config config = f.savedConfig();
        QStringList saved;
        for (const klinker::BrowserSetting &setting : config.browsers) {
            saved.append(setting.id);
        }
        // Settings of a browser that is not installed keep their place.
        QCOMPARE(saved, (QStringList{u"gone.desktop"_s, u"gamma.desktop"_s, u"beta.desktop"_s, u"alpha.desktop"_s, u"delta.desktop"_s}));
        QCOMPARE(config.browsers[0].name, std::optional<QString>(u"Uninstalled"_s));
        QVERIFY(config.browsers[0].hidden);
        QVERIFY(config.browsers[1].hidden);
        QCOMPARE(config.browsers[2].name, std::optional<QString>(u"Beta Mine"_s));
        QVERIFY(config.browsers[3].removed);
        QCOMPARE(config.browsers[4], klinker::BrowserSetting{.id = u"delta.desktop"_s});

        // The recorded position holds when a browser that sorts first by name
        // is installed later.
        klinker::test::writeBrowserDesktopFile(f.dataDir, u"aardvark.desktop"_s, u"Aardvark"_s, Fixture + u" %u"_s);
        f.open();
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"beta.desktop"_s, u"delta.desktop"_s, u"aardvark.desktop"_s}));
    }

    void applyKeepsTheSavedOrderForLaterRescans_data()
    {
        QTest::addColumn<bool>("uninstallBeta");
        QTest::newRow("new browser found") << false;
        QTest::newRow("saved browser uninstalled") << true;
    }

    // Without reopening the dialog, a rescan after Apply starts from what was
    // just saved rather than from the configuration the dialog was opened with.
    void applyKeepsTheSavedOrderForLaterRescans()
    {
        QFETCH(bool, uninstallBeta);
        SettingsFixture f;
        QVERIFY(QFile::remove(f.dataDir + u"/applications/gamma.desktop"_s));
        f.open();
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"alpha.desktop"_s, u"beta.desktop"_s}));
        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(f.dialog->apply());

        klinker::test::writeBrowserDesktopFile(f.dataDir, u"aardvark.desktop"_s, u"Aardvark"_s, Fixture + u" %u"_s);
        if (uninstallBeta) {
            QVERIFY(QFile::remove(f.dataDir + u"/applications/beta.desktop"_s));
        }
        click(f.dialog.get(), u"rescanButton"_s);
        QCOMPARE(tableIds(f.browsers()), uninstallBeta ? (QStringList{u"alpha.desktop"_s, u"aardvark.desktop"_s})
                                                       : (QStringList{u"alpha.desktop"_s, u"beta.desktop"_s, u"aardvark.desktop"_s}));
        QVERIFY(f.dialog->apply());

        const Config config = f.savedConfig();
        QStringList saved;
        for (const klinker::BrowserSetting &setting : config.browsers) {
            saved.append(setting.id);
            QCOMPARE(setting, klinker::BrowserSetting{.id = setting.id});
        }
        // An uninstalled browser keeps its saved place.
        QCOMPARE(saved, (QStringList{u"alpha.desktop"_s, u"beta.desktop"_s, u"aardvark.desktop"_s}));
        QVERIFY(config.privacy.stripTrackingParameters);
    }

    // Apply
    //
    // Apply is enabled only while the settings differ from what was last
    // loaded or saved; OK always saves and closes.

    void applyStartsDisabled_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("no file, browsers found") << QByteArray();
        QTest::newRow("saved settings") << TwoRules;
    }

    void applyStartsDisabled()
    {
        QFETCH(QByteArray, json);
        SettingsFixture f(json);
        QCOMPARE(f.browsers()->topLevelItemCount(), 3);
        QCOMPARE(f.dialog->draftConfig().browsers.size(), 3); // found browsers keep their place when saved
        QVERIFY(!f.canApply());
        QVERIFY(f.button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!f.message()->isVisible());
    }

    void appearanceAndPrivacyChangesEnableApplyUntilUndone()
    {
        SettingsFixture f;
        auto *style = child<QComboBox>(f.dialog.get(), u"chooserStyleCombo"_s);
        style->setCurrentIndex(style->findData(u"horizontal"_s));
        QVERIFY(f.canApply());
        style->setCurrentIndex(style->findData(u"vertical"_s));
        QVERIFY(!f.canApply());

        auto *size = child<QSpinBox>(f.dialog.get(), u"iconSizeSpin"_s);
        size->setValue(64);
        QVERIFY(f.canApply());
        size->setValue(48);
        QVERIFY(!f.canApply());

        for (const QString &name : {u"showNamesCheck"_s, u"closeOnFocusLossCheck"_s, u"stripTrackingCheck"_s, u"unwrapRedirectsCheck"_s}) {
            auto *check = child<QCheckBox>(f.dialog.get(), name);
            check->toggle();
            QVERIFY2(f.canApply(), qPrintable(name));
            check->toggle();
            QVERIFY2(!f.canApply(), qPrintable(name));
        }
    }

    void browserChangesEnableApplyUntilUndone()
    {
        SettingsFixture f;
        select(f.browsers(), 1);
        click(f.dialog.get(), u"hideBrowserButton"_s);
        QVERIFY(f.canApply());
        click(f.dialog.get(), u"hideBrowserButton"_s); // Show
        QVERIFY(!f.canApply());

        select(f.browsers(), 2);
        click(f.dialog.get(), u"moveBrowserUpButton"_s);
        QVERIFY(f.canApply());
        click(f.dialog.get(), u"moveBrowserDownButton"_s);
        QVERIFY(!f.canApply());

        select(f.browsers(), 0);
        click(f.dialog.get(), u"editBrowserButton"_s);
        fillEditor(f.dialog.get(), u"Alpha Renamed"_s, Fixture, u"{url}"_s, u"firefox"_s);
        QVERIFY(f.canApply());
        click(f.dialog.get(), u"editBrowserButton"_s);
        fillEditor(f.dialog.get(), u"Alpha"_s, Fixture, u"{url}"_s, u"firefox"_s);
        QVERIFY(!f.canApply());

        click(f.dialog.get(), u"removeBrowserButton"_s);
        confirmRemoval(f.dialog.get());
        QVERIFY(f.canApply());

        f.open();
        click(f.dialog.get(), u"addBrowserButton"_s);
        fillEditor(f.dialog.get(), u"Work Profile"_s, Fixture, u"{url}"_s);
        QVERIFY(f.canApply());

        f.writeConfig("{\"version\":1,\"browsers\":[{\"id\":\"alpha.desktop\",\"name\":\"Mine\"}]}");
        f.open();
        QVERIFY(!f.canApply());
        select(f.browsers(), 0);
        click(f.dialog.get(), u"resetBrowserButton"_s);
        QVERIFY(f.canApply());
    }

    void ruleChangesEnableApplyUntilUndone()
    {
        SettingsFixture f(TwoRules);
        f.showRule(0);
        auto *name = child<QLineEdit>(f.dialog.get(), u"ruleNameEdit"_s);
        name->setText(u"Work!"_s);
        QVERIFY(f.canApply());
        name->setText(u"Work"_s);
        QVERIFY(!f.canApply());

        auto *enabled = child<QCheckBox>(f.dialog.get(), u"ruleEnabledCheck"_s);
        enabled->setChecked(false);
        QVERIFY(f.canApply());
        enabled->setChecked(true);
        QVERIFY(!f.canApply());

        f.rules()->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
        QVERIFY(f.canApply());
        f.rules()->topLevelItem(1)->setCheckState(0, Qt::Checked);
        QVERIFY(!f.canApply());

        auto *type = child<QComboBox>(f.dialog.get(), u"ruleTypeCombo"_s);
        type->setCurrentIndex(type->findData(u"domain"_s));
        QVERIFY(f.canApply());
        type->setCurrentIndex(type->findData(u"subdomain"_s));
        QVERIFY(!f.canApply());

        auto *pattern = child<QLineEdit>(f.dialog.get(), u"rulePatternEdit"_s);
        pattern->setText(u"corp.example.org"_s);
        QVERIFY(f.canApply());
        pattern->setText(u"corp.example"_s);
        QVERIFY(!f.canApply());

        auto *target = child<QComboBox>(f.dialog.get(), u"ruleBrowserCombo"_s);
        target->setCurrentIndex(target->findData(u"gamma.desktop"_s));
        QVERIFY(f.canApply());
        target->setCurrentIndex(target->findData(u"beta.desktop"_s));
        QVERIFY(!f.canApply());

        f.showRule(1);
        click(f.dialog.get(), u"moveRuleUpButton"_s);
        QVERIFY(f.canApply());
        click(f.dialog.get(), u"moveRuleDownButton"_s);
        QVERIFY(!f.canApply());

        click(f.dialog.get(), u"addRuleButton"_s);
        QVERIFY(f.canApply());
        click(f.dialog.get(), u"removeRuleButton"_s); // the new rule
        QVERIFY(!f.canApply());
        f.showRule(0);
        click(f.dialog.get(), u"removeRuleButton"_s);
        QVERIFY(f.canApply());
    }

    void lookingAroundKeepsApplyDisabled()
    {
        SettingsFixture f(TwoRules);
        for (const QString &page : {u"appearance"_s, u"rules"_s, u"privacy"_s, u"about"_s, u"browsers"_s}) {
            f.dialog->showPage(page);
        }
        select(f.browsers(), 2);
        select(f.browsers(), 0);
        f.showRule(1);
        child<QLineEdit>(f.dialog.get(), u"testUrlEdit"_s)->setText(u"https://github.com/kde"_s);

        // Editors that are cancelled or confirmed unchanged.
        select(f.browsers(), 1);
        click(f.dialog.get(), u"editBrowserButton"_s);
        QPointer<klinker::BrowserEditDialog> editor;
        QTRY_VERIFY((editor = visibleChild<klinker::BrowserEditDialog>(f.dialog.get())));
        editor->reject();
        click(f.dialog.get(), u"editBrowserButton"_s);
        QTRY_VERIFY((editor = visibleChild<klinker::BrowserEditDialog>(f.dialog.get())));
        child<QDialogButtonBox>(editor, u"buttonBox"_s)->button(QDialogButtonBox::Ok)->click();
        click(f.dialog.get(), u"removeBrowserButton"_s);
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = visibleChild<QMessageBox>(f.dialog.get())));
        box->reject();

        click(f.dialog.get(), u"rescanButton"_s); // nothing new installed
        QCOMPARE(tableIds(f.browsers()), (QStringList{u"gamma.desktop"_s, u"alpha.desktop"_s, u"beta.desktop"_s}));
        QVERIFY(!f.canApply());
    }

    void rescanFindingABrowserEnablesApply()
    {
        SettingsFixture f;
        klinker::test::writeBrowserDesktopFile(f.dataDir, u"delta.desktop"_s, u"Delta"_s, Fixture + u" %u"_s);
        click(f.dialog.get(), u"rescanButton"_s);
        QVERIFY(f.canApply());
    }

    // Saving needs no confirmation: the message goes away, with its space.
    void applySavesQuietlyAndDisablesItself()
    {
        SettingsFixture f(TwoRules);
        QWidget *footer = f.button(QDialogButtonBox::Apply)->parentWidget()->parentWidget();
        const int footerHeight = footer->sizeHint().height();
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        click(f.dialog.get(), u"rescanButton"_s);
        QVERIFY(f.message()->isVisible());
        auto *strip = child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s);
        strip->setChecked(true);
        QVERIFY(f.canApply());
        f.button(QDialogButtonBox::Apply)->click();

        QCOMPARE(saved.size(), 1);
        QVERIFY(f.savedConfig().privacy.stripTrackingParameters);
        QVERIFY(f.dialog->isVisible());
        QVERIFY(!f.canApply());
        QVERIFY(f.button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(!f.message()->isVisibleTo(f.dialog.get()));
        QCOMPARE(footer->sizeHint().height(), footerHeight);

        strip->setChecked(false);
        QVERIFY(f.canApply());
        strip->setChecked(true); // as saved
        QVERIFY(!f.canApply());
        QCOMPARE(saved.size(), 1);
    }

    void failedApplyKeepsTheChanges()
    {
        SettingsFixture f(TwoRules);
        const QByteArray external = "{\"version\":1,\"privacy\":{\"stripTrackingParameters\":true}}";
        f.writeConfig(external);
        auto *unwrap = child<QCheckBox>(f.dialog.get(), u"unwrapRedirectsCheck"_s);
        unwrap->setChecked(true);
        f.button(QDialogButtonBox::Apply)->click();
        QCOMPARE(f.fileBytes(), external);
        QVERIFY(f.canApply());
        QVERIFY(f.message()->isVisible());
        QCOMPARE(f.message()->messageType(), KMessageWidget::Error);
        QAction *reload = nullptr;
        for (QAction *action : f.message()->actions()) {
            if (action->text().contains(u"Reload"_s)) {
                reload = action;
            }
        }
        QVERIFY(reload && reload->isVisible());

        reload->trigger();
        QVERIFY(!f.canApply());
        QVERIFY(child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->isChecked());
        unwrap->setChecked(true);
        QVERIFY(f.canApply());
        f.button(QDialogButtonBox::Apply)->click();
        QVERIFY(!f.canApply());
        QCOMPARE(f.savedConfig().privacy, (klinker::PrivacySettings{true, true}));
    }

    // OK saves what is shown even without changes, e.g. after Reload from Disk
    // or to write the defaults.
    void okSavesWithoutChanges()
    {
        SettingsFixture f;
        QSignalSpy saved(f.dialog.get(), &SettingsDialog::saved);
        QVERIFY(!f.canApply());
        f.button(QDialogButtonBox::Ok)->click();
        QVERIFY(!f.dialog->isVisible());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(f.savedConfig().browsers.size(), 3);
    }

    void afterApplyScreenshot_data()
    {
        QTest::addColumn<QString>("style");
        for (const QString &style : {u"Oxygen"_s, u"Breeze"_s}) {
            QTest::newRow(qPrintable(style)) << style;
        }
    }

    // As reported, with the Breeze Dark colours.
    void afterApplyScreenshot()
    {
        QFETCH(QString, style);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style, true);
        SettingsFixture f(TwoRules);
        f.dialog->resize(900, 680);
        child<QCheckBox>(f.dialog.get(), u"stripTrackingCheck"_s)->setChecked(true);
        f.button(QDialogButtonBox::Apply)->click();
        QVERIFY(f.savedConfig().privacy.stripTrackingParameters);
        QVERIFY(!f.canApply());
        QVERIFY(!f.message()->isVisibleTo(f.dialog.get()));
        // The style draws the disabled button dimmed.
        QStyleOptionButton option;
        option.initFrom(f.button(QDialogButtonBox::Apply));
        QCOMPARE(option.palette.currentColorGroup(), QPalette::Disabled);
        QVERIFY(f.button(QDialogButtonBox::Apply)->styleSheet().isEmpty());
        if (!qEnvironmentVariableIsEmpty("KLINKER_SCREENSHOT_DIR")) {
            QTest::qWait(600); // Oxygen fades the button out
        }
        klinker::test::saveScreenshot(f.dialog.get(), u"settings-after-apply-%1-dark"_s.arg(style.toLower()));
    }

    void screenshots_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void screenshots()
    {
        QFETCH(bool, dark);
        klinker::test::applyTheme(dark);
        const QString suffix = dark ? u"dark"_s : u"light"_s;
        SettingsFixture f(TwoRules);
        f.dialog->resize(900, 680);
        for (const QString &page : {u"browsers"_s, u"appearance"_s, u"rules"_s, u"privacy"_s, u"about"_s}) {
            f.dialog->showPage(page);
            if (page == u"rules"_s) {
                f.showRule(1);
                child<QLineEdit>(f.dialog.get(), u"testUrlEdit"_s)->setText(u"https://github.com/KDE/plasma-desktop"_s);
            }
            const QImage image = f.dialog->grab().toImage();
            const int lightness = klinker::test::meanLightness(image);
            QVERIFY2(dark ? lightness < 110 : lightness > 150, qPrintable(u"%1: mean lightness %2"_s.arg(page).arg(lightness)));
            klinker::test::saveScreenshot(f.dialog.get(), u"settings-%1-%2"_s.arg(page, suffix));
        }
        select(f.browsers(), 0);
        click(f.dialog.get(), u"editBrowserButton"_s);
        auto *editor = visibleChild<klinker::BrowserEditDialog>(f.dialog.get());
        QVERIFY(editor);
        klinker::test::saveScreenshot(editor, u"settings-browser-editor-"_s + suffix);
        editor->reject();
        klinker::test::applyTheme(false);
    }
};

QTEST_MAIN(TestSettings)

#include "test_settings.moc"
