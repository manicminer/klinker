#include "core/browserdiscovery.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace kbs;

namespace
{
const QByteArray s_browserMime = "MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;\n";

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return file.write(content) == content.size();
}

QByteArray entry(const QByteArray &name, const QByteArray &exec, const QByteArray &extra = {}, const QByteArray &mime = s_browserMime)
{
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + exec + "\n" + mime + extra;
}

const DetectedBrowser *findBrowser(const QList<DetectedBrowser> &browsers, const QString &id)
{
    for (const auto &browser : browsers) {
        if (browser.desktopId == id) {
            return &browser;
        }
    }
    return nullptr;
}

SelfIdentity testSelf()
{
    return SelfIdentity::current();
}
} // namespace

class TestBrowserDiscovery : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_root;
    QString m_home, m_local, m_usr, m_flatpak;
    DiscoveryResult m_result;

    QString apps(const QString &dataDir, const QString &file) const
    {
        return dataDir + u"/applications/"_s + file;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CURRENT_DESKTOP", "X-Test:KDE");
        m_home = m_root.filePath(u"home"_s);
        m_local = m_root.filePath(u"usr-local"_s);
        m_usr = m_root.filePath(u"usr"_s);
        m_flatpak = m_root.filePath(u"flatpak"_s);

        const QString tryExecOk = m_root.filePath(u"bin/present-browser"_s);
        QVERIFY(writeFile(tryExecOk, "#!/bin/sh\nexit 0\n"));
        QVERIFY(QFile::setPermissions(tryExecOk, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

        // Plain system browser.
        QVERIFY(writeFile(apps(m_usr, u"firefox.desktop"_s), entry("Firefox", "firefox %u", "Icon=firefox\n")));
        // User override of a system entry wins.
        QVERIFY(writeFile(apps(m_usr, u"chromium.desktop"_s), entry("Chromium", "chromium %U")));
        QVERIFY(writeFile(apps(m_home, u"chromium.desktop"_s), entry("Chromium (user)", "chromium --user %U")));
        // /usr/local beats /usr.
        QVERIFY(writeFile(apps(m_usr, u"vivaldi.desktop"_s), entry("Vivaldi", "vivaldi-usr %U")));
        QVERIFY(writeFile(apps(m_local, u"vivaldi.desktop"_s), entry("Vivaldi", "vivaldi-local %U")));
        // A Hidden user file masks the system entry.
        QVERIFY(writeFile(apps(m_usr, u"brave.desktop"_s), entry("Brave", "brave %u")));
        QVERIFY(writeFile(apps(m_home, u"brave.desktop"_s), "[Desktop Entry]\nHidden=true\n"));
        // A user file that stops being a browser also masks the system one.
        QVERIFY(writeFile(apps(m_usr, u"epiphany.desktop"_s), entry("Web", "epiphany %u")));
        QVERIFY(writeFile(apps(m_home, u"epiphany.desktop"_s), entry("Web", "epiphany %u", {}, "MimeType=text/html;\n")));
        // Filters.
        QVERIFY(writeFile(apps(m_usr, u"nodisplay.desktop"_s), entry("NoDisplay", "nodisplay %u", "NoDisplay=true\n")));
        QVERIFY(writeFile(apps(m_usr, u"gnome-only.desktop"_s), entry("GNOME only", "gnome-only %u", "OnlyShowIn=GNOME;\n")));
        QVERIFY(writeFile(apps(m_usr, u"not-kde.desktop"_s), entry("Not KDE", "not-kde %u", "NotShowIn=KDE;\n")));
        QVERIFY(writeFile(apps(m_usr, u"kde-only.desktop"_s), entry("KDE only", "kde-only %u", "OnlyShowIn=GNOME;KDE;\n")));
        QVERIFY(writeFile(apps(m_usr, u"tryexec-missing.desktop"_s), entry("Missing", "missing %u", "TryExec=/nonexistent/kbs/browser\n")));
        QVERIFY(writeFile(apps(m_usr, u"tryexec-present.desktop"_s),
                          entry("Present", "present-browser %u", "TryExec=" + tryExecOk.toUtf8() + "\n")));
        QVERIFY(writeFile(apps(m_usr, u"editor.desktop"_s), entry("Editor", "editor %F", {}, "MimeType=text/plain;\n")));
        QVERIFY(writeFile(apps(m_usr, u"link.desktop"_s), "[Desktop Entry]\nType=Link\nName=Link\nURL=https://example.com\n" + s_browserMime));
        QVERIFY(writeFile(apps(m_usr, u"https-only.desktop"_s),
                          entry("HTTPS only", "secure %u", {}, "MimeType=x-scheme-handler/https;\n")));
        QVERIFY(writeFile(apps(m_usr, u"no-exec.desktop"_s), "[Desktop Entry]\nType=Application\nName=No Exec\n" + s_browserMime));
        QVERIFY(writeFile(apps(m_usr, u"broken-exec.desktop"_s), entry("Broken", "broken \"--unterminated %u")));
        // Never ourselves.
        QVERIFY(writeFile(apps(m_usr, u"kde-browser-switcher.desktop"_s), entry("Switcher", "/opt/other-name %u")));
        QVERIFY(writeFile(apps(m_usr, u"switcher-copy.desktop"_s), entry("Switcher copy", "kde-browser-switcher %u")));
        QVERIFY(writeFile(apps(m_usr, u"switcher-env.desktop"_s), entry("Switcher env", "env A=1 /usr/bin/kde-browser-switcher %u")));
        QVERIFY(writeFile(apps(m_usr, u"klinker.desktop"_s), entry("Klinker", "/opt/other-name %u")));
        QVERIFY(writeFile(apps(m_usr, u"io.github.manicminer.Klinker.desktop"_s), entry("Klinker", "/opt/other-name %u")));
        QVERIFY(writeFile(apps(m_local, u"io.github.manicminer.KdeBrowserSwitcher.desktop"_s), entry("Browser Switcher", "/opt/other-name %u")));
        QVERIFY(writeFile(apps(m_usr, u"klinker-copy.desktop"_s), entry("Klinker copy", "klinker %u")));
        QVERIFY(writeFile(apps(m_usr, u"klinker-env.desktop"_s), entry("Klinker env", "env A=1 /home/user/.local/bin/klinker %u")));
        QVERIFY(writeFile(apps(m_local, u"switcher-local.desktop"_s), entry("Old switcher", "/home/user/.local/bin/kde-browser-switcher %U")));
        QVERIFY(writeFile(apps(m_usr, u"klinker-helper.desktop"_s), entry("Not Klinker", "klinker-helper %u")));
        // A wrapper that hides the program it runs could be ourselves.
        QVERIFY(writeFile(apps(m_usr, u"switcher-split.desktop"_s), entry("Switcher split", "env -S kde-browser-switcher %u")));
        // Subdirectories form the desktop ID.
        QVERIFY(writeFile(apps(m_usr, u"vendor/web.desktop"_s), entry("Vendor Web", "vendor-web %u")));
        // Exec string escapes and quoting.
        QVERIFY(writeFile(apps(m_usr, u"quoted.desktop"_s),
                          entry("Quoted", R"("/opt/my browser/run" --x="a\\\\b" --name=%c\sx %u)")));
        // Flatpak export directory with a wrapper command.
        QVERIFY(writeFile(apps(m_flatpak, u"org.mozilla.firefox.desktop"_s),
                          entry("Firefox (Flatpak)",
                                "/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=firefox --file-forwarding org.mozilla.firefox @@u %u @@",
                                "Icon=org.mozilla.firefox\n")));
        // Missing name falls back to the ID.
        QVERIFY(writeFile(apps(m_usr, u"nameless.desktop"_s), "[Desktop Entry]\nType=Application\nExec=nameless %u\n" + s_browserMime));

        m_result = discoverBrowsers({m_home, m_root.filePath(u"does-not-exist"_s), m_local, m_usr, m_flatpak}, testSelf());
    }

    void detectsBrowserWithCommand()
    {
        const auto *firefox = findBrowser(m_result.browsers, u"firefox.desktop"_s);
        QVERIFY(firefox);
        QCOMPARE(firefox->name, u"Firefox"_s);
        QCOMPARE(firefox->icon, u"firefox"_s);
        QCOMPARE(firefox->desktopFilePath, apps(m_usr, u"firefox.desktop"_s));
        QCOMPARE(firefox->command, (CommandTemplate{u"firefox"_s, {u"{url}"_s}}));
        QVERIFY(findBrowser(m_result.browsers, u"https-only.desktop"_s));
    }

    void higherPrecedenceDirectoryWins()
    {
        const auto *chromium = findBrowser(m_result.browsers, u"chromium.desktop"_s);
        QVERIFY(chromium);
        QCOMPARE(chromium->name, u"Chromium (user)"_s);
        QCOMPARE(chromium->desktopFilePath, apps(m_home, u"chromium.desktop"_s));
        QCOMPARE(chromium->command.arguments, (QStringList{u"--user"_s, u"{url}"_s}));

        const auto *vivaldi = findBrowser(m_result.browsers, u"vivaldi.desktop"_s);
        QVERIFY(vivaldi);
        QCOMPARE(vivaldi->command.program, u"vivaldi-local"_s);
    }

    void maskedAndFilteredEntriesAreExcluded_data()
    {
        QTest::addColumn<QString>("id");
        QTest::newRow("hidden mask") << u"brave.desktop"_s;
        QTest::newRow("non-browser mask") << u"epiphany.desktop"_s;
        QTest::newRow("NoDisplay") << u"nodisplay.desktop"_s;
        QTest::newRow("OnlyShowIn other desktop") << u"gnome-only.desktop"_s;
        QTest::newRow("NotShowIn current desktop") << u"not-kde.desktop"_s;
        QTest::newRow("TryExec missing") << u"tryexec-missing.desktop"_s;
        QTest::newRow("not a browser") << u"editor.desktop"_s;
        QTest::newRow("not an application") << u"link.desktop"_s;
        QTest::newRow("no Exec") << u"no-exec.desktop"_s;
        QTest::newRow("broken Exec") << u"broken-exec.desktop"_s;
        QTest::newRow("self by desktop ID") << u"kde-browser-switcher.desktop"_s;
        QTest::newRow("self by executable") << u"switcher-copy.desktop"_s;
        QTest::newRow("self behind env") << u"switcher-env.desktop"_s;
        QTest::newRow("Klinker by desktop ID") << u"klinker.desktop"_s;
        QTest::newRow("Klinker by application ID") << u"io.github.manicminer.Klinker.desktop"_s;
        QTest::newRow("Browser Switcher by application ID") << u"io.github.manicminer.KdeBrowserSwitcher.desktop"_s;
        QTest::newRow("Klinker by executable") << u"klinker-copy.desktop"_s;
        QTest::newRow("Klinker behind env") << u"klinker-env.desktop"_s;
        QTest::newRow("Browser Switcher by executable path") << u"switcher-local.desktop"_s;
        QTest::newRow("uninspectable env wrapper") << u"switcher-split.desktop"_s;
    }

    void maskedAndFilteredEntriesAreExcluded()
    {
        QFETCH(QString, id);
        QVERIFY(!findBrowser(m_result.browsers, id));
    }

    void includedEntries()
    {
        QVERIFY(findBrowser(m_result.browsers, u"kde-only.desktop"_s));
        QVERIFY(findBrowser(m_result.browsers, u"klinker-helper.desktop"_s));
        QVERIFY(findBrowser(m_result.browsers, u"tryexec-present.desktop"_s));
    }

    void brokenEntriesAreReported()
    {
        QVERIFY(std::ranges::any_of(m_result.warnings, [](const QString &w) {
            return w.contains(u"broken-exec.desktop"_s);
        }));
        QVERIFY(std::ranges::any_of(m_result.warnings, [](const QString &w) {
            return w.contains(u"switcher-split.desktop"_s);
        }));
    }

    void subdirectoryDesktopId()
    {
        QCOMPARE(desktopIdForPath(u"/x/applications"_s, u"/x/applications/a/b/c.desktop"_s), u"a-b-c.desktop"_s);
        QCOMPARE(desktopIdForPath(u"/x/applications/"_s, u"/x/applications/c.desktop"_s), u"c.desktop"_s);
        const auto *web = findBrowser(m_result.browsers, u"vendor-web.desktop"_s);
        QVERIFY(web);
        QCOMPARE(web->command.program, u"vendor-web"_s);
    }

    void execEscapesAndQuoting()
    {
        const auto *quoted = findBrowser(m_result.browsers, u"quoted.desktop"_s);
        QVERIFY(quoted);
        QCOMPARE(quoted->command.program, u"/opt/my browser/run"_s);
        QCOMPARE(quoted->command.arguments, (QStringList{u"--x=a\\b"_s, u"--name=Quoted"_s, u"x"_s, u"{url}"_s}));
    }

    void flatpakWrapperKept()
    {
        const auto *firefox = findBrowser(m_result.browsers, u"org.mozilla.firefox.desktop"_s);
        QVERIFY(firefox);
        QCOMPARE(firefox->command.program, u"/usr/bin/flatpak"_s);
        QCOMPARE(firefox->command.arguments.last(), u"@@"_s);
        QVERIFY(firefox->command.arguments.contains(u"{url}"_s));
    }

    void missingNameFallsBackToId()
    {
        const auto *nameless = findBrowser(m_result.browsers, u"nameless.desktop"_s);
        QVERIFY(nameless);
        QCOMPARE(nameless->name, u"nameless.desktop"_s);
    }

    void sortedByName()
    {
        QStringList names;
        for (const auto &browser : m_result.browsers) {
            names.append(browser.name);
        }
        QStringList sorted = names;
        std::ranges::sort(sorted, [](const QString &a, const QString &b) {
            return QString::localeAwareCompare(a.toCaseFolded(), b.toCaseFolded()) < 0;
        });
        QCOMPARE(names, sorted);
        QCOMPARE(m_result.browsers.size(), 11);
    }

    void defaultDataDirsFollowXdg()
    {
        const QByteArray oldHome = qgetenv("XDG_DATA_HOME");
        const QByteArray oldDirs = qgetenv("XDG_DATA_DIRS");
        const QString home = QDir::homePath();
        const QString userFlatpak = home + u"/.local/share/flatpak/exports/share"_s;
        const QString systemFlatpak = u"/var/lib/flatpak/exports/share"_s;

        qputenv("XDG_DATA_HOME", "/h");
        qputenv("XDG_DATA_DIRS", "/a:relative:/b/:");
        QCOMPARE(defaultDataDirs(), (QStringList{u"/h"_s, u"/a"_s, u"/b"_s, userFlatpak, systemFlatpak}));

        qputenv("XDG_DATA_DIRS", "/var/lib/flatpak/exports/share/:/usr/share");
        QCOMPARE(defaultDataDirs(), (QStringList{u"/h"_s, systemFlatpak, u"/usr/share"_s, userFlatpak}));

        qunsetenv("XDG_DATA_HOME");
        qunsetenv("XDG_DATA_DIRS");
        QCOMPARE(defaultDataDirs(),
                 (QStringList{home + u"/.local/share"_s, u"/usr/local/share"_s, u"/usr/share"_s, userFlatpak, systemFlatpak}));

        qputenv("XDG_DATA_HOME", "relative/path");
        QCOMPARE(defaultDataDirs().first(), home + u"/.local/share"_s);

        oldHome.isNull() ? qunsetenv("XDG_DATA_HOME") : qputenv("XDG_DATA_HOME", oldHome);
        oldDirs.isNull() ? qunsetenv("XDG_DATA_DIRS") : qputenv("XDG_DATA_DIRS", oldDirs);
    }
};

QTEST_GUILESS_MAIN(TestBrowserDiscovery)
#include "test_browserdiscovery.moc"
