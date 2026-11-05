// Klinker inside a Flatpak sandbox, simulated: HostSystem points at stand-ins
// for `flatpak-spawn --host` and for HostCommand (a private D-Bus peer, not the
// session bus) that run commands here with a private "host" PATH. Nothing
// reaches the real host, its applications or a real browser.

#include "app/launcher.h"
#include "core/browserdiscovery.h"
#include "core/hostsystem.h"
#include "ui/documentportal.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusServer>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <optional>

using namespace Qt::StringLiterals;
using namespace klinker;

namespace
{

bool writeFile(const QString &path, const QByteArray &content, bool executable = false)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        return false;
    }
    file.close();
    return !executable || file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
}

QByteArray browserEntry(const QByteArray &name, const QByteArray &exec, const QByteArray &extra = {})
{
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + exec
        + "\nMimeType=x-scheme-handler/http;x-scheme-handler/https;\n" + extra;
}

using HostFds = QMap<uint, QDBusUnixFileDescriptor>;
using HostEnv = QMap<QString, QString>;

// org.freedesktop.Flatpak.Development.HostCommand as flatpak's session helper
// implements it, on the private host: it answers with the PID once the program
// has been executed and with an error if it cannot be, and reads bytestrings
// without their terminating NUL as empty.
class FakeHostCommand : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Flatpak.Development")

public:
    struct Call {
        QString cwd;
        QStringList argv;
        QList<uint> fds; // the valid ones
        HostEnv envs;
        uint flags = 0;
    };

    explicit FakeHostCommand(const QString &hostBin)
        : m_hostBin(hostBin)
    {
    }

    QList<Call> takeCalls()
    {
        const QMutexLocker locker(&m_mutex);
        return std::exchange(m_calls, {});
    }

public Q_SLOTS:
    uint HostCommand(const QByteArray &cwd, const QByteArrayList &argv, const HostFds &fds, const HostEnv &envs, uint flags)
    {
        const auto text = [](const QByteArray &bytes) {
            return bytes.endsWith('\0') ? QFile::decodeName(bytes.chopped(1)) : QString();
        };
        Call call{text(cwd), {}, {}, envs, flags};
        for (const QByteArray &argument : argv) {
            call.argv.append(text(argument));
        }
        for (auto it = fds.cbegin(); it != fds.cend(); ++it) {
            if (it.value().isValid()) {
                call.fds.append(it.key());
            }
        }
        {
            const QMutexLocker locker(&m_mutex);
            m_calls.append(call);
        }
        if (call.argv.isEmpty() || call.argv.constFirst().isEmpty()) {
            sendErrorReply(QDBusError::InvalidArgs, u"No command given"_s);
            return 0;
        }
        const QString &name = call.argv.constFirst();
        const QString program = name.contains(u'/') ? name : QStandardPaths::findExecutable(name, {m_hostBin});
        qint64 pid = 0;
        if (program.isEmpty() || !QProcess::startDetached(program, call.argv.mid(1), call.cwd, &pid)) {
            sendErrorReply(QDBusError::Failed, u"Failed to start command: Failed to execute child process “%1”"_s.arg(name));
            return 0;
        }
        return uint(pid);
    }

private:
    QString m_hostBin;
    QMutex m_mutex;
    QList<Call> m_calls;
};

} // namespace

