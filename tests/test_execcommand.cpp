#include "core/execcommand.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace kbs;

namespace
{
QString makeExecutable(const QTemporaryDir &dir, const QString &name)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

SelfIdentity testSelf(const QString &path = {})
{
    return {u"kde-browser-switcher.desktop"_s, u"kde-browser-switcher"_s, path.isEmpty() ? QStringList() : QStringList{path}};
}
} // namespace

class TestExecCommand : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parseExec_data()
    {
        QTest::addColumn<QString>("exec");
        QTest::addColumn<QString>("program");
        QTest::addColumn<QStringList>("arguments");

        QTest::newRow("simple") << u"firefox %u"_s << u"firefox"_s << QStringList{u"{url}"_s};
        QTest::newRow("multi url code") << u"/usr/bin/chromium --incognito %U"_s << u"/usr/bin/chromium"_s
                                        << QStringList{u"--incognito"_s, u"{url}"_s};
        QTest::newRow("file codes") << u"browser %f %F"_s << u"browser"_s << QStringList{u"{url}"_s, u"{url}"_s};
        QTest::newRow("no field code") << u"browser --new-window"_s << u"browser"_s << QStringList{u"--new-window"_s};
        QTest::newRow("quoted program") << u"\"/opt/My Browser/browser\" --flag %u"_s << u"/opt/My Browser/browser"_s
                                        << QStringList{u"--flag"_s, u"{url}"_s};
        QTest::newRow("quoted escapes") << u"browser \"--name=a \\\"q\\\" \\\\ \\$HOME \\`x\\`\""_s << u"browser"_s
                                        << QStringList{u"--name=a \"q\" \\ $HOME `x`"_s};
        QTest::newRow("empty quoted argument kept") << u"browser \"\" %u"_s << u"browser"_s << QStringList{QString(), u"{url}"_s};
        QTest::newRow("single quotes") << u"sh -c 'exec browser \"$1\"' sh %u"_s << u"sh"_s
                                       << QStringList{u"-c"_s, u"exec browser \"$1\""_s, u"sh"_s, u"{url}"_s};
        QTest::newRow("tabs and repeated spaces") << u"browser\t  --a   %u  "_s << u"browser"_s << QStringList{u"--a"_s, u"{url}"_s};
        QTest::newRow("embedded code") << u"browser --url=%u"_s << u"browser"_s << QStringList{u"--url={url}"_s};
        QTest::newRow("percent escape") << u"browser --zoom=100%%"_s << u"browser"_s << QStringList{u"--zoom=100%"_s};
        QTest::newRow("icon name path") << u"browser %i --title=%c %k"_s << u"browser"_s
                                        << QStringList{u"--icon"_s, u"web-icon"_s, u"--title=Web Browser"_s, u"/apps/web.desktop"_s};
        QTest::newRow("deprecated codes dropped") << u"browser %d %D %n %N %v %m %u"_s << u"browser"_s << QStringList{u"{url}"_s};
        QTest::newRow("unknown code dropped") << u"browser %z %u"_s << u"browser"_s << QStringList{u"{url}"_s};
        QTest::newRow("env wrapper") << u"env MOZ_ENABLE_WAYLAND=1 firefox --new-window %u"_s << u"env"_s
                                     << QStringList{u"MOZ_ENABLE_WAYLAND=1"_s, u"firefox"_s, u"--new-window"_s, u"{url}"_s};
        QTest::newRow("flatpak wrapper")
            << u"/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=firefox --file-forwarding org.mozilla.firefox @@u %u @@"_s
            << u"/usr/bin/flatpak"_s
            << QStringList{u"run"_s,
                           u"--branch=stable"_s,
                           u"--arch=x86_64"_s,
                           u"--command=firefox"_s,
                           u"--file-forwarding"_s,
                           u"org.mozilla.firefox"_s,
                           u"@@u"_s,
                           u"{url}"_s,
                           u"@@"_s};
    }

    void parseExec()
    {
        QFETCH(QString, exec);
        QFETCH(QString, program);
        QFETCH(QStringList, arguments);
        const DesktopExecContext context{u"Web Browser"_s, u"web-icon"_s, u"/apps/web.desktop"_s};
        const auto result = parseDesktopExec(exec, context);
        QVERIFY2(result.command.has_value(), qPrintable(result.error));
        QCOMPARE(result.command->program, program);
        QCOMPARE(result.command->arguments, arguments);
    }

    void iconCodeWithoutIconIsDropped()
    {
        const auto result = parseDesktopExec(u"browser %i %u"_s, {u"Name"_s, QString(), QString()});
        QVERIFY(result.command.has_value());
        QCOMPARE(result.command->arguments, QStringList{u"{url}"_s});
    }

    void parseExecErrors_data()
    {
        QTest::addColumn<QString>("exec");
        QTest::newRow("empty") << QString();
        QTest::newRow("blank") << u"  \t "_s;
        QTest::newRow("unterminated double quote") << u"browser \"--a %u"_s;
        QTest::newRow("unterminated single quote") << u"browser '--a %u"_s;
        QTest::newRow("program is only a field code") << u"%u"_s;
        QTest::newRow("program is only deprecated codes") << u"%d %u"_s;
    }

    void parseExecErrors()
    {
        QFETCH(QString, exec);
        const auto result = parseDesktopExec(exec);
        QVERIFY(!result.command.has_value());
        QVERIFY(!result.error.isEmpty());
    }

    void urlIsOneLiteralArgument()
    {
        const QString url = u"https://example.com/a b?x=$(touch /tmp/pwned)&y=`id`;z|w'q\"r\\s*{url}#%2F%00"_s;
        const CommandTemplate command{u"browser"_s, {u"--new-window"_s}};
        const auto launch = buildLaunchCommand(command, url);
        QVERIFY(launch.has_value());
        QCOMPARE(launch->program, u"browser"_s);
        QCOMPARE(launch->arguments, (QStringList{u"--new-window"_s, url}));
    }

    void placeholderIsSubstituted()
    {
        const QString url = u"https://example.com/?q=1&r={url}"_s;
        const CommandTemplate command{u"env"_s, {u"A=1"_s, u"browser"_s, u"--app={url}"_s, u"@@u"_s, u"{url}"_s, u"@@"_s}};
        const auto launch = buildLaunchCommand(command, url);
        QVERIFY(launch.has_value());
        QCOMPARE(launch->program, u"env"_s);
        QCOMPARE(launch->arguments,
                 (QStringList{u"A=1"_s, u"browser"_s, u"--app=https://example.com/?q=1&r={url}"_s, u"@@u"_s, url, u"@@"_s}));
    }

    void unsafeUrlsAreRefused_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("empty") << QString();
        QTest::newRow("option") << u"--renderer-cmd-prefix=/bin/sh"_s;
        QTest::newRow("short option") << u"-P"_s;
    }

    void unsafeUrlsAreRefused()
    {
        QFETCH(QString, url);
        QVERIFY(!buildLaunchCommand({u"browser"_s, {u"{url}"_s}}, url).has_value());
    }

    void argumentLines()
    {
        QCOMPARE(parseArgumentLines(u"--profile\r\n  Work Profile \n\n{url}\n"_s),
                 (QStringList{u"--profile"_s, u"  Work Profile "_s, u"{url}"_s}));
        QCOMPARE(parseArgumentLines(QString()), QStringList());
        const QStringList arguments{u"-P"_s, u"name with spaces"_s, u"--x=\"quoted\""_s, u"{url}"_s};
        QCOMPARE(formatArgumentLines(arguments), u"-P\nname with spaces\n--x=\"quoted\"\n{url}"_s);
        QCOMPARE(parseArgumentLines(formatArgumentLines(arguments)), arguments);
    }

    void effectiveProgramLooksThroughEnv_data()
    {
        QTest::addColumn<QString>("program");
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QString>("effective");
        QTest::newRow("direct") << u"/usr/bin/firefox"_s << QStringList{u"{url}"_s} << u"/usr/bin/firefox"_s;
        QTest::newRow("env assignments") << u"env"_s << QStringList{u"A=1"_s, u"B=2"_s, u"firefox"_s, u"{url}"_s} << u"firefox"_s;
        QTest::newRow("env options") << u"/usr/bin/env"_s
                                     << QStringList{u"-i"_s, u"-u"_s, u"HOME"_s, u"--unset=X"_s, u"-C"_s, u"/tmp"_s, u"--"_s, u"chromium"_s}
                                     << u"chromium"_s;
        QTest::newRow("nested env") << u"env"_s << QStringList{u"A=1"_s, u"env"_s, u"B=2"_s, u"brave"_s} << u"brave"_s;
        QTest::newRow("split string unknown") << u"env"_s << QStringList{u"-S"_s, u"firefox -P x"_s} << QString();
        QTest::newRow("env without program") << u"env"_s << QStringList{u"A=1"_s} << QString();
        QTest::newRow("split string long") << u"env"_s << QStringList{u"--split-string=firefox"_s} << QString();
        QTest::newRow("split string in cluster") << u"env"_s << QStringList{u"-iS"_s, u"firefox"_s} << QString();
        QTest::newRow("unknown long option") << u"env"_s << QStringList{u"--frobnicate"_s, u"x"_s, u"firefox"_s} << QString();
        QTest::newRow("unknown short option") << u"env"_s << QStringList{u"-a"_s, u"x"_s, u"firefox"_s} << QString();
        QTest::newRow("ignore environment dash") << u"env"_s << QStringList{u"-"_s, u"--ignore-environment"_s, u"--chdir=/"_s, u"firefox"_s}
                                                 << u"firefox"_s;
    }

    void effectiveProgramLooksThroughEnv()
    {
        QFETCH(QString, program);
        QFETCH(QStringList, arguments);
        QFETCH(QString, effective);
        QCOMPARE(effectiveProgram({program, arguments}), effective);
    }

    void selfIsRecognised()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString selfPath = makeExecutable(dir, u"switcher-binary"_s);
        QVERIFY(QFile::link(selfPath, dir.filePath(u"link-to-self"_s)));
        const SelfIdentity self = testSelf(selfPath);

        QVERIFY(isSelfCommand({u"kde-browser-switcher"_s, {}}, self));
        QVERIFY(isSelfCommand({u"/usr/bin/kde-browser-switcher"_s, {u"{url}"_s}}, self));
        QVERIFY(isSelfCommand({selfPath, {}}, self));
        QVERIFY(isSelfCommand({dir.filePath(u"link-to-self"_s), {}}, self));
        QVERIFY(isSelfCommand({u"env"_s, {u"A=1"_s, u"kde-browser-switcher"_s, u"{url}"_s}}, self));
        QVERIFY(!isSelfCommand({u"firefox"_s, {}}, self));
        QVERIFY(!isSelfCommand({u"kde-browser-switcher-helper"_s, {}}, self));

        // A wrapper that hides the program it runs might be ourselves.
        QVERIFY(isSelfCommand({u"env"_s, {u"-S"_s, u"kde-browser-switcher"_s, u"{url}"_s}}, self));
        QVERIFY(isSelfCommand({u"env"_s, {u"-S"_s, u"firefox"_s}}, self));
        QVERIFY(!validateCommand({u"env"_s, {u"-S"_s, u"kde-browser-switcher"_s, u"{url}"_s}}, self).ok());

        const auto validation = validateCommand({u"env"_s, {u"kde-browser-switcher"_s}}, self);
        QVERIFY(!validation.ok());
    }

    void validation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString browser = makeExecutable(dir, u"fake-browser"_s);
        const SelfIdentity self = testSelf();

        auto result = validateCommand({browser, {u"{url}"_s}}, self);
        QVERIFY(result.ok());
        QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join(u'\n')));

        result = validateCommand({QString(), {}}, self);
        QVERIFY(!result.ok());

        result = validateCommand({u"{url}"_s, {}}, self);
        QVERIFY(!result.ok());

        result = validateCommand({u"kbs-test-definitely-missing-binary"_s, {}}, self);
        QVERIFY(result.ok());
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(result.warnings.first().contains(u"kbs-test-definitely-missing-binary"_s));

        result = validateCommand({dir.filePath(u"missing"_s), {}}, self);
        QVERIFY(result.ok());
        QCOMPARE(result.warnings.size(), 1);

        // A file that exists but is not executable.
        QFile plain(dir.filePath(u"plain"_s));
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.close();
        result = validateCommand({plain.fileName(), {}}, self);
        QCOMPARE(result.warnings.size(), 1);

        result = validateCommand({u"env"_s, {u"A=1"_s, u"kbs-test-definitely-missing-binary"_s}}, self);
        QVERIFY(result.ok());
        QCOMPARE(result.warnings.size(), 1);
        QVERIFY(result.warnings.first().contains(u"kbs-test-definitely-missing-binary"_s));

        // Fail closed: what an uninspectable wrapper runs cannot be checked.
        result = validateCommand({u"env"_s, {u"-S"_s, u"firefox"_s}}, self);
        QVERIFY(!result.ok());
        QCOMPARE(result.errors.size(), 1);
    }

    void shellInterpolationIsRejected_data()
    {
        QTest::addColumn<QString>("program");
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<bool>("ok");
        QTest::newRow("url inside script") << u"sh"_s << QStringList{u"-c"_s, u"firefox {url}"_s} << false;
        QTest::newRow("url is the script") << u"/bin/bash"_s << QStringList{u"-c"_s, u"{url}"_s} << false;
        QTest::newRow("through env") << u"env"_s << QStringList{u"A=1"_s, u"zsh"_s, u"-c"_s, u"x {url}"_s} << false;
        // Every command string is rejected, even ones that only use "$1".
        QTest::newRow("positional parameter of command string") << u"sh"_s
                                                                << QStringList{u"-c"_s, u"exec firefox \"$1\""_s, u"sh"_s, u"{url}"_s}
                                                                << false;
        QTest::newRow("appended url is the script") << u"/bin/sh"_s << QStringList{u"-c"_s} << false;
        QTest::newRow("appended after script") << u"sh"_s << QStringList{u"-c"_s, u"exec firefox"_s} << false;
        QTest::newRow("end of options before url") << u"sh"_s << QStringList{u"-c"_s, u"--"_s, u"{url}"_s} << false;
        QTest::newRow("option between") << u"bash"_s << QStringList{u"-c"_s, u"-e"_s, u"{url}"_s} << false;
        QTest::newRow("option cluster") << u"bash"_s << QStringList{u"-ec"_s, u"{url}"_s} << false;
        QTest::newRow("plus option") << u"bash"_s << QStringList{u"+e"_s, u"-c"_s, u"{url}"_s} << false;
        QTest::newRow("fish init command") << u"fish"_s << QStringList{u"-C"_s, u"{url}"_s} << false;
        QTest::newRow("fish long command") << u"fish"_s << QStringList{u"--command"_s, u"{url}"_s} << false;
        QTest::newRow("busybox shell") << u"busybox"_s << QStringList{u"sh"_s, u"-c"_s} << false;
        QTest::newRow("env split string to shell") << u"env"_s << QStringList{u"-S"_s, u"sh -c"_s, u"{url}"_s} << false;
        QTest::newRow("env split string appended") << u"env"_s << QStringList{u"-S"_s, u"sh -c"_s} << false;
        QTest::newRow("url as env program") << u"env"_s << QStringList{u"A=1"_s, u"{url}"_s} << false;
        QTest::newRow("url appended as env program") << u"env"_s << QStringList{u"A=1"_s} << false;
        QTest::newRow("shell script gets url as parameter") << u"sh"_s << QStringList{u"/opt/run-browser.sh"_s, u"{url}"_s} << true;
        QTest::newRow("shell script gets appended url") << u"/bin/bash"_s << QStringList{u"/opt/run-browser.sh"_s} << true;
        QTest::newRow("non-shell embedding is fine") << u"browser"_s << QStringList{u"--app={url}"_s} << true;
        QTest::newRow("non-shell -c is fine") << u"browser"_s << QStringList{u"-c"_s, u"{url}"_s} << true;
        QTest::newRow("env wrapper") << u"/usr/bin/env"_s << QStringList{u"-i"_s, u"-u"_s, u"X"_s, u"A=1"_s, u"firefox"_s, u"{url}"_s} << true;
        QTest::newRow("flatpak wrapper")
            << u"/usr/bin/flatpak"_s
            << QStringList{u"run"_s, u"--command=firefox"_s, u"--file-forwarding"_s, u"org.mozilla.firefox"_s, u"@@u"_s, u"{url}"_s, u"@@"_s}
            << true;
    }

    void shellInterpolationIsRejected()
    {
        QFETCH(QString, program);
        QFETCH(QStringList, arguments);
        QFETCH(bool, ok);
        QCOMPARE(validateCommand({program, arguments}, testSelf()).ok(), ok);
    }
};

QTEST_GUILESS_MAIN(TestExecCommand)
#include "test_execcommand.moc"
