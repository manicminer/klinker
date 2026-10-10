#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace klinker
{

// Placeholder replaced by the URL inside any argument that contains it.
inline constexpr QStringView UrlPlaceholder = u"{url}";

// A browser command before a URL is known: program plus argument templates.
struct CommandTemplate {
    QString program;
    QStringList arguments;

    bool operator==(const CommandTemplate &) const = default;
};

// Values used to expand %i, %c and %k.
struct DesktopExecContext {
    QString name;
    QString icon;
    QString desktopFilePath;
};

struct ExecParseResult {
    std::optional<CommandTemplate> command;
    QString error;
};

// Splits an Exec value (already string-unescaped, as returned by KConfig)
// following the Desktop Entry Specification and converts field codes:
// %u %U %f %F -> {url}, %i -> --icon <icon>, %c -> name, %k -> file path,
// %% -> %, deprecated or unknown codes are dropped.
ExecParseResult parseDesktopExec(const QString &exec, const DesktopExecContext &context = {});

struct LaunchCommand {
    QString program;
    QStringList arguments;
};

// Substitutes the URL into every argument containing {url}, or appends it as
// the final argument when none does. The URL is never split or interpreted.
// Returns nothing for an empty URL or one starting with '-', which a browser
// could mistake for an option.
std::optional<LaunchCommand> buildLaunchCommand(const CommandTemplate &command, const QString &url);

// Editing format for manual arguments: one argument per line. Blank lines are
// ignored and a trailing carriage return is dropped; everything else, including
// leading and trailing spaces, is kept verbatim.
QStringList parseArgumentLines(const QString &text);
QString formatArgumentLines(const QStringList &arguments);

// How this application can be recognised so it is never used as a browser.
struct SelfIdentity {
    QStringList desktopIds;
    QStringList executableNames; // file names, wherever they are installed
    QStringList executablePaths;

    static SelfIdentity current();
};

// Absolute path of an executable given as a path or a name looked up in PATH;
// empty when there is no such executable file.
QString resolveExecutable(const QString &program);

// The program that actually runs, looking through `env` wrappers. Empty when it
// cannot be determined, for example with `env -S`, an env option that is not
// known or {url} in the program position.
QString effectiveProgram(const CommandTemplate &command);

// True when the command runs this application, or might: a command whose
// effective program cannot be determined counts as self.
bool isSelfCommand(const CommandTemplate &command, const SelfIdentity &self);

struct CommandValidation {
    QStringList errors; // the command must not be used
    QStringList warnings; // the command may fail, e.g. the executable is missing

    bool ok() const
    {
        return errors.isEmpty();
    }
};

// Errors for commands that run this application, whose effective program cannot
// be determined, or that give a shell a command string (-c and similar, wherever
// the URL ends up) or embed {url} inside a shell argument.
CommandValidation validateCommand(const CommandTemplate &command, const SelfIdentity &self);

} // namespace klinker
