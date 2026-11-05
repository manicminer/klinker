#include "core/config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace Qt::StringLiterals;
using namespace klinker;

Q_DECLARE_METATYPE(klinker::Config)

namespace
{
Config sampleConfig()
{
    Config config;

    BrowserSetting firefox;
    firefox.id = u"firefox.desktop"_s;
    firefox.name = u"Firefox (Work)"_s;
    firefox.arguments = QStringList{u"-P"_s, u"work"_s, u"{url}"_s};
    BrowserSetting chromium;
    chromium.id = u"chromium.desktop"_s;
    chromium.hidden = true;
    BrowserSetting brave;
    brave.id = u"brave-browser.desktop"_s;
    brave.removed = true;
    BrowserSetting manual;
    manual.id = u"manual-1a2b3c4d"_s;
    manual.manual = true;
    manual.name = u"Ünicode “Browser”"_s;
    manual.icon = u"web-browser"_s;
    manual.program = u"/opt/browser/run"_s;
    manual.arguments = QStringList{u""_s, u"--x=$HOME `id`"_s};
    config.browsers = {firefox, chromium, brave, manual};

    config.rules = {
        {u"Work"_s, true, RuleType::Subdomain, u"example.com"_s, u"firefox.desktop"_s},
        {QString(), false, RuleType::Glob, u"https://github.com/*"_s, u"manual-1a2b3c4d"_s},
        {u"Broken regex is still stored"_s, true, RuleType::Regex, u"(unclosed"_s, u"chromium.desktop"_s},
        {u"Exact"_s, true, RuleType::Domain, u"bücher.de"_s, QString()},
    };
    config.privacy.stripTrackingParameters = true;
    config.privacy.unwrapRedirects = false;
    return config;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A temporary $XDG_CONFIG_HOME with the configuration file and, next to it,
// the file of an unrelated application.
struct Locations {
    QTemporaryDir dir;
    QString current = dir.filePath(u"klinker/config.json"_s);
    QString sibling = dir.filePath(u"other-app/config.json"_s);

    ConfigStore store() const
    {
        return ConfigStore(current);
    }

    static bool write(const QString &path, const QByteArray &content)
    {
        return QDir().mkpath(QFileInfo(path).absolutePath()) && writeFile(path, content);
    }
};

Config otherConfig()
{
    Config config;
    config.privacy.unwrapRedirects = true;
    config.chooser.style = ChooserStyle::Horizontal;
    return config;
}
} // namespace

class TestConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaults()
    {
        const Config config;
        QVERIFY(config.browsers.isEmpty());
        QVERIFY(config.rules.isEmpty());
        QVERIFY(!config.privacy.stripTrackingParameters);
        QVERIFY(!config.privacy.unwrapRedirects);

        const auto minimal = configFromJson(R"({"version": 1})");
        QVERIFY2(minimal.config, qPrintable(minimal.error));
        QCOMPARE(*minimal.config, Config());
    }

    void roundTrip()
    {
        const Config config = sampleConfig();
        const QByteArray json = configToJson(config);
        const auto parsed = configFromJson(json);
        QVERIFY2(parsed.config, qPrintable(parsed.error));
        QVERIFY(parsed.warnings.isEmpty());
        QCOMPARE(*parsed.config, config);
    }

    void jsonIsReadableAndMinimal()
    {
        const QByteArray json = configToJson(sampleConfig());
        QVERIFY(json.contains("\"version\": 1"));
        QVERIFY(json.contains("\n    \"rules\": ["));
        QVERIFY(json.contains("\"type\": \"subdomain\""));
        QVERIFY(json.contains("\"stripTrackingParameters\": true"));
        // Defaults are omitted for browser entries.
        QVERIFY(!json.contains("\"hidden\": false"));
        QVERIFY(!json.contains("\"removed\": false"));
        QVERIFY(!json.contains("\"manual\": false"));
        QVERIFY(json.endsWith('\n'));
    }