class TestSandbox : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_hostBin; // the host's PATH
    QString m_spawn;
    QString m_spawnLog; // one byte per host command
    QString m_captures;
    QThread m_hostThread; // serves HostCommand while a test waits for its answer
    FakeHostCommand *m_hostCommand = nullptr;
    QDBusServer *m_hostServer = nullptr;
    QString m_hostAddress;

    QString path(const QString &relative) const
    {
        return m_dir.filePath(relative);
    }

    // The name Klinker opens HostSystem::hostCommandBus under (launcher.cpp).
    QString hostConnectionName() const
    {
        return u"klinker-host:"_s + m_hostAddress;
    }

    HostSystem sandbox(const QString &spawn = QString()) const
    {
        HostSystem system;
        system.sandboxed = true;
        system.appId = u"io.github.manicminer.Klinker"_s;
        system.root = path(u"run-host"_s);
        system.spawn = {spawn.isEmpty() ? m_spawn : spawn, u"--host"_s};
        system.hostCommandBus = m_hostAddress;
        return system;
    }

    int hostCommands() const
    {
        QFile log(m_spawnLog);
        return log.open(QIODevice::ReadOnly) ? int(log.size()) : 0;
    }

    QStringList waitForCapture()
    {
        QStringList files;
        const bool found = QTest::qWaitFor([&] {
            files = QDir(m_captures).entryList({u"*.json"_s}, QDir::Files);
            return !files.isEmpty();
        });
        if (!found) {
            return {};
        }
        QFile file(QDir(m_captures).filePath(files.constFirst()));
        QStringList arguments;
        if (!file.open(QIODevice::ReadOnly)) {
            return arguments;
        }
        for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
            arguments.append(value.toString());
        }
        file.remove();
        return arguments;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_hostBin = path(u"host-bin"_s);
        m_spawnLog = path(u"spawn.log"_s);
        m_captures = path(u"captures"_s);
        QVERIFY(QDir().mkpath(m_captures));
        const QString sh = QStandardPaths::findExecutable(u"sh"_s);
        QVERIFY(!sh.isEmpty());
        QVERIFY(QDir().mkpath(m_hostBin));
        QVERIFY(QFile::link(sh, m_hostBin + u"/sh"_s));
        // For the probe, which resolves symlinks on the host.
        const QString readlink = QStandardPaths::findExecutable(u"readlink"_s);
        QVERIFY(!readlink.isEmpty());
        QVERIFY(QFile::link(readlink, m_hostBin + u"/readlink"_s));
        QVERIFY(writeFile(m_hostBin + u"/host-browser"_s, "#!/bin/sh\nexit 0\n", true));

        // Accepts exactly the arguments Klinker gives flatpak-spawn.
        m_spawn = path(u"flatpak-spawn"_s);
        QVERIFY(writeFile(m_spawn,
                          "#!/bin/sh\n"
                          "printf x >> '" + QFile::encodeName(m_spawnLog) + "'\n"
                          "[ \"$1\" = --host ] || exit 9; shift\n"
                          "case $1 in --directory=*) cd \"${1#--directory=}\" || exit 8; shift ;; *) exit 7 ;; esac\n"
                          "[ \"$1\" = -- ] || exit 6; shift\n"
                          "PATH='" + QFile::encodeName(m_hostBin) + "' exec \"$@\"\n",
                          true));
        QVERIFY(writeFile(path(u"failing-spawn"_s), "#!/bin/sh\necho 'org.freedesktop.Flatpak is not allowed' >&2\nexit 1\n", true));

        qDBusRegisterMetaType<QByteArrayList>();
        qDBusRegisterMetaType<HostFds>();
        qDBusRegisterMetaType<HostEnv>();
        m_hostThread.start();
        m_hostCommand = new FakeHostCommand(m_hostBin);
        m_hostCommand->moveToThread(&m_hostThread);
        QMetaObject::invokeMethod(
            m_hostCommand,
            [this] {
                m_hostServer = new QDBusServer(u"unix:tmpdir=%1"_s.arg(QDir::tempPath()));
                connect(m_hostServer, &QDBusServer::newConnection, m_hostCommand, [this](const QDBusConnection &connection) {
                    QDBusConnection(connection).registerObject(u"/org/freedesktop/Flatpak/Development"_s, m_hostCommand, QDBusConnection::ExportAllSlots);
                });
                m_hostAddress = m_hostServer->address();
            },
            Qt::BlockingQueuedConnection);
        QVERIFY2(m_hostServer->isConnected(), qPrintable(m_hostServer->lastError().message()));

        // Klinker reuses one named peer connection, which connectToPeer()
        // returns before authentication has finished: a call carrying file
        // descriptors sent before then fails as "Not connected to D-Bus server"
        // (seen in flatpak-builder). The session bus Klinker uses in a real
        // sandbox is authenticated before it is handed out, so open the peer
        // connection here and wait until it can pass file descriptors.
        const QDBusConnection client = QDBusConnection::connectToPeer(m_hostAddress, hostConnectionName());
        QVERIFY2(client.isConnected(), qPrintable(client.lastError().message()));
        QVERIFY(QTest::qWaitFor([&] {
            return client.isConnected() && client.connectionCapabilities().testFlag(QDBusConnection::UnixFileDescriptorPassing);
        }));
    }

    void init()
    {
        HostSystem::setCurrent(sandbox());
        QFile::remove(m_spawnLog);
        m_hostCommand->takeCalls();
    }

    void cleanupTestCase()
    {
        HostSystem::setCurrent(HostSystem());
        QDBusConnection::disconnectFromPeer(hostConnectionName());
        QMetaObject::invokeMethod(
            m_hostCommand,
            [this] {
                delete m_hostServer;
                delete m_hostCommand;
            },
            Qt::BlockingQueuedConnection);
        m_hostThread.quit();
        m_hostThread.wait();
    }

    void detectionNeedsFlatpakId()
    {
        // The test environment unsets FLATPAK_ID, also inside a Flatpak build.
        QVERIFY(!HostSystem::detect().sandboxed);
    }

    void programsAreLookedUpOnTheHost()
    {
        QCOMPARE(resolveExecutable(u"host-browser"_s), m_hostBin + u"/host-browser"_s);
        QCOMPARE(resolveExecutable(m_hostBin + u"/host-browser"_s), m_hostBin + u"/host-browser"_s);
        // Present in the sandbox, not on the host.
        QVERIFY(!QStandardPaths::findExecutable(u"ls"_s).isEmpty());
        QVERIFY(resolveExecutable(u"ls"_s).isEmpty());
        QVERIFY(resolveExecutable(m_spawnLog).isEmpty()); // a file, not executable
        QVERIFY(resolveExecutable(u"sh"_s).endsWith(u"/sh"_s));
        // Answers are kept for the process.
        const int before = hostCommands();
        QVERIFY(!resolveExecutable(u"host-browser"_s).isEmpty());
        QVERIFY(resolveExecutable(u"ls"_s).isEmpty());
        QCOMPARE(hostCommands(), before);
        QVERIFY(hostReachable());
    }

    void namesAreNeverCode()
    {
        const QString marker = path(u"pwned"_s);
        const QStringList hostile = {u"$(touch %1)"_s.arg(marker), u"`touch %1`"_s.arg(marker), u"x; touch %1"_s.arg(marker),
                                     u"'\"; touch %1"_s.arg(marker)};
        refreshHostExecutables(hostile);
        for (const QString &program : hostile) {
            QVERIFY2(resolveExecutable(program).isEmpty(), qPrintable(program));
        }
        QVERIFY(!QFile::exists(marker));
        // Not asked at all: options and names that would break the one-per-line answer.
        QFile::remove(m_spawnLog);
        QVERIFY(resolveExecutable(u"-host-browser"_s).isEmpty());
        QVERIFY(resolveExecutable(u"host-browser\nsh"_s).isEmpty());
        QCOMPARE(hostCommands(), 0);
    }

    void unreachableHostFailsClosed()
    {
        for (const QString &spawn : {path(u"failing-spawn"_s), path(u"no-such-spawn"_s)}) {
            HostSystem::setCurrent(sandbox(spawn));
            QVERIFY(resolveExecutable(u"host-browser"_s).isEmpty());
            QVERIFY(!hostReachable());
            Browser browser{u"b"_s, BrowserSource::Manual, u"Host Browser"_s, {}, {u"host-browser"_s, {}}};
            const LaunchResult result = launchBrowser(browser, u"https://example.org/"_s, SelfIdentity::current());
            QVERIFY(!result.ok);
            QVERIFY2(result.error.contains(u"org.freedesktop.Flatpak"_s), qPrintable(result.error));
        }
        QCOMPARE(m_hostCommand->takeCalls().size(), 0);
    }

    void discoveryChecksTryExecOnTheHostInOneQuery()
    {
        const QString data = path(u"run-host/usr/share"_s);
        QVERIFY(writeFile(data + u"/applications/host.desktop"_s, browserEntry("Host", "host-browser %u", "TryExec=host-browser\n")));
        QVERIFY(writeFile(data + u"/applications/sandbox-only.desktop"_s, browserEntry("Sandbox Only", "ls %u", "TryExec=ls\n")));
        QVERIFY(writeFile(data + u"/applications/missing.desktop"_s, browserEntry("Missing", "not-installed %u")));
        QVERIFY(writeFile(data + u"/applications/keyfile.desktop"_s, browserEntry("Key File", "host-browser --desktop-file %k %u")));
        QVERIFY(writeFile(data + u"/applications/self.desktop"_s,
                          browserEntry("Klinker Flatpak", "/usr/bin/flatpak run --branch=stable --command=klinker io.github.manicminer.Klinker @@u %U @@")));
        QVERIFY(writeFile(data + u"/applications/io.github.manicminer.Klinker.desktop"_s, browserEntry("Klinker", "klinker %U")));

        const DiscoveryResult result = discoverBrowsers({data}, SelfIdentity::current());
        QCOMPARE(hostCommands(), 1);
        QStringList ids;
        for (const DetectedBrowser &browser : result.browsers) {
            ids.append(browser.desktopId);
        }
        // Without TryExec a missing program is still listed, as natively; the
        // settings show it as not found.
        QCOMPARE(ids, (QStringList{u"host.desktop"_s, u"keyfile.desktop"_s, u"missing.desktop"_s}));
        // %k is the host's path of the desktop file.
        QCOMPARE(result.browsers.at(1).command.arguments,
                 (QStringList{u"--desktop-file"_s, u"/usr/share/applications/keyfile.desktop"_s, u"{url}"_s}));
        const CommandValidation missing = validateCommand(result.browsers.at(2).command, SelfIdentity::current());
        QVERIFY(missing.ok());
        QCOMPARE(missing.warnings, QStringList{u"“not-installed” was not found in PATH."_s});
    }

    void hostDirectoriesAreTheHostViews()
    {
        qputenv("HOST_XDG_DATA_HOME", QFile::encodeName(path(u"host-data"_s)));
        const QStringList expected = {path(u"host-data"_s), path(u"host-data/flatpak/exports/share"_s), u"/var/lib/flatpak/exports/share"_s,
                                      path(u"run-host/usr/local/share"_s), path(u"run-host/usr/share"_s), u"/var/lib/snapd/desktop"_s};
        QCOMPARE(defaultDataDirs(), expected);
        qunsetenv("HOST_XDG_DATA_HOME");
        QCOMPARE(defaultDataDirs().constFirst(), QDir::homePath() + u"/.local/share"_s);
    }

    void hostPathsMapBothWays()
    {
        const QString root = path(u"run-host"_s);
        QCOMPARE(sandboxPath(u"/usr/share/pixmaps/b.png"_s), root + u"/usr/share/pixmaps/b.png"_s);
        QCOMPARE(sandboxPath(u"/bin/b"_s), root + u"/bin/b"_s);
        QCOMPARE(sandboxPath(u"/usrlocal/b"_s), u"/usrlocal/b"_s);
        QCOMPARE(sandboxPath(u"/opt/b/b"_s), u"/opt/b/b"_s);
        QCOMPARE(sandboxPath(u"/home/u/b.png"_s), u"/home/u/b.png"_s);
        QCOMPARE(hostPath(root + u"/usr/bin/b"_s), u"/usr/bin/b"_s);
        QCOMPARE(hostPath(u"/home/u/b"_s), u"/home/u/b"_s);
        QCOMPARE(hostFilePath(root + u"/usr/bin/b"_s), u"/usr/bin/b"_s);
        QCOMPARE(hostFileUrl(u"file://"_s + root + u"/usr/share/doc/a%20b.html#x"_s), u"file:///usr/share/doc/a%20b.html#x"_s);
        QCOMPARE(hostFileUrl(u"https://example.org/run-host/usr"_s), u"https://example.org/run-host/usr"_s);

        HostSystem::setCurrent(HostSystem());
        QCOMPARE(sandboxPath(u"/usr/bin/b"_s), u"/usr/bin/b"_s);
        QCOMPARE(hostPath(root + u"/usr/bin/b"_s), root + u"/usr/bin/b"_s);
    }

    void hostIconsAreFoundAsFiles()
    {
        qputenv("HOST_XDG_DATA_HOME", QFile::encodeName(path(u"host-data"_s)));
        const QString flatpakIcon = path(u"host-data/flatpak/exports/share/icons/hicolor/128x128/apps/org.example.Browser.png"_s);
        const QString pixmap = path(u"run-host/usr/share/pixmaps/legacy-browser.xpm"_s);
        QVERIFY(writeFile(flatpakIcon, "png"));
        QVERIFY(writeFile(pixmap, "xpm"));
        QCOMPARE(hostIconFile(u"org.example.Browser"_s), flatpakIcon);
        QCOMPARE(hostIconFile(u"legacy-browser"_s), pixmap);
        QVERIFY(hostIconFile(u"absent"_s).isEmpty());
        QVERIFY(hostIconFile(u"../pixmaps/legacy-browser"_s).isEmpty());
        qunsetenv("HOST_XDG_DATA_HOME");
    }

    void browsersStartOnTheHostWithTheirArguments()
    {
        const QString capture = QStringLiteral(KLINKER_CAPTURE_FIXTURE);
        QVERIFY(QFile::link(capture, m_hostBin + u"/capture-browser"_s));
        const QString url = u"https://example.org/a%20b?q=\"x\";$(id)&r='y'#`z`"_s;
        Browser browser{u"b"_s, BrowserSource::Manual, u"Capture"_s, {}, {u"capture-browser"_s, {u"--out"_s, m_captures, u"--new-tab={url}"_s}}};

        const LaunchResult result = launchBrowser(browser, url, SelfIdentity::current());
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(result.pid > 0);
        const QStringList argv = {u"capture-browser"_s, u"--out"_s, m_captures, u"--new-tab=%1"_s.arg(url)};
        QCOMPARE(result.command.program, argv.constFirst());
        QCOMPARE(result.command.arguments, argv.mid(1));
        const QList<FakeHostCommand::Call> calls = m_hostCommand->takeCalls();
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls.constFirst().argv, argv);
        QCOMPARE(calls.constFirst().cwd, QDir::homePath());
        // stdin (/dev/null), stdout and stderr; no environment changes; no
        // flags, so the browser is neither killed with Klinker nor cleared.
        QCOMPARE(calls.constFirst().fds, (QList<uint>{0, 1, 2}));
        QCOMPARE(calls.constFirst().envs, HostEnv());
        QCOMPARE(calls.constFirst().flags, 0u);
        QCOMPARE(waitForCapture(), QStringList{u"--new-tab=%1"_s.arg(url)});
    }

    // The host is reachable, but the browser cannot be started there: a missing
    // program and one whose interpreter does not exist.
    void hostLaunchFailuresAreReported()
    {
        QVERIFY(writeFile(m_hostBin + u"/broken-browser"_s, "#!/nonexistent/interpreter\n", true));
        for (const QString &program : {u"not-installed"_s, u"broken-browser"_s, m_hostBin + u"/broken-browser"_s}) {
            Browser browser{u"b"_s, BrowserSource::Manual, u"Broken"_s, {}, {program, {}}};
            const LaunchResult result = launchBrowser(browser, u"https://example.org/"_s, SelfIdentity::current());
            QVERIFY2(!result.ok, qPrintable(program));
            QVERIFY2(result.error.startsWith(u"Could not start Broken"_s), qPrintable(result.error));
            QVERIFY2(result.error.contains(u"Failed to start command"_s), qPrintable(result.error));
        }
        QCOMPARE(m_hostCommand->takeCalls().size(), 3);
        QVERIFY(hostReachable());
    }

    // Aliases of Klinker that only the host can see: symlinks to the exported
    // Flatpak wrapper (also from the host's PATH) and to a native installation.
    void hostAliasesOfKlinkerAreSelf()
    {
        const QString marker = path(u"klinker-ran"_s);
        const QByteArray script = "#!/bin/sh\ntouch '" + QFile::encodeName(marker) + "'\n";
        const QString wrapper = path(u"host-flatpak/app/io.github.manicminer.Klinker/current/active/export/bin/io.github.manicminer.Klinker"_s);
        const QString exported = path(u"host-flatpak/exports/bin/io.github.manicminer.Klinker"_s);
        const QString native = path(u"host-usr/bin/klinker"_s);
        QVERIFY(writeFile(wrapper, script, true));
        QVERIFY(writeFile(native, script, true));
        QVERIFY(QDir().mkpath(QFileInfo(exported).absolutePath()));
        QVERIFY(QFile::link(wrapper, exported));
        const QString aliases = path(u"host-aliases"_s);
        QVERIFY(QDir().mkpath(aliases));
        QVERIFY(QFile::link(exported, aliases + u"/browser"_s));
        QVERIFY(QFile::link(native, aliases + u"/web"_s));
        QVERIFY(QFile::link(m_hostBin + u"/host-browser"_s, aliases + u"/other"_s));
        QVERIFY(QFile::link(exported, m_hostBin + u"/x-www-browser"_s));

        const QStringList programs = {aliases + u"/browser"_s, aliases + u"/web"_s, aliases + u"/other"_s, u"x-www-browser"_s};
        refreshHostExecutables(programs);
        // Hidden from the sandbox from now on: only the host's answers can tell.
        QVERIFY(QDir(aliases).removeRecursively());
        QVERIFY(QFile::remove(m_hostBin + u"/x-www-browser"_s));

        const SelfIdentity self = SelfIdentity::current();
        const QList<CommandTemplate> refused = {
            {programs.at(0), {}},
            {programs.at(1), {}},
            {programs.at(3), {}},
            {u"env"_s, {u"A=1"_s, programs.at(0)}},
        };
        for (const CommandTemplate &command : refused) {
            const CommandValidation validation = validateCommand(command, self);
            QVERIFY2(validation.errors.contains(u"This command would open Klinker itself."_s), qPrintable(command.program));
            Browser browser{u"b"_s, BrowserSource::Manual, u"Alias"_s, {}, command};
            QVERIFY2(!launchBrowser(browser, u"https://example.org/"_s, self).ok, qPrintable(command.program));
        }
        QVERIFY(validateCommand({programs.at(2), {}}, self).ok());
        QCOMPARE(m_hostCommand->takeCalls().size(), 0);
        QVERIFY(!QFile::exists(marker));
    }

    void selfAndShellCommandsAreStillRefused()
    {
        const SelfIdentity self = SelfIdentity::current();
        const QList<CommandTemplate> refused = {
            {u"flatpak"_s, {u"run"_s, u"io.github.manicminer.Klinker"_s}},
            {u"io.github.manicminer.Klinker"_s, {}},
            {u"klinker"_s, {}},
            {u"sh"_s, {u"-c"_s, u"host-browser {url}"_s}},
        };
        for (const CommandTemplate &command : refused) {
            Browser browser{u"b"_s, BrowserSource::Manual, u"Refused"_s, {}, command};
            const LaunchResult result = launchBrowser(browser, u"https://example.org/"_s, self);
            QVERIFY2(!result.ok, qPrintable(command.program));
        }
        QCOMPARE(QDir(m_captures).entryList(QDir::Files), QStringList());
    }
};

QTEST_GUILESS_MAIN(TestSandbox)
#include "test_sandbox.moc"
