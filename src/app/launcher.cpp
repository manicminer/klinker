#include "launcher.h"

#include "core/hostsystem.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>

#include <fcntl.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

using HostFds = QMap<uint, QDBusUnixFileDescriptor>; // a{uh}
using HostEnv = QMap<QString, QString>; // a{ss}

constexpr int HostCommandTimeoutMs = 25000;

// A D-Bus bytestring (ay) as flatpak reads it: NUL-terminated, or it is empty.
QByteArray bytestring(const QString &text)
{
    return QFile::encodeName(text) + QByteArray(1, '\0');
}

// Starts the command on the host with org.freedesktop.Flatpak.Development.HostCommand,
// which answers with the PID once the host has executed the program, or with an
// error if it could not. Like a native detached start: standard input is
// /dev/null, output goes where Klinker's does, the host session's environment is
// used unchanged, and the browser outlives Klinker (no flags).
bool startOnHost(const LaunchCommand &command, const QString &workingDirectory, qint64 *pid, QString *error)
{
    qDBusRegisterMetaType<QByteArrayList>();
    qDBusRegisterMetaType<HostFds>();
    qDBusRegisterMetaType<HostEnv>();
    const QString &peer = HostSystem::current().hostCommandBus;
    QDBusConnection bus = peer.isEmpty() ? QDBusConnection::sessionBus() : QDBusConnection::connectToPeer(peer, u"klinker-host:"_s + peer);
    if (!bus.isConnected()) {
        *error = bus.lastError().message();
        return false;
    }

    QByteArrayList argv{bytestring(command.program)};
    for (const QString &argument : command.arguments) {
        argv.append(bytestring(argument));
    }
    HostFds fds;
    if (const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC); null >= 0) {
        fds.insert(0, QDBusUnixFileDescriptor(null)); // a duplicate
        ::close(null);
    }
    for (const int fd : {1, 2}) {
        if (QDBusUnixFileDescriptor descriptor(fd); descriptor.isValid()) {
            fds.insert(uint(fd), descriptor);
        }
    }

    QDBusMessage call = QDBusMessage::createMethodCall(u"org.freedesktop.Flatpak"_s, u"/org/freedesktop/Flatpak/Development"_s,
                                                       u"org.freedesktop.Flatpak.Development"_s, u"HostCommand"_s);
    call << bytestring(workingDirectory) << QVariant::fromValue(argv) << QVariant::fromValue(fds) << QVariant::fromValue(HostEnv()) << 0u;
    const QDBusMessage reply = bus.call(call, QDBus::Block, HostCommandTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        *error = reply.errorMessage().isEmpty() ? u"No answer from org.freedesktop.Flatpak."_s : reply.errorMessage();
        return false;
    }
    *pid = reply.arguments().constFirst().toUInt();
    return true;
}

} // namespace

LaunchResult launchBrowser(const Browser &browser, const QString &url, const SelfIdentity &self)
{
    LaunchResult result;
    const QString name = browser.name.isEmpty() ? browser.id : browser.name;

    const CommandValidation validation = validateCommand(browser.command, self);
    if (!validation.ok()) {
        result.error = u"Cannot start %1: %2"_s.arg(name, validation.errors.join(u' '));
        return result;
    }
    const std::optional<LaunchCommand> command = buildLaunchCommand(browser.command, url);
    if (!command) {
        result.error = u"Cannot start %1: the link cannot be passed to it safely."_s.arg(name);
        return result;
    }
    result.command = *command;
    if (HostSystem::current().sandboxed) {
        if (!hostReachable()) {
            result.error = u"Cannot start %1: Klinker cannot run programs outside its Flatpak sandbox. "
                           u"It needs access to org.freedesktop.Flatpak on the session bus."_s.arg(name);
            return result;
        }
        QString error;
        if (!startOnHost(*command, QDir::homePath(), &result.pid, &error)) {
            result.error = u"Could not start %1 (%2): %3"_s.arg(name, command->program, error);
            return result;
        }
        result.ok = true;
        return result;
    }

    QProcess process;
    process.setProgram(result.command.program);
    process.setArguments(result.command.arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setStandardInputFile(QProcess::nullDevice());
    if (!process.startDetached(&result.pid)) {
        result.error = u"Could not start %1 (%2): %3"_s.arg(name, result.command.program, process.errorString());
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace klinker
