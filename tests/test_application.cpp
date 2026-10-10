#include "testsupport.h"

#include "app/application.h"
#include "app/commandline.h"
#include "app/launcher.h"
#include "app/linkpipeline.h"
#include "ui/chooserwindow.h"
#include "ui/placement.h"
#include "ui/settingsdialog.h"
#include "ui/theme.h"

#include <KMessageWidget>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QScreen>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListView>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;
using kbs::Application;
using kbs::BrowserSetting;
using kbs::ChooserWindow;
using kbs::CommandLine;
using kbs::Config;
using kbs::Rule;
using kbs::RuleType;

namespace
{

const QString Fixture = QStringLiteral(KBS_CAPTURE_FIXTURE);
const QString AppBinary = QStringLiteral(KBS_APP_EXECUTABLE);

BrowserSetting manualBrowser(const QString &id, const QString &name, const QString &program, const QStringList &arguments)
{
    BrowserSetting s;
    s.id = id;
    s.manual = true;
    s.name = name;
    s.program = program;
    s.arguments = arguments;
    return s;
}

// A temporary home for one test: config file, empty discovery directory and
// a directory the capture fixture writes into.
struct Sandbox {
    QTemporaryDir dir;
    QString configPath = dir.path() + u"/config/kde-browser-switcher/config.json"_s;
    QString dataDir = dir.path() + u"/data"_s;
    QString captureDir = dir.path() + u"/captures"_s;

    Sandbox()
    {
        QDir().mkpath(dataDir);
        QDir().mkpath(captureDir);
    }

    BrowserSetting capture(const QString &id, const QString &name) const
    {
        return manualBrowser(id, name, Fixture, {u"--out"_s, captureDir, u"--browser="_s + id, u"{url}"_s});
    }

    void writeConfig(const Config &config) const
    {
        writeBytes(kbs::configToJson(config));
    }

    void writeBytes(const QByteArray &bytes) const
    {
        QDir().mkpath(QFileInfo(configPath).absolutePath());
        QFile file(configPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
    }

    QByteArray configBytes() const
    {
        QFile file(configPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // Arguments each fixture run received, sorted for comparison.
    QList<QStringList> captures() const
    {
        QList<QStringList> result;
        const QDir dir(captureDir);
        for (const QString &name : dir.entryList({u"*.json"_s}, QDir::Files)) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            QStringList arguments;
            for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
                arguments.append(value.toString());
            }
            result.append(arguments);
        }
        std::ranges::sort(result);
        return result;
    }

    Application::Options options(bool alwaysChoose = false) const
    {
        Application::Options o;
        o.configPath = configPath;
        o.dataDirs = QStringList{dataDir};
        o.alwaysChoose = alwaysChoose;
        return o;
    }
};

QListView *chooserList(Application &app)
{
    return app.chooser() ? app.chooser()->findChild<QListView *>(u"browserList"_s) : nullptr;
}

// Minimal HTTP server: each path maps to a status and optional Location.
// Requests for stalled paths wait until release().
class RedirectServer : public QObject
{
public:
    QHash<QByteArray, std::pair<int, QByteArray>> routes;
    QSet<QByteArray> stalled;
    QList<QByteArray> requests;

    RedirectServer()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray request = socket->readAll();
                    const QByteArray line = request.left(request.indexOf("\r\n"));
                    const QByteArray path = line.split(' ').value(1);
                    requests.append(line);
                    if (stalled.contains(path)) {
                        m_waiting.append({path, socket});
                        return;
                    }
                    respond(socket, path);
                });
            }
        });
    }

    QString url(const QByteArray &path) const
    {
        return u"http://127.0.0.1:%1"_s.arg(m_server.serverPort()) + QString::fromLatin1(path);
    }

    bool requested(const QByteArray &path) const
    {
        return std::ranges::any_of(requests, [&path](const QByteArray &line) {
            return line.split(' ').value(1) == path;
        });
    }

    // Answers the waiting requests for `path` as routed; later ones are answered at once.
    void release(const QByteArray &path)
    {
        stalled.remove(path);
        for (qsizetype i = m_waiting.size() - 1; i >= 0; --i) {
            if (m_waiting.at(i).first == path) {
                if (QTcpSocket *socket = m_waiting.at(i).second) {
                    respond(socket, path);
                }
                m_waiting.removeAt(i);
            }
        }
    }