    void documentedExampleParses()
    {
        QFile doc(QStringLiteral(KLINKER_SOURCE_DIR "/docs/configuration.md"));
        QVERIFY(doc.open(QIODevice::ReadOnly));
        const QByteArray markdown = doc.readAll();
        const qsizetype begin = markdown.indexOf("```json\n");
        QVERIFY(begin >= 0);
        const qsizetype end = markdown.indexOf("```", begin + 8);
        QVERIFY(end > begin);
        const QByteArray example = markdown.sliced(begin + 8, end - begin - 8);

        const auto parsed = configFromJson(example);
        QVERIFY2(parsed.config, qPrintable(parsed.error));
        QVERIFY(parsed.warnings.isEmpty());
        QCOMPARE(parsed.config->browsers.size(), 5);
        QCOMPARE(parsed.config->rules.size(), 3);
        QCOMPARE(parsed.config->rules[2].pattern, u"^https?://(www\\.)?youtube\\.com/"_s);
        QVERIFY(parsed.config->rules[1].name.isEmpty());
        QVERIFY(!parsed.config->rules[2].enabled);
        // Every chooser field is shown, with a value other than its default
        // where that helps to read it.
        const QJsonObject chooser = QJsonDocument::fromJson(example).object().value("chooser"_L1).toObject();
        QCOMPARE(chooser.keys(), (QStringList{u"closeOnFocusLoss"_s, u"iconSize"_s, u"showBrowserNames"_s, u"style"_s}));
        QCOMPARE(parsed.config->chooser, (ChooserSettings{ChooserStyle::Horizontal, 64, true, false}));
    }

