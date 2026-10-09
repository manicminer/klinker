#include "browserdiscovery.h"

#include <KConfigGroup>
#include <KDesktopFile>

#include <QDir>
#include <QDirIterator>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

// Desktop Entry Specification: OnlyShowIn/NotShowIn against $XDG_CURRENT_DESKTOP.
bool isShownInCurrentDesktop(const KConfigGroup &group)
{
    const QStringList desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(u':', Qt::SkipEmptyParts);
    const auto listsCurrent = [&desktops](const QStringList &list) {
        return std::ranges::any_of(desktops, [&list](const QString &desktop) {
            return list.contains(desktop);
        });
    };
    if (group.hasKey("OnlyShowIn") && !listsCurrent(group.readXdgListEntry("OnlyShowIn"))) {
        return false;
    }
    return !listsCurrent(group.readXdgListEntry("NotShowIn"));
}

std::optional<DetectedBrowser> readBrowser(const QString &path, const QString &desktopId, const SelfIdentity &self, QStringList *warnings)
{
    const KDesktopFile file(path);
    const KConfigGroup group = file.desktopGroup();

    if (group.readEntry("Hidden", false) || file.readType() != "Application"_L1 || file.noDisplay() || !isShownInCurrentDesktop(group)) {
        return std::nullopt;
    }
    const QStringList mimeTypes = group.readXdgListEntry("MimeType");
    if (!mimeTypes.contains("x-scheme-handler/http"_L1) && !mimeTypes.contains("x-scheme-handler/https"_L1)) {
        return std::nullopt;
    }
    const QString tryExec = group.readEntry("TryExec");
    if (!tryExec.isEmpty() && resolveExecutable(tryExec).isEmpty()) {
        return std::nullopt;
    }

    DetectedBrowser browser;
    browser.desktopId = desktopId;
    browser.desktopFilePath = path;
    browser.name = file.readName();
    if (browser.name.isEmpty()) {
        browser.name = desktopId;
    }
    browser.icon = file.readIcon();

    const QString exec = group.readEntry("Exec");
    if (exec.isEmpty()) {
        warnings->append(u"%1: no Exec command."_s.arg(path));
        return std::nullopt;
    }
    const ExecParseResult parsed = parseDesktopExec(exec, {browser.name, browser.icon, path});
    if (!parsed.command) {
        warnings->append(u"%1: %2"_s.arg(path, parsed.error));
        return std::nullopt;
    }
    browser.command = *parsed.command;

    if (desktopId == self.desktopId) {
        return std::nullopt;
    }
    if (effectiveProgram(browser.command).isEmpty()) {
        // It could be ourselves, so it is never offered.
        warnings->append(u"%1: cannot tell which program the command runs."_s.arg(path));
        return std::nullopt;
    }
    if (isSelfCommand(browser.command, self)) {
        return std::nullopt;
    }
    return browser;
}

void appendUnique(QStringList *list, const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (!list->contains(clean)) {
        list->append(clean);
    }
}

} // namespace

QStringList defaultDataDirs()
{
    const QString home = QDir::homePath();

    QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (!QDir::isAbsolutePath(dataHome)) {
        dataHome = home + u"/.local/share"_s;
    }

    QStringList systemDirs = qEnvironmentVariable("XDG_DATA_DIRS").split(u':', Qt::SkipEmptyParts);
    if (systemDirs.isEmpty()) {
        systemDirs = {u"/usr/local/share"_s, u"/usr/share"_s};
    }

    QStringList dirs;
    appendUnique(&dirs, dataHome);
    for (const QString &dir : std::as_const(systemDirs)) {
        if (QDir::isAbsolutePath(dir)) {
            appendUnique(&dirs, dir);
        }
    }
    appendUnique(&dirs, home + u"/.local/share/flatpak/exports/share"_s);
    appendUnique(&dirs, u"/var/lib/flatpak/exports/share"_s);
    return dirs;
}

QString desktopIdForPath(const QString &applicationsDir, const QString &filePath)
{
    return QDir(applicationsDir).relativeFilePath(filePath).replace(u'/', u'-');
}

DiscoveryResult discoverBrowsers(const QStringList &dataDirs, const SelfIdentity &self)
{
    DiscoveryResult result;
    QSet<QString> seenIds;

    for (const QString &dataDir : dataDirs) {
        const QString applicationsDir = dataDir + u"/applications"_s;
        QStringList files;
        QDirIterator it(applicationsDir, {u"*.desktop"_s}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(it.next());
        }
        files.sort();

        for (const QString &path : std::as_const(files)) {
            const QString desktopId = desktopIdForPath(applicationsDir, path);
            if (seenIds.contains(desktopId)) {
                continue;
            }
            // The highest-precedence file decides, even when it hides the entry.
            seenIds.insert(desktopId);
            if (auto browser = readBrowser(path, desktopId, self, &result.warnings)) {
                result.browsers.append(*browser);
            }
        }
    }

    std::ranges::sort(result.browsers, [](const DetectedBrowser &a, const DetectedBrowser &b) {
        const int byName = QString::localeAwareCompare(a.name.toCaseFolded(), b.name.toCaseFolded());
        return byName != 0 ? byName < 0 : a.desktopId < b.desktopId;
    });
    return result;
}

} // namespace kbs