private:
    void respond(QTcpSocket *socket, const QByteArray &path)
    {
        if (socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        const auto [status, location] = routes.value(path, {404, QByteArray()});
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: 0\r\nConnection: close\r\n";
        if (!location.isEmpty()) {
            response += "Location: " + location + "\r\n";
        }
        socket->write(response + "\r\n");
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<std::pair<QByteArray, QPointer<QTcpSocket>>> m_waiting;
};

// Clicks "Reload from Disk" on the settings message.
void reloadFromDisk(kbs::SettingsDialog *dialog)
{
    auto *message = dialog->findChild<KMessageWidget *>(u"settingsMessage"_s);
    QVERIFY(message && message->isVisible());
    for (QAction *action : message->actions()) {
        if (action->text().contains(u"Reload"_s)) {
            action->trigger();
            return;
        }
    }
    QFAIL("no reload action");
}

void hideFirstBrowser(kbs::SettingsDialog *dialog)
{
    auto *table = dialog->findChild<QTreeWidget *>(u"browserTable"_s);
    table->setCurrentItem(table->topLevelItem(0));
    dialog->findChild<QPushButton *>(u"hideBrowserButton"_s)->click();
}

QCheckBox *settingsCheck(Application &app, const QString &name)
{
    return app.settingsDialog() ? app.settingsDialog()->findChild<QCheckBox *>(name) : nullptr;
}

// The default configuration files in the test's private $XDG_CONFIG_HOME,
// removed again afterwards.
struct DefaultLocations {
    QString current = kbs::ConfigStore::defaultPath();
    QString legacy = kbs::ConfigStore::legacyDefaultPath();

    DefaultLocations()
    {
        QVERIFY(current.startsWith(QDir::tempPath()));
        QVERIFY(!QFile::exists(current) && !QFile::exists(legacy));
    }

    ~DefaultLocations()
    {
        QDir(QFileInfo(current).absolutePath()).removeRecursively();
        QDir(QFileInfo(legacy).absolutePath()).removeRecursively();
    }

    static void write(const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
};

// Links to /slow are redirected to /target, which a rule opens with "a"; the
// chooser closes when it loses the focus.
Config routedAfterRedirect(const Sandbox &box)
{
    Config config;
    config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
    config.rules = {{QString(), true, RuleType::Glob, u"http://127.0.0.1:*/target"_s, u"a"_s}};
    config.privacy.unwrapRedirects = true;
    config.chooser.closeOnFocusLoss = true;
    return config;
}

void stallRedirect(RedirectServer &server)
{
    server.routes.insert("/slow", {302, "/target"});
    server.routes.insert("/target", {200, {}});
    server.stalled.insert("/slow");
}

Config configOnDisk(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return kbs::configFromJson(file.readAll()).config.value_or(Config());
}

} // namespace

class TestApplication : public QObject
{
    Q_OBJECT

public:
    static void initMain()
    {
        kbs::test::isolateEnvironment();
    }

private Q_SLOTS:
    void initTestCase()
    {
        kbs::test::applyTheme(false);
        QVERIFY(QFileInfo(Fixture).isExecutable());
    }

    // Command line

    void commandLineModes()
    {
        CommandLine none = kbs::parseCommandLine({u"kde-browser-switcher"_s});
        QCOMPARE(none.action, CommandLine::Action::Run);
        QVERIFY(none.settings);
        QVERIFY(none.urls.isEmpty());

        CommandLine settings = kbs::parseCommandLine({u"kbs"_s, u"--settings"_s});
        QVERIFY(settings.settings);

        CommandLine links = kbs::parseCommandLine(
            {u"kbs"_s, u"--choose"_s, u"--config"_s, u"/tmp/x.json"_s, u"https://a.example/"_s, u"http://b.example/x"_s});
        QCOMPARE(links.action, CommandLine::Action::Run);
        QVERIFY(!links.settings);
        QVERIFY(links.choose);
        QCOMPARE(links.configPath, u"/tmp/x.json"_s);
        QCOMPARE(links.urls, (QStringList{u"https://a.example/"_s, u"http://b.example/x"_s}));

        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, u"--help"_s}).action, CommandLine::Action::ShowHelp);
        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, u"-h"_s}).action, CommandLine::Action::ShowHelp);
        // Only documented options exist; Qt's --help-all must not fall through to opening settings.
        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, u"--help-all"_s}).action, CommandLine::Action::Error);
        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, u"--version"_s}).action, CommandLine::Action::ShowVersion);
        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, u"--bogus"_s}).action, CommandLine::Action::Error);
    }

    void linksArePreservedExactly()
    {
        const QString odd = u"https://Example.COM:8443/a%2Fb/%7Euser/?q=a+b&q=a%20b&&utm_source=x#frag?x=1"_s;
        QCOMPARE(kbs::parseCommandLine({u"kbs"_s, odd}).urls, QStringList{odd});
    }

    void rawArgumentsKeepEveryByte()
    {
        QCOMPARE(kbs::decodeArgument(QByteArray("https://e.example/caf\xc3\xa9")), u"https://e.example/café"_s);
        // Invalid UTF-8 would otherwise turn into U+FFFD replacement characters.
        QCOMPARE(kbs::decodeArgument(QByteArray("https://e.example/\xff\xfe?a=\xe9&b=%41")), u"https://e.example/%FF%FE?a=%E9&b=%41"_s);
    }

    void invalidLinksAreRejected_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("javascript") << u"javascript:alert(1)"_s;
        QTest::newRow("ftp") << u"ftp://example.com/file"_s;
        QTest::newRow("no host") << u"https://"_s;
        QTest::newRow("space") << u"https://exa mple.com/"_s;
        QTest::newRow("newline") << u"https://example.com/\nX-Injected: 1"_s;
        QTest::newRow("relative") << u"example.com/path"_s;
        QTest::newRow("empty") << u""_s;
        QTest::newRow("data") << u"data:text/html,<script>x</script>"_s;
    }

    void invalidLinksAreRejected()
    {
        QFETCH(QString, url);
        QVERIFY(!kbs::urlArgumentError(url).isEmpty());
        const CommandLine parsed = kbs::parseCommandLine({u"kbs"_s, u"https://ok.example/"_s, url});
        QCOMPARE(parsed.action, CommandLine::Action::Error);
        QVERIFY(!parsed.error.isEmpty());
    }

    void localFilesBecomeFileUrls()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + u"/page with space.html"_s;
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const CommandLine parsed = kbs::parseCommandLine({u"kbs"_s, path});
        QCOMPARE(parsed.action, CommandLine::Action::Run);
        QCOMPARE(parsed.urls, QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)});
        QVERIFY(kbs::urlArgumentError(u"file:///etc/hosts"_s).isEmpty());
    }

    void binaryHelpVersionAndErrors()
    {
        QCOMPARE(QFileInfo(AppBinary).fileName(), u"klinker"_s);
        QProcess help;
        help.start(AppBinary, {u"--help"_s});
        QVERIFY(help.waitForFinished(10000));
        QCOMPARE(help.exitCode(), 0);
        const QString helpText = QString::fromUtf8(help.readAllStandardOutput());
        // Qt names the program as it was invoked: "klinker" when found in PATH.
        QVERIFY2(helpText.startsWith(u"Usage: %1 "_s.arg(AppBinary)), qPrintable(helpText));
        QVERIFY(helpText.contains(u"--settings"_s));
        QVERIFY(helpText.contains(u"--choose"_s));
        QVERIFY(helpText.contains(u"--config"_s));

        QProcess version;
        version.start(AppBinary, {u"--version"_s});
        QVERIFY(version.waitForFinished(10000));
        QCOMPARE(version.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(version.readAllStandardOutput()), u"klinker %1\n"_s.arg(QStringLiteral(KBS_VERSION)));

        QProcess bad;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        bad.setProcessEnvironment(env);
        bad.start(AppBinary, {u"javascript:alert(1)"_s});
        QVERIFY(bad.waitForFinished(10000));
        QCOMPARE(bad.exitCode(), 2);
        const QString error = QString::fromUtf8(bad.readAllStandardError());
        QVERIFY(error.contains(u"javascript:alert(1)"_s));
        QVERIFY2(error.startsWith(u"klinker: "_s) && error.contains(u"“klinker --help”"_s), qPrintable(error));
    }

    void desktopFileUsesTheKlinkerIdentity()
    {
        QFile file(QStringLiteral(KBS_SOURCE_DIR "/data/io.github.manicminer.Klinker.desktop"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        const QStringList lines = text.split(u'\n');
        for (const QString &line : {u"Name=Klinker"_s, u"Exec=klinker %U"_s, u"TryExec=klinker"_s, u"Icon=io.github.manicminer.Klinker"_s,
                                    u"Name=Configure Klinker"_s, u"Exec=klinker --settings"_s}) {
            QVERIFY2(lines.contains(line), qPrintable(line));
        }
        QVERIFY(!text.contains(u"Browser Switcher"_s));
        QVERIFY(QFile::exists(QStringLiteral(KBS_SOURCE_DIR "/data/icons/io.github.manicminer.Klinker.svg")));
        QCOMPARE(kbs::AppDisplayName.toString(), u"Klinker"_s);
        QCOMPARE(kbs::AppId.toString(), u"io.github.manicminer.Klinker"_s);
    }

    // Launching

    void launchPassesTheUrlAsOneArgument()
    {
        Sandbox box;
        kbs::Browser browser;
        browser.id = u"cap"_s;
        browser.name = u"Capture"_s;
        browser.command = {Fixture, {u"--out"_s, box.captureDir, u"--flag"_s, u"{url}"_s}};
        const QString url = u"https://example.com/a;b|c&d$(touch x)`id`?q='\"*#frag"_s;
        const kbs::LaunchResult result = kbs::launchBrowser(browser, url, kbs::appSelfIdentity());
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.pid > 0);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(), (QStringList{u"--flag"_s, url}));
        QVERIFY(!QFile::exists(u"x"_s));
    }

    void launchErrorsAreReported()
    {
        kbs::Browser missing;
        missing.name = u"Missing"_s;
        missing.command = {u"/nonexistent/browser"_s, {u"{url}"_s}};
        kbs::LaunchResult result = kbs::launchBrowser(missing, u"https://example.com/"_s, kbs::appSelfIdentity());
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(u"Missing"_s));

        for (const QString &name : {u"klinker"_s, u"kde-browser-switcher"_s, AppBinary}) {
            kbs::Browser self;
            self.name = u"Loop"_s;
            self.command = {name, {u"{url}"_s}};
            result = kbs::launchBrowser(self, u"https://example.com/"_s, kbs::appSelfIdentity());
            QVERIFY2(!result.ok, qPrintable(name));
        }

        kbs::Browser ok;
        ok.name = u"Fixture"_s;
        ok.command = {Fixture, {u"--out"_s, u"/nonexistent"_s, u"{url}"_s}};
        result = kbs::launchBrowser(ok, u"-oops"_s, kbs::appSelfIdentity());
        QVERIFY(!result.ok);
    }

    void appIdentityUsesTheInstalledDesktopId()
    {
        const kbs::SelfIdentity self = kbs::appSelfIdentity();
        QCOMPARE(self.desktopIds.first(), u"io.github.manicminer.Klinker.desktop"_s);
        // Earlier installations under the old name are never offered or started either.
        QVERIFY(self.desktopIds.contains(u"io.github.manicminer.KdeBrowserSwitcher.desktop"_s));
        QVERIFY(self.desktopIds.contains(u"kde-browser-switcher.desktop"_s));
        QVERIFY(self.desktopIds.contains(u"klinker.desktop"_s));
        QCOMPARE(self.executableNames, (QStringList{u"klinker"_s, u"kde-browser-switcher"_s}));
        QVERIFY(self.executablePaths.contains(QCoreApplication::applicationFilePath()));
    }

    // Configuration of Browser Switcher, the name before Klinker

    void defaultLocationReadsTheLegacyConfiguration()
    {
        Sandbox box;
        DefaultLocations files;
        Config old;
        old.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        old.rules = {{QString(), true, RuleType::Domain, u"old.example"_s, u"b"_s}};
        const QByteArray legacy = kbs::configToJson(old);
        DefaultLocations::write(files.legacy, legacy);

        Application::Options options = box.options();
        options.configPath.clear();
        Application app(options);
        QCOMPARE(app.config(), old);

        // Opening links reads the old file and writes nothing.
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://old.example/"_s});
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QVERIFY(!QFile::exists(files.current));
        QVERIFY(!QFileInfo::exists(QFileInfo(files.current).absolutePath()));

        // Saving the settings imports them into the new location only.
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!app.settingsDialog());
        Config expected = old;
        expected.privacy.stripTrackingParameters = true;
        QCOMPARE(configOnDisk(files.current), expected);
        QCOMPARE(DefaultLocations::read(files.legacy), legacy);
        QCOMPARE(app.config(), expected);
    }

    void explicitConfigNeverReadsTheLegacyConfiguration()
    {
        Sandbox box;
        DefaultLocations files;
        Config old;
        old.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        const QByteArray legacy = kbs::configToJson(old);
        DefaultLocations::write(files.legacy, legacy);

        Application app(box.options());
        QCOMPARE(app.config(), Config());
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        QVERIFY(app.settingsDialog()->apply());
        QVERIFY(QFile::exists(box.configPath));
        QVERIFY(!QFile::exists(files.current));
        QCOMPARE(DefaultLocations::read(files.legacy), legacy);
        app.settingsDialog()->reject();
    }

    // Preprocessing

    void pipelineWithoutPrivacyKeepsTheLink()
    {
        kbs::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &kbs::LinkPipeline::finished);
        const QString url = u"https://example.com/?utm_source=x&id=1#f"_s;
        pipeline.start(url, {});
        QCOMPARE(finished.size(), 0); // never synchronous
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<kbs::ProcessedLink>();
        QCOMPARE(link.originalUrl, url);
        QCOMPARE(link.finalUrl, url);
        QVERIFY(!link.warning);
        QVERIFY(!link.status.isEmpty());
    }

    void pipelineStripsTrackingWithoutNetwork()
    {
        kbs::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &kbs::LinkPipeline::finished);
        QVERIFY(!kbs::LinkPipeline::needsNetwork(u"https://e.example/"_s, {true, false}));
        pipeline.start(u"https://e.example/p?utm_source=x&id=1&fbclid=y#f"_s, {true, false});
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<kbs::ProcessedLink>();
        QCOMPARE(link.finalUrl, u"https://e.example/p?id=1#f"_s);
        QVERIFY(link.status.contains(u"2"_s));
    }

    void pipelineFollowsRedirectsAndStripsEveryHop()
    {
        RedirectServer server;
        server.routes.insert("/start?utm_source=a", {302, "/middle?x=1&utm_medium=b"});
        server.routes.insert("/start", {302, "/middle?x=1&utm_medium=b"});
        server.routes.insert("/middle?x=1", {301, "/final?keep=1&gclid=z"});
        server.routes.insert("/final?keep=1", {200, {}});
        kbs::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &kbs::LinkPipeline::finished);
        const QString start = server.url("/start?utm_source=a");
        QVERIFY(kbs::LinkPipeline::needsNetwork(start, {true, true}));
        pipeline.start(start, {true, true});
        QVERIFY(pipeline.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto link = finished.first().at(0).value<kbs::ProcessedLink>();
        QCOMPARE(link.originalUrl, start);
        QCOMPARE(link.finalUrl, server.url("/final?keep=1"));
        QVERIFY2(!link.warning, qPrintable(link.status));
        QVERIFY(link.status.contains(u"2 redirects"_s));
        // No request ever carried a tracking parameter.
        for (const QByteArray &request : std::as_const(server.requests)) {
            QVERIFY2(!request.contains("utm_") && !request.contains("gclid"), request.constData());
        }
    }

    void pipelineCanBeSkipped()
    {
        RedirectServer server;
        server.stalled.insert("/slow");
        kbs::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &kbs::LinkPipeline::finished);
        pipeline.start(server.url("/slow"), {false, true});
        QTRY_VERIFY(!server.requests.isEmpty());
        pipeline.skip();
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<kbs::ProcessedLink>();
        QCOMPARE(link.finalUrl, server.url("/slow"));
        QVERIFY(link.skipped);
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
    }

    void pipelineReportsFailuresWithASafeLink()
    {
        RedirectServer server;
        server.routes.insert("/bad", {302, "javascript:alert(1)"});
        kbs::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &kbs::LinkPipeline::finished);
        pipeline.start(server.url("/bad"), {false, true});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto link = finished.first().at(0).value<kbs::ProcessedLink>();
        QVERIFY(link.warning);
        QCOMPARE(link.finalUrl, server.url("/bad"));
        QVERIFY(!link.status.isEmpty());
    }

    // Whole flow

    void ruleRoutesWithoutAChooser()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.rules = {{QString(), true, RuleType::Subdomain, u"work.example"_s, u"b"_s}};
        box.writeConfig(config);
        const QByteArray before = box.configBytes();

        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        const QString url = u"https://git.work.example/repo?x=1&y=$(id)#top"_s;
        app.openUrls({url});
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QVERIFY(!app.chooser());
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(), (QStringList{u"--browser=b"_s, url}));
        QCOMPARE(box.configBytes(), before);
    }

    void chooserLaunchesTheChoiceAndSavesNothing()
    {
        Sandbox box;
        kbs::test::writeBrowserDesktopFile(box.dataDir, u"detected.desktop"_s, u"Detected"_s, Fixture + u" --out "_s + box.captureDir + u" --browser=detected %u"_s);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->displayedBrowserIds(), QStringList{u"detected.desktop"_s});
        QCOMPARE(app.chooser()->finalUrl(), u"https://kde.org/"_s);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(finished.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(), (QStringList{u"--browser=detected"_s, u"https://kde.org/"_s}));
        QTRY_VERIFY(!app.chooser());
        QVERIFY(!QFile::exists(box.configPath));
    }

    void cancelMovesToTheNextLink()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://one.example/"_s, u"https://two.example/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->finalUrl(), u"https://one.example/"_s);
        QTest::keyClick(chooserList(app), Qt::Key_Escape);
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && app.chooser()->finalUrl() == u"https://two.example/"_s);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(finished.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first().last(), u"https://two.example/"_s);
    }

    void multipleLinksAreHandledInOrder()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.rules = {{QString(), true, RuleType::Domain, u"routed.example"_s, u"b"_s}};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://routed.example/1"_s, u"https://ask.example/2"_s, u"https://routed.example/3"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(launched.size(), 1);
        QCOMPARE(app.chooser()->finalUrl(), u"https://ask.example/2"_s);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(finished.size(), 1);
        QStringList order;
        for (const auto &args : launched) {
            order.append(args.at(0).toString() + u' ' + args.at(1).toString());
        }
        QCOMPARE(order, (QStringList{u"b https://routed.example/1"_s, u"a https://ask.example/2"_s, u"b https://routed.example/3"_s}));
        QTRY_COMPARE(box.captures().size(), 3);
    }

    void launchFailureKeepsTheChooserForARetry()
    {
        Sandbox box;
        Config config;
        config.browsers = {manualBrowser(u"broken"_s, u"Broken"_s, u"/nonexistent/browser"_s, {u"{url}"_s}),
                           box.capture(u"ok"_s, u"Works"_s)};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_VERIFY(app.chooser()->messageText().contains(u"Broken"_s));
        QVERIFY(app.chooser()->isVisible());
        QCOMPARE(finished.size(), 0);
        kbs::test::saveScreenshot(app.chooser(), u"app-launch-error-light"_s);

        QTest::keyClick(chooserList(app), Qt::Key_2);
        QTRY_COMPARE(finished.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
    }

    void failingRuleFallsBackToTheChooser()
    {
        Sandbox box;
        Config config;
        config.browsers = {manualBrowser(u"broken"_s, u"Broken"_s, u"/nonexistent/browser"_s, {u"{url}"_s}),
                           box.capture(u"ok"_s, u"Works"_s)};
        config.rules = {{u"Docs"_s, true, RuleType::Domain, u"docs.example"_s, u"broken"_s}};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://docs.example/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QVERIFY(app.chooser()->messageText().contains(u"Broken"_s));
    }

    void trackingIsRemovedBeforeRoutingAndLaunch()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.rules = {{QString(), true, RuleType::Glob, u"https://example.com/page?id=7"_s, u"a"_s}};
        config.privacy.stripTrackingParameters = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://example.com/page?utm_source=news&id=7&fbclid=abc"_s});
        QTRY_COMPARE(finished.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first().last(), u"https://example.com/page?id=7"_s);
    }

    void redirectsAreResolvedBeforeRouting()
    {
        RedirectServer server;
        server.routes.insert("/short", {302, "/docs/manual"});
        server.routes.insert("/docs/manual", {200, {}});
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        // Matches only the resolved address, not the short link.
        config.rules = {{QString(), true, RuleType::Glob, u"http://127.0.0.1:*/docs/*"_s, u"b"_s}};
        config.privacy.unwrapRedirects = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/short")});
        // The chooser shows the busy state while the redirect is followed.
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QCOMPARE(launched.first().at(1).toString(), server.url("/docs/manual"));
    }

    void busyChooserBlocksChoosingUntilResolved()
    {
        RedirectServer server;
        server.stalled.insert("/slow");
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.privacy.unwrapRedirects = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow")});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QVERIFY(app.chooser()->isBusy());
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QCOMPARE(launched.size(), 0);
        kbs::test::saveScreenshot(app.chooser(), u"app-busy-light"_s);
        app.chooser()->findChild<QPushButton *>(u"skipButton"_s)->click();
        QTRY_VERIFY(!app.chooser()->isBusy());
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), server.url("/slow"));
    }

    void alwaysChooseBypassesRules()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.rules = {{QString(), true, RuleType::Glob, u"*"_s, u"a"_s}};
        box.writeConfig(config);
        Application app(box.options(true));
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(launched.size(), 0);
    }

    void hiddenBrowsersAreRoutableButNotListed()
    {
        Sandbox box;
        Config config;
        BrowserSetting hidden = box.capture(u"h"_s, u"Hidden"_s);
        hidden.hidden = true;
        config.browsers = {hidden, box.capture(u"v"_s, u"Visible"_s)};
        config.rules = {{QString(), true, RuleType::Domain, u"secret.example"_s, u"h"_s}};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://secret.example/"_s, u"https://other.example/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(launched.first().at(0).toString(), u"h"_s);
        QCOMPARE(app.chooser()->displayedBrowserIds(), QStringList{u"v"_s});
    }

    void emptyChooserStillOffersSettings()
    {
        Sandbox box;
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        auto *empty = app.chooser()->findChild<QWidget *>(u"emptyState"_s);
        QVERIFY(empty->isVisible());
        app.chooser()->findChild<QPushButton *>(u"emptyStateSettingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isVisible());
    }

    void savedSettingsRefreshTheChooser()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->displayedBrowserIds(), (QStringList{u"a"_s, u"b"_s}));

        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isVisible());
        // The layer-shell chooser would cover the dialog, so it steps aside.
        QVERIFY(!app.chooser()->isVisible());
        auto *table = app.settingsDialog()->findChild<QTreeWidget *>(u"browserTable"_s);
        table->setCurrentItem(table->topLevelItem(0));
        app.settingsDialog()->findChild<QPushButton *>(u"hideBrowserButton"_s)->click();
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();

        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->displayedBrowserIds(), QStringList{u"b"_s});
        QCOMPARE(app.config().browsers.at(0).hidden, true);
        QSignalSpy launched(&app, &Application::launched);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
    }

    void cancelledSettingsChangeNothing()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        box.writeConfig(config);
        const QByteArray before = box.configBytes();
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        auto *table = app.settingsDialog()->findChild<QTreeWidget *>(u"browserTable"_s);
        table->setCurrentItem(table->topLevelItem(0));
        app.settingsDialog()->findChild<QPushButton *>(u"hideBrowserButton"_s)->click();
        app.settingsDialog()->reject();
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->displayedBrowserIds(), (QStringList{u"a"_s, u"b"_s}));
        QCOMPARE(box.configBytes(), before);
        QCOMPARE(app.config(), config);
    }

    void settingsOnlyInvocationFinishesWhenClosed()
    {
        Sandbox box;
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isVisible());
        app.settingsDialog()->reject();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!QFile::exists(box.configPath));
    }

    void unusableConfigIsReportedAndNeverOverwritten()
    {
        Sandbox box;
        box.writeBytes("{ broken");
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QVERIFY(app.chooser()->messageText().contains(box.configPath));
        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        QVERIFY(!app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
        QCOMPARE(box.configBytes(), QByteArray("{ broken"));
    }

    void concurrentInvocationsDoNotTouchTheConfig()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.rules = {{QString(), true, RuleType::Glob, u"https://parallel.example/*"_s, u"a"_s}};
        box.writeConfig(config);
        const QByteArray before = box.configBytes();

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        env.insert(u"XDG_DATA_HOME"_s, box.dataDir);
        env.insert(u"XDG_DATA_DIRS"_s, box.dataDir);
        env.insert(u"XDG_CONFIG_HOME"_s, box.dir.path() + u"/xdg-config"_s);
        std::vector<std::unique_ptr<QProcess>> processes;
        for (int i = 0; i < 6; ++i) {
            auto process = std::make_unique<QProcess>();
            process->setProcessEnvironment(env);
            process->start(AppBinary, {u"--config"_s, box.configPath, u"https://parallel.example/%1"_s.arg(i)});
            processes.push_back(std::move(process));
        }
        for (auto &process : processes) {
            QVERIFY(process->waitForFinished(20000));
            QCOMPARE(process->exitStatus(), QProcess::NormalExit);
            QVERIFY2(process->exitCode() == 0, process->readAllStandardError().constData());
        }
        QTRY_COMPARE(box.captures().size(), 6);
        QCOMPARE(box.configBytes(), before);
        QVERIFY(!QFile::exists(box.configPath + u".lock"_s));
    }

    void twoSettingsWindowsCannotLoseEachOthersChanges()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application first(box.options());
        Application second(box.options());
        first.openSettings();
        second.openSettings();
        QTRY_VERIFY(first.settingsDialog() && second.settingsDialog());
        first.settingsDialog()->findChild<QCheckBox *>(u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(first.settingsDialog()->apply());
        const QByteArray afterFirst = box.configBytes();
        second.settingsDialog()->findChild<QCheckBox *>(u"unwrapRedirectsCheck"_s)->setChecked(true);
        QVERIFY(!second.settingsDialog()->apply());
        QCOMPARE(box.configBytes(), afterFirst);
        QVERIFY(second.settingsDialog()->findChild<KMessageWidget *>(u"settingsMessage"_s)->isVisible());
    }

    void cancelledReloadCannotRevertAnotherInvocation()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        box.writeConfig(config);
        Application first(box.options());
        Application second(box.options());
        second.openUrls({u"https://pending.example/"_s});
        QTRY_VERIFY(second.chooser() && second.chooser()->isVisible());

        first.openSettings();
        QTRY_VERIFY(first.settingsDialog());
        settingsCheck(first, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(first.settingsDialog()->apply());
        first.settingsDialog()->reject();

        second.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(second.settingsDialog());
        hideFirstBrowser(second.settingsDialog());
        QVERIFY(!second.settingsDialog()->apply());
        reloadFromDisk(second.settingsDialog());
        second.settingsDialog()->reject();
        QTRY_VERIFY(!second.settingsDialog() && second.chooser() && second.chooser()->isVisible());
        QCOMPARE(second.config(), config);

        second.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(second.settingsDialog());
        hideFirstBrowser(second.settingsDialog());
        // The cancelled reload was never adopted, so the file is still newer
        // than what this invocation loaded.
        QVERIFY(!second.settingsDialog()->apply());
        QVERIFY(configOnDisk(box.configPath).privacy.stripTrackingParameters);
        QVERIFY(!configOnDisk(box.configPath).browsers.at(0).hidden);
    }

    void reloadAfterApplyKeepsTheSavedBaseline()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(app.settingsDialog()->apply());
        QCOMPARE(app.config().privacy, (kbs::PrivacySettings{true, false}));

        {
            Application other(box.options());
            other.openSettings();
            QTRY_VERIFY(other.settingsDialog());
            settingsCheck(other, u"unwrapRedirectsCheck"_s)->setChecked(true);
            QVERIFY(other.settingsDialog()->apply());
            other.settingsDialog()->reject();
        }
        const QByteArray external = box.configBytes();

        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(false);
        QVERIFY(!app.settingsDialog()->apply());
        reloadFromDisk(app.settingsDialog());
        app.settingsDialog()->reject();
        QTRY_VERIFY(!app.settingsDialog());
        // Still what it applied, still paired with what it saved.
        QCOMPARE(app.config().privacy, (kbs::PrivacySettings{true, false}));

        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(false);
        QVERIFY(!app.settingsDialog()->apply());
        QCOMPARE(box.configBytes(), external);

        // Reload followed by Apply adopts the file and the new baseline together.
        reloadFromDisk(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(false);
        QVERIFY(app.settingsDialog()->apply());
        app.settingsDialog()->reject();
        QTRY_VERIFY(!app.settingsDialog());
        QCOMPARE(app.config().privacy, (kbs::PrivacySettings{false, true}));
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"unwrapRedirectsCheck"_s)->setChecked(false);
        QVERIFY(app.settingsDialog()->apply());
        QCOMPARE(configOnDisk(box.configPath).privacy, kbs::PrivacySettings());
    }

    // Chooser preferences

    void chooserUsesTheConfiguredPreferences()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.chooser = {kbs::ChooserStyle::Horizontal, 64, false, false};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->preferences(), config.chooser);
        QCOMPARE(chooserList(app)->flow(), QListView::LeftToRight);
        // Placed at its preferred size for the screen (the offscreen screen has
        // no name, so placement does not move it; see test_placement).
        const QSize maximum = kbs::maximumSize(app.chooser()->screen()->availableGeometry(), 8);
        QCOMPARE(app.chooser()->size(), app.chooser()->preferredSize(maximum));
        QVERIFY(app.chooser()->width() > app.chooser()->preferredSize(maximum).height());
        QSignalSpy launched(&app, &Application::launched);
        QTest::keyClick(chooserList(app), Qt::Key_2);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
    }

    void savedAppearanceAppliesWhenTheChooserReturns()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s), box.capture(u"c"_s, u"Gamma"_s)};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QPointer<ChooserWindow> chooser = app.chooser();
        QScreen *screen = chooser->screen();
        QCOMPARE(chooser->width(), ChooserWindow::CardWidth);

        chooser->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isVisible());
        auto *style = app.settingsDialog()->findChild<QComboBox *>(u"chooserStyleCombo"_s);
        style->setCurrentIndex(style->findData(u"horizontal"_s));
        app.settingsDialog()->findChild<QSpinBox *>(u"iconSizeSpin"_s)->setValue(96);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();

        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.settingsDialog());
        QCOMPARE(app.chooser(), chooser.data()); // the same link, the same window
        QCOMPARE(chooser->preferences(), (kbs::ChooserSettings{kbs::ChooserStyle::Horizontal, 96, true, false}));
        QCOMPARE(chooserList(app)->flow(), QListView::LeftToRight);
        // Refitted on the screen it was on.
        QCOMPARE(chooser->screen(), screen);
        QCOMPARE(chooser->size(), chooser->preferredSize(kbs::maximumSize(screen->availableGeometry(), 8)));
        QVERIFY(screen->availableGeometry().adjusted(8, 8, -8, -8).contains(chooser->geometry()));
        QSignalSpy launched(&app, &Application::launched);
        QTest::keyClick(chooserList(app), Qt::Key_3);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"c"_s);
    }

    // Closing on focus loss

    void focusLossKeepsTheChooserByDefault()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isActiveWindow());
        kbs::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        QVERIFY(app.chooser() && app.chooser()->isVisible());
        QSignalSpy launched(&app, &Application::launched);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(launched.size(), 1);
    }

    // As with Esc, the next link waiting is shown.
    void focusLossCancelsAndMovesToTheNextLink()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.chooser.closeOnFocusLoss = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://one.example/"_s, u"https://two.example/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isActiveWindow());
        QCOMPARE(app.chooser()->finalUrl(), u"https://one.example/"_s);
        kbs::test::focusOtherApplication();
        QTRY_VERIFY(app.chooser() && app.chooser()->finalUrl() == u"https://two.example/"_s);
        // The next chooser has the focus of its own and stays.
        QTRY_VERIFY(app.chooser()->isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        QCOMPARE(launched.size(), 0);
        QCOMPARE(finished.size(), 0);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), u"https://two.example/"_s);
        QTRY_COMPARE(box.captures().size(), 1);
    }

    void focusLossWhileResolvingNeverLaunches()
    {
        RedirectServer server;
        server.routes.insert("/slow", {302, "/target"});
        server.routes.insert("/target", {200, {}});
        server.stalled.insert("/slow");
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.rules = {{QString(), true, RuleType::Glob, u"*"_s, u"a"_s}};
        config.privacy.unwrapRedirects = true;
        config.chooser.closeOnFocusLoss = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow")});
        QTRY_VERIFY(server.requested("/slow"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isBusy() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> chooser = app.chooser();
        QSignalSpy cancelled(chooser.data(), &ChooserWindow::cancelled);

        kbs::test::focusOtherApplication();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(cancelled.size(), 1);
        QTRY_VERIFY(!chooser);
        server.release("/slow");
        QTest::qWait(300);
        QCOMPARE(launched.size(), 0);
        QCOMPARE(finished.size(), 1);
        QVERIFY(box.captures().isEmpty());
    }

    // Deciding whether the focus has really gone takes FocusLossDelayMs. A link
    // that becomes ready meanwhile waits for that decision: here the focus
    // stays away, so it is cancelled and never routed.
    void linkReadyBeforeTheFocusLossDecisionNeverLaunches()
    {
        RedirectServer server;
        stallRedirect(server);
        Sandbox box;
        box.writeConfig(routedAfterRedirect(box));
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow")});
        QTRY_VERIFY(server.requested("/slow"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isBusy() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> chooser = app.chooser();
        QSignalSpy cancelled(chooser.data(), &ChooserWindow::cancelled);

        kbs::test::focusOtherApplication();
        QVERIFY(chooser->isFocusLossPending());
        server.release("/slow");
        // Ready well within the delay: held back, the chooser still busy.
        QTRY_VERIFY_WITH_TIMEOUT(app.isRoutingDeferred(), ChooserWindow::FocusLossDelayMs);
        QVERIFY(chooser->isFocusLossPending() && chooser->isBusy());
        QCOMPARE(launched.size(), 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(cancelled.size(), 1);
        QTRY_VERIFY(!chooser);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 100);
        QCOMPARE(launched.size(), 0);
        QVERIFY(box.captures().isEmpty());
    }

    // The focus comes back before the decision: the waiting link is routed, once.
    void linkReadyBeforeTheFocusComesBackIsRoutedOnce()
    {
        RedirectServer server;
        stallRedirect(server);
        Sandbox box;
        box.writeConfig(routedAfterRedirect(box));
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow")});
        QTRY_VERIFY(server.requested("/slow"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isBusy() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> chooser = app.chooser();
        QSignalSpy cancelled(chooser.data(), &ChooserWindow::cancelled);
        QSignalSpy resolved(chooser.data(), &ChooserWindow::focusLossResolved);

        kbs::test::focusOtherApplication();
        QVERIFY(chooser->isFocusLossPending());
        server.release("/slow");
        // Ready well within the delay: held back, the chooser still busy.
        QTRY_VERIFY_WITH_TIMEOUT(app.isRoutingDeferred(), ChooserWindow::FocusLossDelayMs);
        QVERIFY(chooser->isFocusLossPending() && chooser->isBusy());
        QCOMPARE(launched.size(), 0);
        QVERIFY(chooser && chooser->isVisible());

        kbs::test::focusWindow(chooser);
        QCOMPARE(resolved.size(), 1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), server.url("/target"));
        QCOMPARE(cancelled.size(), 0);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 100);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(finished.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
    }

    // The settings opened before the decision hide the chooser, which is not
    // losing the focus; the waiting link is routed once they close.
    void linkReadyBeforeTheSettingsOpenIsRoutedOnceTheyClose()
    {
        RedirectServer server;
        stallRedirect(server);
        Sandbox box;
        box.writeConfig(routedAfterRedirect(box));
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow")});
        QTRY_VERIFY(server.requested("/slow"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isBusy() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> chooser = app.chooser();
        QSignalSpy cancelled(chooser.data(), &ChooserWindow::cancelled);
        QSignalSpy resolved(chooser.data(), &ChooserWindow::focusLossResolved);

        kbs::test::focusOtherApplication();
        QVERIFY(chooser->isFocusLossPending());
        server.release("/slow");
        // Ready well within the delay: held back, the chooser still busy.
        QTRY_VERIFY_WITH_TIMEOUT(app.isRoutingDeferred(), ChooserWindow::FocusLossDelayMs);
        QVERIFY(chooser->isFocusLossPending() && chooser->isBusy());
        QCOMPARE(launched.size(), 0);

        chooser->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && !chooser->isVisible());
        // Resolved by hiding, but still waiting, now for the settings.
        QCOMPARE(resolved.size(), 1);
        QVERIFY(!chooser->isFocusLossPending() && app.isRoutingDeferred());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 100);
        QCOMPARE(launched.size(), 0);
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(finished.size(), 0);

        app.settingsDialog()->reject();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), server.url("/target"));
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 100);
        QCOMPARE(launched.size(), 1);
        QTRY_COMPARE(box.captures().size(), 1);
    }

    // The cancelled link is dropped and the next one is shown; nothing of the
    // first link or its chooser can act on the next.
    void linkReadyBeforeTheFocusLossDecisionMovesToTheNextLink()
    {
        RedirectServer server;
        stallRedirect(server);
        Sandbox box;
        box.writeConfig(routedAfterRedirect(box));
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/slow"), u"https://ask.example/"_s});
        QTRY_VERIFY(server.requested("/slow"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isBusy() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> first = app.chooser();

        kbs::test::focusOtherApplication();
        QVERIFY(first->isFocusLossPending());
        server.release("/slow");
        // Ready well within the delay: held back, the chooser still busy.
        QTRY_VERIFY_WITH_TIMEOUT(app.isRoutingDeferred(), ChooserWindow::FocusLossDelayMs);
        QVERIFY(first->isFocusLossPending() && first->isBusy());
        QCOMPARE(launched.size(), 0);

        QTRY_VERIFY(!first && app.chooser() && app.chooser()->finalUrl() == u"https://ask.example/"_s);
        QTRY_VERIFY(app.chooser()->isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 100);
        QVERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(launched.size(), 0);
        QCOMPARE(finished.size(), 0);

        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), u"https://ask.example/"_s);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first().last(), u"https://ask.example/"_s);
    }

    // The chooser steps aside for the settings; switching to another
    // application meanwhile is not leaving the chooser.
    void settingsDoNotCountAsFocusLoss()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isActiveWindow());
        QPointer<ChooserWindow> chooser = app.chooser();

        // Turned on in the settings opened from the chooser.
        chooser->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isActiveWindow());
        settingsCheck(app, u"closeOnFocusLossCheck"_s)->setChecked(true);
        QVERIFY(app.settingsDialog()->apply());
        kbs::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        kbs::test::focusWindow(app.settingsDialog());
        QTRY_VERIFY(app.settingsDialog()->isActiveWindow());
        app.settingsDialog()->reject();

        QTRY_VERIFY(chooser && chooser->isVisible() && chooser->isActiveWindow());
        QCOMPARE(finished.size(), 0);
        QVERIFY(chooser->preferences().closeOnFocusLoss);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        QVERIFY(chooser && chooser->isVisible());

        kbs::test::focusOtherApplication();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 0);
    }

    // Privacy settings changed while a link is pending

    void privacyChangeDuringResolutionRestartsTheLink()
    {
        RedirectServer server;
        server.routes.insert("/page?utm_source=x", {200, {}});
        server.routes.insert("/page", {200, {}});
        server.stalled.insert("/page?utm_source=x");
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.rules = {{QString(), true, RuleType::Regex, u"utm_source="_s, u"a"_s}, {QString(), true, RuleType::Glob, u"*"_s, u"b"_s}};
        config.privacy.unwrapRedirects = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/page?utm_source=x")});
        QTRY_VERIFY(server.requested("/page?utm_source=x"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && app.chooser()->isBusy());

        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(app.settingsDialog()->apply());
        // The superseded request completing must not route or launch.
        server.release("/page?utm_source=x");
        QTest::qWait(200);
        QCOMPARE(launched.size(), 0);
        // The link starts again from the original, cleaned before the request,
        // and once ready nothing opens behind the settings window.
        QTRY_VERIFY(server.requested("/page"));
        QTest::qWait(200);
        QCOMPARE(launched.size(), 0);
        QVERIFY(box.captures().isEmpty());

        app.settingsDialog()->reject(); // closes; Apply has already saved
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QCOMPARE(launched.first().at(1).toString(), server.url("/page"));
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(), (QStringList{u"--browser=b"_s, server.url("/page")}));
    }

    void privacyChangeAfterResolutionUpdatesTheChoice()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy launched(&app, &Application::launched);
        const QString url = u"https://example.com/page?utm_source=news&id=7"_s;
        const QString clean = u"https://example.com/page?id=7"_s;
        app.openUrls({url});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.chooser()->isBusy());
        QCOMPARE(app.chooser()->finalUrl(), url);

        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();

        // What is shown is what is launched.
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.chooser()->isBusy());
        QCOMPARE(app.chooser()->finalUrl(), clean);
        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), clean);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first().last(), clean);
    }

    void privacyChangeAfterResolutionRoutesOnceClosed()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        // Matches only once tracking is removed.
        config.rules = {{QString(), true, RuleType::Glob, u"https://example.com/page?id=7"_s, u"b"_s}};
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy finished(&app, &Application::finished);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://example.com/page?utm_source=news&id=7"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.chooser()->isBusy());

        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(app.settingsDialog()->apply());
        QTest::qWait(200);
        QCOMPARE(launched.size(), 0);

        app.settingsDialog()->reject();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QCOMPARE(launched.first().at(1).toString(), u"https://example.com/page?id=7"_s);
    }

    void privacyChangeAppliesToRestartedAndQueuedLinks()
    {
        RedirectServer server;
        server.routes.insert("/two", {200, {}});
        server.stalled = {"/one?utm_source=x", "/one"};
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.privacy.unwrapRedirects = true;
        box.writeConfig(config);
        Application app(box.options());
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({server.url("/one?utm_source=x"), server.url("/two?utm_source=y")});
        QTRY_VERIFY(server.requested("/one?utm_source=x"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());

        app.chooser()->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(server.requested("/one"));
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && app.chooser()->isBusy());

        // Cancelling the restarted link moves on; the queued one uses the new settings.
        QTest::keyClick(app.chooser(), Qt::Key_Escape);
        QTRY_VERIFY(server.requested("/two"));
        QVERIFY(!server.requested("/two?utm_source=y"));
        server.release("/one?utm_source=x");
        server.release("/one");
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.chooser()->isBusy());
        QCOMPARE(app.chooser()->finalUrl(), server.url("/two"));
        QTest::qWait(100);
        QCOMPARE(launched.size(), 0);
        QCOMPARE(app.chooser()->finalUrl(), server.url("/two"));

        QTest::keyClick(chooserList(app), Qt::Key_1);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(1).toString(), server.url("/two"));
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first().last(), server.url("/two"));
    }
};

QTEST_MAIN(TestApplication)

#include "test_application.moc"
