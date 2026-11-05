#pragma once

#include <QString>
#include <QStringList>

namespace klinker
{

// Where browsers live. Natively that is this system. Inside a Flatpak sandbox
// it is the host: host programs are looked up with `flatpak-spawn --host` and
// started with its D-Bus method, HostCommand (both the org.freedesktop.Flatpak
// permission), so PATH, TryExec and executable checks see the host and never
// the runtime, and host files are read through the read-only views the
// manifest grants.
struct HostSystem {
    bool sandboxed = false;
    QString appId; // the sandbox's own application ID
    QString root = QStringLiteral("/run/host"); // where host-os shows the host's /usr
    QStringList spawn = {QStringLiteral("flatpak-spawn"), QStringLiteral("--host")};
    // Where HostCommand is called: empty for the session bus, otherwise the
    // address of a peer that implements it (tests).
    QString hostCommandBus;

    // Sandboxed when FLATPAK_ID is set and /.flatpak-info exists.
    static HostSystem detect();
    static const HostSystem &current();
    // For tests; also forgets every host lookup.
    static void setCurrent(const HostSystem &system);
};

// The host's data directories that hold desktop entries, in precedence order,
// as the sandbox sees them.
QStringList hostDataDirs();

// A host path as the sandbox sees it (/usr/... is under HostSystem::root), and
// back. Other paths are the same on both sides.
QString sandboxPath(const QString &hostPath);
QString hostPath(const QString &sandboxPath);

// Looks the programs up on the host in one go, replacing earlier answers.
void refreshHostExecutables(const QStringList &programs);
// Absolute host path of a program name or path; empty when the host has no such
// executable or cannot be asked. Answers are kept for the process lifetime.
QString hostExecutable(const QString &program);
// The same with every symlink resolved on the host, e.g. Klinker's exported
// wrapper for an alias of it in ~/bin, which the sandbox may not see.
QString hostCanonicalExecutable(const QString &program);
// Whether programs can be run on the host at all.
bool hostReachable();

// An image file for an icon name from the host's hicolor themes or pixmaps, for
// icons the sandbox's icon theme lookup cannot see; empty if there is none.
QString hostIconFile(const QString &name);

} // namespace klinker
