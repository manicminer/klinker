#include "launcher.h"

#include <QDir>
#include <QProcess>

using namespace Qt::StringLiterals;

namespace klinker
{

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

    QProcess process;
    process.setProgram(command->program);
    process.setArguments(command->arguments);
    process.setWorkingDirectory(QDir::homePath());
    process.setStandardInputFile(QProcess::nullDevice());
    if (!process.startDetached(&result.pid)) {
        result.error = u"Could not start %1 (%2): %3"_s.arg(name, command->program, process.errorString());
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace klinker
