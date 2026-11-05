#include "hostsystem.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>

#include <optional>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

// Prints two lines per argument: its absolute path if it is an executable file,
// found in PATH for a bare name, otherwise nothing; then that path with every
// symlink resolved on the host (empty if it cannot be resolved), so an alias
// is known by what it runs. The names are positional parameters, never part of
// the script.
constexpr auto ProbeScript = R"(for p do
r=
case $p in
*/*) [ -f "$p" ] && [ -x "$p" ] && r=$p ;;
*) r=$(command -v "$p") || r=; case $r in /*) ;; *) r= ;; esac ;;
esac
c=
[ -n "$r" ] && c=$(readlink -f "$r" 2>/dev/null)
printf '%s\n%s\n' "$r" "$c"
done)";

constexpr int ProbeTimeoutMs = 5000;

HostSystem &currentSystem()
{
    static HostSystem system = HostSystem::detect();
    return system;
}

struct HostExecutable {
    QString path;
    QString canonical;
};

QHash<QString, HostExecutable> &executableCache()
{
    static QHash<QString, HostExecutable> cache;
    return cache;
}

std::optional<bool> &reachableCache()
{
    static std::optional<bool> reachable;
    return reachable;
}

// Names the probe cannot carry (one per line) or that would be options.
bool probeable(const QString &program)
{
    return !program.isEmpty() && !program.startsWith(u'-') && !program.contains(u'\n');
}

QString hostDataHome()
{
    const QString host = qEnvironmentVariable("HOST_XDG_DATA_HOME");
    return QDir::isAbsolutePath(host) ? QDir::cleanPath(host) : QDir::homePath() + u"/.local/share"_s;
}

const QStringList &hostOsPrefixes()
{
    static const QStringList prefixes = {u"/usr"_s, u"/bin"_s, u"/sbin"_s, u"/lib"_s, u"/lib64"_s};
    return prefixes;
}

bool hasPrefix(const QString &path, const QString &prefix)
{
    return path == prefix || path.startsWith(prefix + u'/');
}

} // namespace

HostSystem HostSystem::detect()
{
    HostSystem system;
    system.appId = qEnvironmentVariable("FLATPAK_ID");
    system.sandboxed = !system.appId.isEmpty() && QFile::exists(u"/.flatpak-info"_s);
    return system;
}

const HostSystem &HostSystem::current()
{
    return currentSystem();
}

void HostSystem::setCurrent(const HostSystem &system)
{
    currentSystem() = system;
    executableCache().clear();
    reachableCache().reset();
}

QStringList hostDataDirs()
{
    const QString dataHome = hostDataHome();
    // As the host's XDG_DATA_DIRS usually orders them with Flatpak installed.
    return {dataHome,
            dataHome + u"/flatpak/exports/share"_s,
            u"/var/lib/flatpak/exports/share"_s,
            sandboxPath(u"/usr/local/share"_s),
            sandboxPath(u"/usr/share"_s),
            u"/var/lib/snapd/desktop"_s};
}

QString sandboxPath(const QString &hostPath)
{
    const HostSystem &system = HostSystem::current();
    if (!system.sandboxed || !QDir::isAbsolutePath(hostPath)) {
        return hostPath;
    }
    for (const QString &prefix : hostOsPrefixes()) {
        if (hasPrefix(hostPath, prefix)) {
            return system.root + hostPath;
        }
    }
    return hostPath;
}

QString hostPath(const QString &sandboxPath)
{
    const HostSystem &system = HostSystem::current();
    if (!system.sandboxed) {
        return sandboxPath;
    }
    for (const QString &prefix : hostOsPrefixes()) {
        if (hasPrefix(sandboxPath, system.root + prefix)) {
            return sandboxPath.sliced(system.root.size());
        }
    }
    return sandboxPath;
}

void refreshHostExecutables(const QStringList &programs)
{
    QStringList names;
    for (const QString &program : programs) {
        if (!probeable(program)) {
            executableCache().insert(program, {});
        } else if (!names.contains(program)) {
            names.append(program);
        }
    }
    if (names.isEmpty() && !programs.isEmpty()) {
        return;
    }

    const HostSystem &system = HostSystem::current();
    QProcess probe;
    probe.setProgram(system.spawn.value(0));
    probe.setArguments(system.spawn.mid(1)
                       + QStringList{u"--directory=%1"_s.arg(QDir::homePath()), u"--"_s, u"sh"_s, u"-c"_s, QString::fromLatin1(ProbeScript), u"sh"_s}
                       + names);
    probe.setStandardInputFile(QProcess::nullDevice());
    probe.start();
    const bool finished = probe.waitForFinished(ProbeTimeoutMs);
    QStringList lines = QString::fromLocal8Bit(probe.readAllStandardOutput()).split(u'\n');
    lines.removeLast(); // after the final newline
    const bool ok = finished && probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0 && lines.size() == 2 * names.size();
    if (!finished) {
        probe.kill();
        probe.waitForFinished();
    }
    reachableCache() = ok;
    if (!ok) {
        qWarning().noquote() << "Cannot look up programs on the host with" << system.spawn.join(u' ') << ':'
                             << QString::fromLocal8Bit(probe.readAllStandardError()).trimmed();
    }
    for (qsizetype i = 0; i < names.size(); ++i) {
        HostExecutable executable;
        if (ok && !lines.at(2 * i).isEmpty()) {
            executable.path = lines.at(2 * i);
            const QString &canonical = lines.at(2 * i + 1);
            executable.canonical = QDir::isAbsolutePath(canonical) ? canonical : executable.path;
        }
        executableCache().insert(names.at(i), executable);
    }
}

QString hostExecutable(const QString &program)
{
    if (!executableCache().contains(program)) {
        refreshHostExecutables({program});
    }
    return executableCache().value(program).path;
}

QString hostCanonicalExecutable(const QString &program)
{
    if (!executableCache().contains(program)) {
        refreshHostExecutables({program});
    }
    return executableCache().value(program).canonical;
}

bool hostReachable()
{
    if (!reachableCache()) {
        refreshHostExecutables({});
    }
    return *reachableCache();
}

QString hostIconFile(const QString &name)
{
    if (name.isEmpty() || name.contains(u'/')) {
        return {};
    }
    static const QStringList sizes = {u"scalable"_s, u"256x256"_s, u"128x128"_s, u"96x96"_s, u"64x64"_s, u"48x48"_s, u"32x32"_s};
    for (const QString &dataDir : hostDataDirs()) {
        QStringList candidates;
        for (const QString &size : sizes) {
            candidates << u"%1/icons/hicolor/%2/apps/%3"_s.arg(dataDir, size, name);
        }
        candidates << u"%1/pixmaps/%2"_s.arg(dataDir, name);
        for (const QString &base : std::as_const(candidates)) {
            for (const auto extension : {".svg"_L1, ".png"_L1, ".xpm"_L1}) {
                if (QFile::exists(base + extension)) {
                    return base + extension;
                }
            }
        }
    }
    return {};
}

} // namespace klinker
