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
#include <QScrollBar>
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
using klinker::Application;
using klinker::BrowserSetting;
using klinker::ChooserWindow;
using klinker::CommandLine;
using klinker::Config;
using klinker::Rule;
using klinker::RuleType;

namespace
{

const QString Fixture = QStringLiteral(KLINKER_CAPTURE_FIXTURE);
const QString AppBinary = QStringLiteral(KLINKER_APP_EXECUTABLE);

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
    QString configPath = dir.path() + u"/config/klinker/config.json"_s;
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
        writeBytes(klinker::configToJson(config));
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
void reloadFromDisk(klinker::SettingsDialog *dialog)
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

void hideFirstBrowser(klinker::SettingsDialog *dialog)
{
    auto *table = dialog->findChild<QTreeWidget *>(u"browserTable"_s);
    table->setCurrentItem(table->topLevelItem(0));
    dialog->findChild<QPushButton *>(u"hideBrowserButton"_s)->click();
}

QCheckBox *settingsCheck(Application &app, const QString &name)
{
    return app.settingsDialog() ? app.settingsDialog()->findChild<QCheckBox *>(name) : nullptr;
}

// The default configuration file in the test's private $XDG_CONFIG_HOME and,
// next to it, the file of an unrelated application; removed again afterwards.
struct DefaultLocations {
    QString current = klinker::ConfigStore::defaultPath();
    QString sibling = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/other-app/config.json"_s;

    DefaultLocations()
    {
        QVERIFY(current.startsWith(QDir::tempPath()));
        QVERIFY(!QFile::exists(current) && !QFile::exists(sibling));
    }