    void invalidDocuments_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("errorContains");
        QTest::newRow("syntax") << QByteArray("{\n  \"version\": 1,\n  oops\n}") << u"line 3"_s;
        QTest::newRow("empty") << QByteArray() << QString();
        QTest::newRow("not an object") << QByteArray("[]") << QString();
        QTest::newRow("missing version") << QByteArray("{}") << u"version"_s;
        QTest::newRow("newer version") << QByteArray(R"({"version": 2})") << u"newer"_s;
        QTest::newRow("version string") << QByteArray(R"({"version": "1"})") << u"version"_s;
        QTest::newRow("version fraction") << QByteArray(R"({"version": 1.5})") << u"version"_s;
        QTest::newRow("browsers not array") << QByteArray(R"({"version": 1, "browsers": {}})") << u"browsers"_s;
        QTest::newRow("browser not object") << QByteArray(R"({"version": 1, "browsers": ["x"]})") << u"browsers[0]"_s;
        QTest::newRow("browser without id") << QByteArray(R"({"version": 1, "browsers": [{}]})") << u"browsers[0].id"_s;
        QTest::newRow("browser bad hidden") << QByteArray(R"({"version": 1, "browsers": [{"id": "a", "hidden": "yes"}]})")
                                            << u"browsers[0].hidden"_s;
        QTest::newRow("browser bad arguments") << QByteArray(R"({"version": 1, "browsers": [{"id": "a", "arguments": ["x", 1]}]})")
                                               << u"browsers[0].arguments"_s;
        QTest::newRow("duplicate browser") << QByteArray(R"({"version": 1, "browsers": [{"id": "a"}, {"id": "a"}]})")
                                           << u"browsers[1].id"_s;
        QTest::newRow("manual removed") << QByteArray(R"({"version": 1, "browsers": [{"id": "m", "manual": true, "program": "x", "removed": true}]})")
                                        << u"browsers[0]"_s;
        QTest::newRow("manual without program") << QByteArray(R"({"version": 1, "browsers": [{"id": "m", "manual": true}]})")
                                                << u"browsers[0].program"_s;
        QTest::newRow("rule unknown type") << QByteArray(R"({"version": 1, "rules": [{"type": "wildcard", "pattern": "x", "browser": "a"}]})")
                                           << u"rules[0].type"_s;
        QTest::newRow("rule missing type") << QByteArray(R"({"version": 1, "rules": [{"pattern": "x", "browser": "a"}]})") << u"rules[0].type"_s;
        QTest::newRow("rule missing pattern") << QByteArray(R"({"version": 1, "rules": [{"type": "glob", "browser": "a"}]})")
                                              << u"rules[0].pattern"_s;
        QTest::newRow("rule bad enabled") << QByteArray(R"({"version": 1, "rules": [{"type": "glob", "pattern": "x", "enabled": 1}]})")
                                          << u"rules[0].enabled"_s;
        QTest::newRow("privacy bad") << QByteArray(R"({"version": 1, "privacy": {"unwrapRedirects": "true"}})") << u"privacy.unwrapRedirects"_s;
    }

    void invalidDocuments()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, errorContains);
        const auto parsed = configFromJson(json);
        QVERIFY(!parsed.config);
        QVERIFY(!parsed.error.isEmpty());
        QVERIFY2(parsed.error.contains(errorContains), qPrintable(parsed.error));
    }

    void unknownKeysWarn()
    {
        const auto parsed = configFromJson(R"({"version": 1, "extra": 1, "browsers": [{"id": "a", "hiden": true}], "rules": [{"type": "glob", "pattern": "*", "browser": "a", "x": 0}]})");
        QVERIFY2(parsed.config, qPrintable(parsed.error));
        QCOMPARE(parsed.warnings.size(), 3);
        QVERIFY(parsed.warnings.join(u'\n').contains(u"browsers[0].hiden"_s));
        QVERIFY(!parsed.config->browsers[0].hidden);
    }

    void chooserDefaultsWithoutTheObject()
    {
        const ChooserSettings defaults;
        QCOMPARE(defaults.style, ChooserStyle::Horizontal);
        QCOMPARE(defaults.iconSize, 48);
        QVERIFY(defaults.showBrowserNames);
        QVERIFY(!defaults.closeOnFocusLoss);

        // A file without the chooser object gets the defaults.
        const auto old = configFromJson(R"({"version": 1, "privacy": {"unwrapRedirects": true}})");
        QVERIFY2(old.config, qPrintable(old.error));
        QCOMPARE(old.config->chooser, ChooserSettings());
        QVERIFY(old.warnings.isEmpty());

        // Missing fields keep their defaults.
        const auto partial = configFromJson(R"({"version": 1, "chooser": {"style": "horizontal"}})");
        QVERIFY2(partial.config, qPrintable(partial.error));
        QCOMPARE(partial.config->chooser, (ChooserSettings{ChooserStyle::Horizontal, 48, true, false}));

        // An explicitly saved vertical list is kept.
        const auto saved = configFromJson(R"({"version": 1, "chooser": {"style": "vertical"}})");
        QVERIFY2(saved.config, qPrintable(saved.error));
        QCOMPARE(saved.config->chooser, (ChooserSettings{ChooserStyle::Vertical, 48, true, false}));
    }

    void chooserRoundTripsWithExactNames()
    {
        Config config = sampleConfig();
        config.chooser = {ChooserStyle::Horizontal, 96, false, true};
        const QByteArray json = configToJson(config);
        const QJsonObject chooser = QJsonDocument::fromJson(json).object().value("chooser"_L1).toObject();
        QCOMPARE(chooser,
                 (QJsonObject{{u"style"_s, u"horizontal"_s}, {u"iconSize"_s, 96}, {u"showBrowserNames"_s, false}, {u"closeOnFocusLoss"_s, true}}));
        const auto parsed = configFromJson(json);
        QVERIFY2(parsed.config, qPrintable(parsed.error));
        QVERIFY(parsed.warnings.isEmpty());
        QCOMPARE(*parsed.config, config);

        for (const int size : {ChooserSettings::MinIconSize, ChooserSettings::MaxIconSize}) {
            config.chooser = {ChooserStyle::Vertical, size, true, false};
            QCOMPARE(configFromJson(configToJson(config)).config->chooser, config.chooser);
        }
    }

    void malformedChooserIsRejected_data()
    {
        QTest::addColumn<QByteArray>("chooser");
        QTest::addColumn<QString>("errorPath");
        QTest::newRow("not an object") << QByteArray(R"("horizontal")") << u"chooser"_s;
        QTest::newRow("style not a string") << QByteArray(R"({"style": 1})") << u"chooser.style"_s;
        QTest::newRow("unknown style") << QByteArray(R"({"style": "diagonal"})") << u"chooser.style"_s;
        QTest::newRow("style with other case") << QByteArray(R"({"style": "Horizontal"})") << u"chooser.style"_s;
        QTest::newRow("size as string") << QByteArray(R"({"iconSize": "48"})") << u"chooser.iconSize"_s;
        QTest::newRow("size not an integer") << QByteArray(R"({"iconSize": 47.5})") << u"chooser.iconSize"_s;
        QTest::newRow("size too small") << QByteArray(R"({"iconSize": 23})") << u"chooser.iconSize"_s;
        QTest::newRow("size too large") << QByteArray(R"({"iconSize": 129})") << u"chooser.iconSize"_s;
        QTest::newRow("size huge") << QByteArray(R"({"iconSize": 1e300})") << u"chooser.iconSize"_s;
        QTest::newRow("size negative") << QByteArray(R"({"iconSize": -48})") << u"chooser.iconSize"_s;
        QTest::newRow("size null") << QByteArray(R"({"iconSize": null})") << u"chooser.iconSize"_s;
        QTest::newRow("names not a bool") << QByteArray(R"({"showBrowserNames": "yes"})") << u"chooser.showBrowserNames"_s;
        QTest::newRow("focus not a bool") << QByteArray(R"({"closeOnFocusLoss": 1})") << u"chooser.closeOnFocusLoss"_s;
    }

    void malformedChooserIsRejected()
    {
        QFETCH(QByteArray, chooser);
        QFETCH(QString, errorPath);
        const auto parsed = configFromJson(R"({"version": 1, "chooser": )" + chooser + "}");
        QVERIFY(!parsed.config);
        QVERIFY2(parsed.error.startsWith(errorPath + u':'), qPrintable(parsed.error));
    }

    void unknownChooserKeysWarn()
    {
        const auto parsed = configFromJson(R"({"version": 1, "chooser": {"style": "horizontal", "colour": "red"}})");
        QVERIFY2(parsed.config, qPrintable(parsed.error));
        QCOMPARE(parsed.warnings, QStringList{u"chooser.colour: unknown key ignored."_s});
        QCOMPARE(parsed.config->chooser.style, ChooserStyle::Horizontal);
    }

    void defaultPathFollowsXdg()
    {
        const QByteArray old = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", "/tmp/klinker-xdg-config");
        QCOMPARE(ConfigStore::defaultPath(), u"/tmp/klinker-xdg-config/klinker/config.json"_s);
        old.isNull() ? qunsetenv("XDG_CONFIG_HOME") : qputenv("XDG_CONFIG_HOME", old);
    }

    // Only the given file is read or written

    void missingFileGivesDefaultsAndIgnoresSiblings()
    {
        Locations l;
        const QByteArray sibling = configToJson(sampleConfig());
        QVERIFY(Locations::write(l.sibling, sibling));
        ConfigStore store = l.store();
        const auto loaded = store.load();
        QCOMPARE(loaded.status, ConfigStore::LoadStatus::Missing);
        QCOMPARE(loaded.config, Config());
        QVERIFY(loaded.error.isEmpty());
        QVERIFY(!QFileInfo::exists(QFileInfo(l.current).absolutePath()));

        // The first save creates the file; the other application's stays as it is.
        QVERIFY(store.save(otherConfig()).ok);
        QCOMPARE(configFromJson(readFile(l.current)).config, otherConfig());
        QCOMPARE(readFile(l.sibling), sibling);
        QCOMPARE(QDir(QFileInfo(l.sibling).absolutePath()).entryList(QDir::Files | QDir::Hidden), QStringList{u"config.json"_s});
    }

    void existingFileIsReadAndSiblingsIgnored()
    {
        Locations l;
        QVERIFY(Locations::write(l.current, configToJson(otherConfig())));
        const QByteArray sibling = configToJson(sampleConfig());
        QVERIFY(Locations::write(l.sibling, sibling));
        ConfigStore store = l.store();
        QCOMPARE(store.load().config, otherConfig());
        QVERIFY(store.save(Config()).ok);
        QCOMPARE(configFromJson(readFile(l.current)).config, Config());
        QCOMPARE(readFile(l.sibling), sibling);
    }

    void noFileAtAllGivesDefaults()
    {
        Locations l;
        ConfigStore store = l.store();
        QCOMPARE(store.load().status, ConfigStore::LoadStatus::Missing);
        QCOMPARE(store.path(), l.current);
        QVERIFY(store.save(sampleConfig()).ok);
        QCOMPARE(QDir(l.dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList{u"klinker"_s});
        QCOMPARE(configFromJson(readFile(l.current)).config, sampleConfig());
    }

    void malformedSiblingIsIgnored()
    {
        Locations l;
        QVERIFY(Locations::write(l.sibling, "{ broken"));
        ConfigStore store = l.store();
        const auto loaded = store.load();
        QCOMPARE(loaded.status, ConfigStore::LoadStatus::Missing);
        QVERIFY(loaded.error.isEmpty());
        QVERIFY(store.save(sampleConfig()).ok);
        QCOMPARE(configFromJson(readFile(l.current)).config, sampleConfig());
        QCOMPARE(readFile(l.sibling), QByteArray("{ broken"));
    }

    void changedSiblingDoesNotBlockSaving()
    {
        Locations l;
        ConfigStore store = l.store();
        QCOMPARE(store.load().status, ConfigStore::LoadStatus::Missing);
        QVERIFY(Locations::write(l.sibling, configToJson(otherConfig())));
        QVERIFY(store.save(sampleConfig()).ok);
        QVERIFY(Locations::write(l.sibling, "{ broken"));
        QVERIFY(store.save(otherConfig()).ok);
        QCOMPARE(configFromJson(readFile(l.current)).config, otherConfig());
    }

    void explicitPathIsUsedAsGiven()
    {
        // As with --config FILE: the default location is neither read nor written.
        Locations l;
        const QByteArray atDefault = configToJson(sampleConfig());
        QVERIFY(Locations::write(l.current, atDefault));
        const QString path = l.dir.filePath(u"elsewhere/settings.json"_s);
        ConfigStore store(path);
        QCOMPARE(store.path(), path);
        QCOMPARE(store.load().status, ConfigStore::LoadStatus::Missing);
        QVERIFY(store.save(otherConfig()).ok);
        QCOMPARE(configFromJson(readFile(path)).config, otherConfig());
        QCOMPARE(readFile(l.current), atDefault);
        QCOMPARE(ConfigStore(path).load().config, otherConfig());
    }

    void changesSinceLoadingBlockSaving_data()
    {
        QTest::addColumn<bool>("existsAtLoad");
        QTest::addColumn<QString>("change");
        QTest::newRow("edited") << true << u"edit"_s;
        QTest::newRow("made malformed") << true << u"break"_s;
        QTest::newRow("deleted") << true << u"delete"_s;
        QTest::newRow("created after empty load") << false << u"edit"_s;
    }

    void changesSinceLoadingBlockSaving()
    {
        QFETCH(bool, existsAtLoad);
        QFETCH(QString, change);
        Locations l;
        if (existsAtLoad) {
            QVERIFY(Locations::write(l.current, configToJson(sampleConfig())));
        }
        ConfigStore store = l.store();
        store.load();

        if (change == u"edit"_s) {
            QVERIFY(Locations::write(l.current, configToJson(otherConfig())));
        } else if (change == u"break"_s) {
            QVERIFY(Locations::write(l.current, "{ broken"));
        } else {
            QVERIFY(QFile::remove(l.current));
        }
        const bool existsBefore = QFile::exists(l.current);
        const QByteArray before = readFile(l.current);

        Config mine = sampleConfig();
        mine.chooser.closeOnFocusLoss = true;
        const auto saved = store.save(mine);
        QVERIFY(!saved.ok);
        QVERIFY2(saved.error.contains(u"changed"_s) || saved.error.contains(u"not a valid"_s), qPrintable(saved.error));
        QCOMPARE(QFile::exists(l.current), existsBefore);
        QCOMPARE(readFile(l.current), before);

        // Loading again picks up the change, after which saving works.
        if (change != u"break"_s) {
            store.load();
            QVERIFY(store.save(mine).ok);
            QCOMPARE(configFromJson(readFile(l.current)).config, mine);
        }
    }

    void storeMissingThenSaveAndLoad()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"nested/klinker/config.json"_s);
        ConfigStore store(path);
        const auto loaded = store.load();
        QCOMPARE(loaded.status, ConfigStore::LoadStatus::Missing);
        QCOMPARE(loaded.config, Config());
        QVERIFY(!QFile::exists(path));

        const auto saved = store.save(sampleConfig());
        QVERIFY2(saved.ok, qPrintable(saved.error));
        QVERIFY(QFile::exists(path));
        QCOMPARE(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files | QDir::Hidden), QStringList{u"config.json"_s});

        ConfigStore other(path);
        const auto reloaded = other.load();
        QCOMPARE(reloaded.status, ConfigStore::LoadStatus::Loaded);
        QCOMPARE(reloaded.config, sampleConfig());

        // Saving again from the same store works (its own write is not a conflict).
        Config changed = sampleConfig();
        changed.privacy.unwrapRedirects = true;
        QVERIFY(store.save(changed).ok);
        QVERIFY(store.save(changed).ok);
    }

    void saveRequiresLoad()
    {
        QTemporaryDir dir;
        ConfigStore store(dir.filePath(u"config.json"_s));
        const auto result = store.save(Config());
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
    }

    void malformedFileIsNeverOverwritten()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        const QByteArray broken = "{ \"version\": 1, \"rules\": [ oops";
        QVERIFY(writeFile(path, broken));

        ConfigStore store(path);
        const auto loaded = store.load();
        QCOMPARE(loaded.status, ConfigStore::LoadStatus::Invalid);
        QVERIFY(!loaded.error.isEmpty());
        QVERIFY(loaded.error.contains(path));
        QCOMPARE(loaded.config, Config());

        const auto saved = store.save(sampleConfig());
        QVERIFY(!saved.ok);
        QVERIFY(!saved.error.isEmpty());
        QCOMPARE(readFile(path), broken);

        // Semantically invalid JSON is protected too.
        QVERIFY(writeFile(path, R"({"version": 99})"));
        QVERIFY(!store.save(sampleConfig()).ok);
        QCOMPARE(readFile(path), QByteArray(R"({"version": 99})"));
    }

    void invalidOutgoingConfigIsNeverWritten_data()
    {
        QTest::addColumn<Config>("config");
        QTest::addColumn<QString>("errorPath");

        // What BrowserCatalog::addManual({}) produces: a manual browser without a program.
        BrowserCatalog catalog;
        QVERIFY(!catalog.addManual({}).isEmpty());
        Config emptyManual = sampleConfig();
        emptyManual.browsers = catalog.settings();
        QTest::newRow("manual without program") << emptyManual << u"browsers[0].program"_s;

        Config duplicate = sampleConfig();
        duplicate.browsers.append(duplicate.browsers.first());
        QTest::newRow("duplicate browser id") << duplicate << u"browsers[4].id"_s;

        Config emptyId = sampleConfig();
        emptyId.browsers[1].id.clear();
        QTest::newRow("empty browser id") << emptyId << u"browsers[1].id"_s;

        Config removedManual = sampleConfig();
        removedManual.browsers[3].removed = true;
        QTest::newRow("removed manual browser") << removedManual << u"browsers[3]"_s;
    }

    void invalidOutgoingConfigIsNeverWritten()
    {
        QFETCH(Config, config);
        QFETCH(QString, errorPath);

        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        const QByteArray valid = configToJson(sampleConfig());
        QVERIFY(writeFile(path, valid));

        ConfigStore store(path);
        QCOMPARE(store.load().status, ConfigStore::LoadStatus::Loaded);

        const auto saved = store.save(config);
        QVERIFY(!saved.ok);
        QVERIFY2(saved.error.contains(errorPath), qPrintable(saved.error));
        QCOMPARE(readFile(path), valid);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{u"config.json"_s});

        ConfigStore other(path);
        const auto reloaded = other.load();
        QCOMPARE(reloaded.status, ConfigStore::LoadStatus::Loaded);
        QCOMPARE(reloaded.config, sampleConfig());

        // The rejected save is not a conflict for the next valid one.
        Config changed = sampleConfig();
        changed.privacy.unwrapRedirects = true;
        QVERIFY(store.save(changed).ok);
        QCOMPARE(ConfigStore(path).load().config, changed);
    }

    void invalidOutgoingConfigDoesNotCreateFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        ConfigStore store(path);
        QCOMPARE(store.load().status, ConfigStore::LoadStatus::Missing);
        Config config;
        config.browsers = {BrowserSetting{}};
        QVERIFY(!store.save(config).ok);
        QVERIFY(!QFile::exists(path));
        QVERIFY(store.save(sampleConfig()).ok);
    }

    void concurrentModificationIsDetected()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        ConfigStore first(path);
        ConfigStore second(path);
        first.load();
        second.load();

        QVERIFY(first.save(sampleConfig()).ok);
        const QByteArray afterFirst = readFile(path);

        const auto conflict = second.save(Config());
        QVERIFY(!conflict.ok);
        QVERIFY(!conflict.error.isEmpty());
        QCOMPARE(readFile(path), afterFirst);

        QCOMPARE(second.load().config, sampleConfig());
        QVERIFY(second.save(Config()).ok);

        // A file deleted after loading is also a change.
        QVERIFY(QFile::remove(path));
        QVERIFY(!second.save(sampleConfig()).ok);
    }

    void heldLockFailsCleanly()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        ConfigStore store(path);
        store.load();
        QLockFile lock(path + u".lock"_s);
        QVERIFY(lock.tryLock(0));
        const auto result = store.save(sampleConfig());
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!QFile::exists(path));
        lock.unlock();
        QVERIFY(store.save(sampleConfig()).ok);
    }

    void saveFailureIsReported()
    {
        QTemporaryDir dir;
        // Parent "directory" is a regular file.
        QVERIFY(writeFile(dir.filePath(u"not-a-dir"_s), "x"));
        ConfigStore blocked(dir.filePath(u"not-a-dir/config.json"_s));
        blocked.load();
        auto result = blocked.save(sampleConfig());
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());

        if (geteuid() == 0) {
            QSKIP("Permission checks do not apply to root");
        }
        const QString readOnly = dir.filePath(u"read-only"_s);
        QVERIFY(QDir().mkpath(readOnly));
        QVERIFY(QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        ConfigStore store(readOnly + u"/config.json"_s);
        store.load();
        result = store.save(sampleConfig());
        QVERIFY(!result.ok);
        QVERIFY(!result.error.isEmpty());
        QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    void unreadableFileIsInvalid()
    {
        if (geteuid() == 0) {
            QSKIP("Permission checks do not apply to root");
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(u"config.json"_s);
        QVERIFY(writeFile(path, configToJson(sampleConfig())));
        QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
        ConfigStore store(path);
        const auto loaded = store.load();
        QCOMPARE(loaded.status, ConfigStore::LoadStatus::Invalid);
        QVERIFY(!store.save(Config()).ok);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(readFile(path), configToJson(sampleConfig()));
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