    ~DefaultLocations()
    {
        QDir(QFileInfo(current).absolutePath()).removeRecursively();
        QDir(QFileInfo(sibling).absolutePath()).removeRecursively();
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
    return klinker::configFromJson(file.readAll()).config.value_or(Config());
}

} // namespace

class TestApplication : public QObject
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
        QVERIFY(QFileInfo(Fixture).isExecutable());
    }

    // Command line

    void commandLineModes()
    {
        CommandLine none = klinker::parseCommandLine({u"klinker"_s});
        QCOMPARE(none.action, CommandLine::Action::Run);
        QVERIFY(none.settings);
        QVERIFY(none.urls.isEmpty());

        CommandLine settings = klinker::parseCommandLine({u"klinker"_s, u"--settings"_s});
        QVERIFY(settings.settings);

        CommandLine links = klinker::parseCommandLine(
            {u"klinker"_s, u"--choose"_s, u"--config"_s, u"/tmp/x.json"_s, u"https://a.example/"_s, u"http://b.example/x"_s});
        QCOMPARE(links.action, CommandLine::Action::Run);
        QVERIFY(!links.settings);
        QVERIFY(links.choose);
        QCOMPARE(links.configPath, u"/tmp/x.json"_s);
        QCOMPARE(links.urls, (QStringList{u"https://a.example/"_s, u"http://b.example/x"_s}));

        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"--help"_s}).action, CommandLine::Action::ShowHelp);
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"-h"_s}).action, CommandLine::Action::ShowHelp);
        // Only documented options exist; Qt's --help-all must not fall through to opening settings.
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"--help-all"_s}).action, CommandLine::Action::Error);
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"--version"_s}).action, CommandLine::Action::ShowVersion);
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"--bogus"_s}).action, CommandLine::Action::Error);

        CommandLine list = klinker::parseCommandLine({u"klinker"_s, u"--config"_s, u"/tmp/x.json"_s, u"--list-browsers"_s});
        QCOMPARE(list.action, CommandLine::Action::ListBrowsers);
        QCOMPARE(list.configPath, u"/tmp/x.json"_s);
        for (const QString &other : {u"https://a.example/"_s, u"--settings"_s, u"--choose"_s}) {
            QCOMPARE(klinker::parseCommandLine({u"klinker"_s, u"--list-browsers"_s, other}).action, CommandLine::Action::Error);
        }
    }

    void browserListingShowsCommandsAndProblems()
    {
        QTemporaryDir dir;
        const QString data = dir.filePath(u"data"_s);
        klinker::test::writeBrowserDesktopFile(data, u"capture.desktop"_s, u"Capture"_s, Fixture + u" --out /nonexistent %u"_s);
        klinker::test::writeBrowserDesktopFile(data, u"gone.desktop"_s, u"Gone"_s, u"klinker-test-no-such-browser %u"_s);
        const QString config = dir.filePath(u"config.json"_s);
        QFile file(config);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version": 1, "browsers": [{"id": "gone.desktop", "hidden": true}, {"id": "manual-1", "manual": true,
            "name": "Self", "program": "klinker", "arguments": ["{url}"]}]})");
        file.close();

        const QStringList lines = klinker::browserListing(config, QStringList{data}).split(u'\n', Qt::SkipEmptyParts);
        QCOMPARE(lines, (QStringList{
                            u"gone.desktop\tGone\tklinker-test-no-such-browser {url}\t“klinker-test-no-such-browser” was not found in PATH."_s,
                            u"manual-1\tSelf\tklinker {url}\tThis command would open Klinker itself."_s,
                            u"capture.desktop\tCapture\t%1 --out /nonexistent {url}\tok"_s.arg(Fixture),
                        }));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{");
        file.close();
        QVERIFY(klinker::browserListing(config, QStringList{data}).startsWith(u"error: %1: "_s.arg(config)));
    }

    void linksArePreservedExactly()
    {
        const QString odd = u"https://Example.COM:8443/a%2Fb/%7Euser/?q=a+b&q=a%20b&&utm_source=x#frag?x=1"_s;
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, odd}).urls, QStringList{odd});
    }

    void rawArgumentsKeepEveryByte()
    {
        QCOMPARE(klinker::decodeArgument(QByteArray("https://e.example/caf\xc3\xa9")), u"https://e.example/café"_s);
        // Invalid UTF-8 would otherwise turn into U+FFFD replacement characters.
        QCOMPARE(klinker::decodeArgument(QByteArray("https://e.example/\xff\xfe?a=\xe9&b=%41")), u"https://e.example/%FF%FE?a=%E9&b=%41"_s);
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
        QVERIFY(!klinker::urlArgumentError(url).isEmpty());
        const CommandLine parsed = klinker::parseCommandLine({u"klinker"_s, u"https://ok.example/"_s, url});
        QCOMPARE(parsed.action, CommandLine::Action::Error);
        QVERIFY(!parsed.error.isEmpty());
    }

    // KIO expands %U with QUrl::PrettyDecoded, which turns %20 back into spaces.
    void prettyDecodedSpacesAreReEncoded_data()
    {
        QTest::addColumn<QString>("argument");
        QTest::addColumn<QString>("expected");
        QTest::newRow("oauth scopes") << u"https://login.example/oauth2/v2.0/authorize?scope=a%2Fb offline_access openid profile&state=s1"_s
                                      << u"https://login.example/oauth2/v2.0/authorize?scope=a%2Fb%20offline_access%20openid%20profile&state=s1"_s;
        QTest::newRow("path, plus, fragment") << u"http://e.example/a b/c%2Bd+e?q=1%252 2&r=%26#f g"_s
                                              << u"http://e.example/a%20b/c%2Bd+e?q=1%252%202&r=%26#f%20g"_s;
        QTest::newRow("missing file") << u"file:///nonexistent dir/page one.html"_s << u"file:///nonexistent%20dir/page%20one.html"_s;
        QTest::newRow("already encoded") << u"https://e.example/a%20b?x=%2F%2B%25+"_s << u"https://e.example/a%20b?x=%2F%2B%25+"_s;
    }

    void prettyDecodedSpacesAreReEncoded()
    {
        QFETCH(QString, argument);
        QFETCH(QString, expected);
        const CommandLine parsed = klinker::parseCommandLine({u"klinker"_s, argument});
        QCOMPARE(parsed.action, CommandLine::Action::Run);
        QCOMPARE(parsed.urls, QStringList{expected});
        QCOMPARE(klinker::parseCommandLine({u"klinker"_s, expected}).urls, QStringList{expected});
    }

    void controlCharactersAreStillRejected_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("tab") << u"https://e.example/a\tb"_s;
        QTest::newRow("newline") << u"https://e.example/a b\nc"_s;
        QTest::newRow("carriage return") << u"https://e.example/?a=1\r"_s;
        QTest::newRow("nul") << u"https://e.example/a"_s + QChar(0) + u"b"_s;
        QTest::newRow("no-break space") << u"https://e.example/a\u00a0b"_s;
        QTest::newRow("bidi override") << u"https://e.example/a\u202eb"_s;
        QTest::newRow("leading space") << u" https://e.example/"_s;
        QTest::newRow("space in host") << u"https://e .example/"_s;
        QTest::newRow("space in scheme") << u"java script:alert(1)"_s;
        QTest::newRow("other scheme") << u"javascript:alert('a b')"_s;
    }

    void controlCharactersAreStillRejected()
    {
        QFETCH(QString, url);
        const CommandLine parsed = klinker::parseCommandLine({u"klinker"_s, url});
        QCOMPARE(parsed.action, CommandLine::Action::Error);
        QVERIFY(parsed.urls.isEmpty());
    }

    void localFilesBecomeFileUrls()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + u"/page with space.html"_s;
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const CommandLine parsed = klinker::parseCommandLine({u"klinker"_s, path});
        QCOMPARE(parsed.action, CommandLine::Action::Run);
        QCOMPARE(parsed.urls, QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)});
        QVERIFY(klinker::urlArgumentError(u"file:///etc/hosts"_s).isEmpty());
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
        QCOMPARE(QString::fromUtf8(version.readAllStandardOutput()), u"klinker %1\n"_s.arg(QStringLiteral(KLINKER_VERSION)));

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

    // As KIO starts the handler: one argv element with literal spaces.
    void binaryLaunchesAPrettyDecodedLinkEncoded()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        config.rules = {{QString(), true, RuleType::Domain, u"login.example"_s, u"a"_s}};
        box.writeConfig(config);

        QProcess app;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        env.insert(u"HOME"_s, box.dir.path());
        env.insert(u"XDG_DATA_DIRS"_s, box.dataDir);
        env.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"unix:path=%1/no-bus"_s.arg(box.dir.path()));
        app.setProcessEnvironment(env);
        app.start(AppBinary, {u"--config"_s, box.configPath, u"https://login.example/authorize?scope=a%2Fb offline_access openid&state=s1"_s});
        QVERIFY(app.waitForFinished(15000));
        QVERIFY2(app.exitCode() == 0, app.readAllStandardError().constData());
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(),
                 (QStringList{u"--browser=a"_s, u"https://login.example/authorize?scope=a%2Fb%20offline_access%20openid&state=s1"_s}));
    }

    void desktopFileUsesTheKlinkerIdentity()
    {
        QFile file(QStringLiteral(KLINKER_SOURCE_DIR "/data/io.github.manicminer.Klinker.desktop"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        const QStringList lines = text.split(u'\n');
        for (const QString &line : {u"Name=Klinker"_s, u"Exec=klinker %U"_s, u"TryExec=klinker"_s, u"Icon=io.github.manicminer.Klinker"_s,
                                    u"Name=Configure Klinker"_s, u"Exec=klinker --settings"_s}) {
            QVERIFY2(lines.contains(line), qPrintable(line));
        }
        QVERIFY(QFile::exists(QStringLiteral(KLINKER_SOURCE_DIR "/data/icons/io.github.manicminer.Klinker.svg")));
        QCOMPARE(klinker::AppDisplayName.toString(), u"Klinker"_s);
        QCOMPARE(klinker::AppId.toString(), u"io.github.manicminer.Klinker"_s);
    }

    // Launching

    void launchPassesTheUrlAsOneArgument()
    {
        Sandbox box;
        klinker::Browser browser;
        browser.id = u"cap"_s;
        browser.name = u"Capture"_s;
        browser.command = {Fixture, {u"--out"_s, box.captureDir, u"--flag"_s, u"{url}"_s}};
        const QString url = u"https://example.com/a;b|c&d$(touch x)`id`?q='\"*#frag"_s;
        const klinker::LaunchResult result = klinker::launchBrowser(browser, url, klinker::appSelfIdentity());
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.pid > 0);
        QTRY_COMPARE(box.captures().size(), 1);
        QCOMPARE(box.captures().first(), (QStringList{u"--flag"_s, url}));
        QVERIFY(!QFile::exists(u"x"_s));
    }

    void launchErrorsAreReported()
    {
        klinker::Browser missing;
        missing.name = u"Missing"_s;
        missing.command = {u"/nonexistent/browser"_s, {u"{url}"_s}};
        klinker::LaunchResult result = klinker::launchBrowser(missing, u"https://example.com/"_s, klinker::appSelfIdentity());
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(u"Missing"_s));

        for (const QString &name : {u"klinker"_s, u"/usr/bin/klinker"_s, AppBinary}) {
            klinker::Browser self;
            self.name = u"Loop"_s;
            self.command = {name, {u"{url}"_s}};
            result = klinker::launchBrowser(self, u"https://example.com/"_s, klinker::appSelfIdentity());
            QVERIFY2(!result.ok, qPrintable(name));
        }

        klinker::Browser ok;
        ok.name = u"Fixture"_s;
        ok.command = {Fixture, {u"--out"_s, u"/nonexistent"_s, u"{url}"_s}};
        result = klinker::launchBrowser(ok, u"-oops"_s, klinker::appSelfIdentity());
        QVERIFY(!result.ok);
    }

    void appIdentityUsesTheInstalledDesktopId()
    {
        const klinker::SelfIdentity self = klinker::appSelfIdentity();
        QCOMPARE(self.desktopIds.first(), u"io.github.manicminer.Klinker.desktop"_s);
        QCOMPARE(self.desktopIds, (QStringList{u"io.github.manicminer.Klinker.desktop"_s, u"klinker.desktop"_s}));
        QCOMPARE(self.executableNames, (QStringList{u"klinker"_s, u"io.github.manicminer.Klinker"_s}));
        QVERIFY(self.executablePaths.contains(QCoreApplication::applicationFilePath()));
    }

    // Configuration location

    void defaultLocationIsReadWhenPresent()
    {
        Sandbox box;
        DefaultLocations files;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.rules = {{QString(), true, RuleType::Domain, u"routed.example"_s, u"b"_s}};
        const QByteArray bytes = klinker::configToJson(config);
        DefaultLocations::write(files.current, bytes);

        Application::Options options = box.options();
        options.configPath.clear();
        Application app(options);
        QCOMPARE(app.config(), config);
        QSignalSpy launched(&app, &Application::launched);
        app.openUrls({u"https://routed.example/"_s});
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
        QCOMPARE(DefaultLocations::read(files.current), bytes);
    }

    void missingDefaultGivesDefaultsAndIgnoresOtherFiles()
    {
        Sandbox box;
        DefaultLocations files;
        Config other;
        other.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        other.rules = {{QString(), true, RuleType::Domain, u"routed.example"_s, u"a"_s}};
        const QByteArray sibling = klinker::configToJson(other);
        DefaultLocations::write(files.sibling, sibling);

        Application::Options options = box.options();
        options.configPath.clear();
        Application app(options);
        QCOMPARE(app.config(), Config());

        // No rule applies, so nothing is launched: the chooser asks.
        QSignalSpy launched(&app, &Application::launched);
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://routed.example/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QTest::keyClick(chooserList(app), Qt::Key_Escape);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(launched.size(), 0);
        QVERIFY(box.captures().isEmpty());
        QVERIFY(!QFileInfo::exists(QFileInfo(files.current).absolutePath()));

        // Saving writes the default location only.
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!app.settingsDialog());
        Config expected;
        expected.privacy.stripTrackingParameters = true;
        QCOMPARE(configOnDisk(files.current), expected);
        QCOMPARE(DefaultLocations::read(files.sibling), sibling);
        QCOMPARE(app.config(), expected);
    }

    void explicitConfigIgnoresTheDefaultLocation()
    {
        Sandbox box;
        DefaultLocations files;
        Config atDefault;
        atDefault.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        const QByteArray bytes = klinker::configToJson(atDefault);
        DefaultLocations::write(files.current, bytes);

        Application app(box.options());
        QCOMPARE(app.config(), Config());
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        QVERIFY(app.settingsDialog()->apply());
        QVERIFY(QFile::exists(box.configPath));
        QCOMPARE(DefaultLocations::read(files.current), bytes);
        app.settingsDialog()->reject();
    }

    // Preprocessing

    void pipelineWithoutPrivacyKeepsTheLink()
    {
        klinker::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &klinker::LinkPipeline::finished);
        const QString url = u"https://example.com/?utm_source=x&id=1#f"_s;
        pipeline.start(url, {});
        QCOMPARE(finished.size(), 0); // never synchronous
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<klinker::ProcessedLink>();
        QCOMPARE(link.originalUrl, url);
        QCOMPARE(link.finalUrl, url);
        QVERIFY(!link.warning);
        QVERIFY(!link.status.isEmpty());
    }

    void pipelineStripsTrackingWithoutNetwork()
    {
        klinker::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &klinker::LinkPipeline::finished);
        QVERIFY(!klinker::LinkPipeline::needsNetwork(u"https://e.example/"_s, {true, false}));
        pipeline.start(u"https://e.example/p?utm_source=x&id=1&fbclid=y#f"_s, {true, false});
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<klinker::ProcessedLink>();
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
        klinker::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &klinker::LinkPipeline::finished);
        const QString start = server.url("/start?utm_source=a");
        QVERIFY(klinker::LinkPipeline::needsNetwork(start, {true, true}));
        pipeline.start(start, {true, true});
        QVERIFY(pipeline.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto link = finished.first().at(0).value<klinker::ProcessedLink>();
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
        klinker::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &klinker::LinkPipeline::finished);
        pipeline.start(server.url("/slow"), {false, true});
        QTRY_VERIFY(!server.requests.isEmpty());
        pipeline.skip();
        QTRY_COMPARE(finished.size(), 1);
        const auto link = finished.first().at(0).value<klinker::ProcessedLink>();
        QCOMPARE(link.finalUrl, server.url("/slow"));
        QVERIFY(link.skipped);
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
    }

    void pipelineReportsFailuresWithASafeLink()
    {
        RedirectServer server;
        server.routes.insert("/bad", {302, "javascript:alert(1)"});
        klinker::LinkPipeline pipeline;
        QSignalSpy finished(&pipeline, &klinker::LinkPipeline::finished);
        pipeline.start(server.url("/bad"), {false, true});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto link = finished.first().at(0).value<klinker::ProcessedLink>();
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
        klinker::test::writeBrowserDesktopFile(box.dataDir, u"detected.desktop"_s, u"Detected"_s, Fixture + u" --out "_s + box.captureDir + u" --browser=detected %u"_s);
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
        klinker::test::saveScreenshot(app.chooser(), u"app-launch-error-light"_s);

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
        klinker::test::saveScreenshot(app.chooser(), u"app-busy-light"_s);
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
        QCOMPARE(app.config().privacy, (klinker::PrivacySettings{true, false}));

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
        QCOMPARE(app.config().privacy, (klinker::PrivacySettings{true, false}));

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
        QCOMPARE(app.config().privacy, (klinker::PrivacySettings{false, true}));
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"unwrapRedirectsCheck"_s)->setChecked(false);
        QVERIFY(app.settingsDialog()->apply());
        QCOMPARE(configOnDisk(box.configPath).privacy, klinker::PrivacySettings());
    }

    // Nothing is left to apply after Reload from Disk, but OK still takes the
    // reloaded file over.
    void reloadThenOkAdoptsTheFile()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s)};
        box.writeConfig(config);
        Application app(box.options());
        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        {
            Application other(box.options());
            other.openSettings();
            QTRY_VERIFY(other.settingsDialog());
            settingsCheck(other, u"unwrapRedirectsCheck"_s)->setChecked(true);
            QVERIFY(other.settingsDialog()->apply());
            other.settingsDialog()->reject();
        }

        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(!app.settingsDialog()->apply());
        reloadFromDisk(app.settingsDialog());
        auto *buttons = app.settingsDialog()->findChild<QDialogButtonBox *>();
        QVERIFY(!buttons->button(QDialogButtonBox::Apply)->isEnabled());
        buttons->button(QDialogButtonBox::Ok)->click();
        QTRY_VERIFY(!app.settingsDialog());
        QCOMPARE(app.config().privacy, (klinker::PrivacySettings{false, true}));

        app.openSettings();
        QTRY_VERIFY(app.settingsDialog());
        settingsCheck(app, u"stripTrackingCheck"_s)->setChecked(true);
        QVERIFY(app.settingsDialog()->apply());
        QCOMPARE(configOnDisk(box.configPath).privacy, (klinker::PrivacySettings{true, true}));
    }

    // Chooser preferences

    void chooserUsesTheConfiguredPreferences()
    {
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s)};
        config.chooser = {klinker::ChooserStyle::Horizontal, 64, false, false};
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QCOMPARE(app.chooser()->preferences(), config.chooser);
        QCOMPARE(chooserList(app)->flow(), QListView::LeftToRight);
        // Placed at its preferred size for the screen (the offscreen screen has
        // no name, so placement does not move it; see test_placement).
        const QSize maximum = klinker::maximumSize(app.chooser()->screen()->availableGeometry(), 8);
        QCOMPARE(app.chooser()->size(), app.chooser()->preferredSize(maximum));
        QVERIFY(app.chooser()->width() > app.chooser()->preferredSize(maximum).height());
        QSignalSpy launched(&app, &Application::launched);
        QTest::keyClick(chooserList(app), Qt::Key_2);
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first().at(0).toString(), u"b"_s);
    }

    void horizontalChooserFitsItsBrowsers_data()
    {
        QTest::addColumn<QString>("style");
        for (const QString &style : {u"Breeze"_s, u"Oxygen"_s, u"Fusion"_s}) {
            QTest::newRow(qPrintable(style)) << style;
        }
    }

    // As reported: six named browsers in a horizontal chooser with 48 px icons
    // on a screen wide enough for them. The window is as wide as the row, and
    // nothing scrolls or is cut off, whatever the widget style.
    void horizontalChooserFitsItsBrowsers()
    {
        QFETCH(QString, style);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style, true);
        Sandbox box;
        Config config;
        for (const QString &name : {u"ChatGPT"_s, u"Chromium"_s, u"Falkon"_s, u"Firefox"_s, u"LibreWolf"_s, u"Switchyard"_s}) {
            config.browsers.append(box.capture(name.toLower(), name));
        }
        config.chooser = {klinker::ChooserStyle::Horizontal, 48, true, true};
        box.writeConfig(config);
        Application app(box.options(true));
        QSignalSpy finished(&app, &Application::finished);
        app.openUrls({u"https://www.kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        ChooserWindow *chooser = app.chooser();
        QListView *list = chooserList(app);
        klinker::test::saveScreenshot(chooser, u"controller-horizontal-reported-six-%1"_s.arg(style.toLower()));
        const QSize maximum = klinker::maximumSize(chooser->screen()->availableGeometry(), 8);
        QCOMPARE(chooser->size(), chooser->preferredSize(maximum));
        QVERIFY(chooser->width() < maximum.width());
        const QString problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        const QRect tile = list->visualRect(list->model()->index(0, 0));
        QCOMPARE(list->width(), 6 * tile.width() + 2 * list->frameWidth());
        QCOMPARE(list->viewport()->size(), QSize(6 * tile.width(), tile.height()));
        QVERIFY(tile.height() >= 48 + list->fontMetrics().height());

        QTest::keyClick(list, Qt::Key_Escape);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(box.captures().isEmpty());
    }

    void savedAppearanceAppliesWhenTheChooserReturns_data()
    {
        QTest::addColumn<QString>("widgetStyle");
        for (const QString &style : {u"Breeze"_s, u"Oxygen"_s}) {
            QTest::newRow(qPrintable(style)) << style;
        }
    }

    void savedAppearanceAppliesWhenTheChooserReturns()
    {
        QFETCH(QString, widgetStyle);
        if (!klinker::test::hasStyle(widgetStyle)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(widgetStyle);
        Sandbox box;
        Config config;
        config.browsers = {box.capture(u"a"_s, u"Alpha"_s), box.capture(u"b"_s, u"Beta"_s), box.capture(u"c"_s, u"Gamma"_s)};
        config.chooser.style = klinker::ChooserStyle::Vertical; // a saved choice is kept
        box.writeConfig(config);
        Application app(box.options());
        app.openUrls({u"https://kde.org/"_s});
        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible());
        QPointer<ChooserWindow> chooser = app.chooser();
        QScreen *screen = chooser->screen();
        QCOMPARE(chooser->width(), ChooserWindow::CardWidth);
        QCOMPARE(chooserList(app)->flow(), QListView::TopToBottom);

        chooser->findChild<QPushButton *>(u"settingsButton"_s)->click();
        QTRY_VERIFY(app.settingsDialog() && app.settingsDialog()->isVisible());
        auto *style = app.settingsDialog()->findChild<QComboBox *>(u"chooserStyleCombo"_s);
        style->setCurrentIndex(style->findData(u"horizontal"_s));
        app.settingsDialog()->findChild<QSpinBox *>(u"iconSizeSpin"_s)->setValue(96);
        app.settingsDialog()->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();

        QTRY_VERIFY(app.chooser() && app.chooser()->isVisible() && !app.settingsDialog());
        QCOMPARE(app.chooser(), chooser.data()); // the same link, the same window
        QCOMPARE(chooser->preferences(), (klinker::ChooserSettings{klinker::ChooserStyle::Horizontal, 96, true, false}));
        QCOMPARE(chooserList(app)->flow(), QListView::LeftToRight);
        // Refitted on the screen it was on.
        QCOMPARE(chooser->screen(), screen);
        QCOMPARE(chooser->size(), chooser->preferredSize(klinker::maximumSize(screen->availableGeometry(), 8)));
        QVERIFY(screen->availableGeometry().adjusted(8, 8, -8, -8).contains(chooser->geometry()));
        const QString problem = klinker::test::horizontalStripProblem(chooserList(app));
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QVERIFY(!chooserList(app)->horizontalScrollBar()->isVisible());
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
        klinker::test::focusOtherApplication();
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
        klinker::test::focusOtherApplication();
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

        klinker::test::focusOtherApplication();
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

        klinker::test::focusOtherApplication();
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

        klinker::test::focusOtherApplication();
        QVERIFY(chooser->isFocusLossPending());
        server.release("/slow");
        // Ready well within the delay: held back, the chooser still busy.
        QTRY_VERIFY_WITH_TIMEOUT(app.isRoutingDeferred(), ChooserWindow::FocusLossDelayMs);
        QVERIFY(chooser->isFocusLossPending() && chooser->isBusy());
        QCOMPARE(launched.size(), 0);
        QVERIFY(chooser && chooser->isVisible());

        klinker::test::focusWindow(chooser);
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

        klinker::test::focusOtherApplication();
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

        klinker::test::focusOtherApplication();
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
        klinker::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        klinker::test::focusWindow(app.settingsDialog());
        QTRY_VERIFY(app.settingsDialog()->isActiveWindow());
        app.settingsDialog()->reject();

        QTRY_VERIFY(chooser && chooser->isVisible() && chooser->isActiveWindow());
        QCOMPARE(finished.size(), 0);
        QVERIFY(chooser->preferences().closeOnFocusLoss);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 200);
        QVERIFY(chooser && chooser->isVisible());

        klinker::test::focusOtherApplication();
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
