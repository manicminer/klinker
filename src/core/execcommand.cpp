#include "execcommand.h"

#include "hostsystem.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

QString expandFieldCodes(const QString &token, const DesktopExecContext &context)
{
    QString result;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token.at(i);
        if (c != u'%' || i + 1 == token.size()) {
            result += c;
            continue;
        }
        switch (token.at(++i).unicode()) {
        case 'u':
        case 'U':
        case 'f':
        case 'F':
            result += UrlPlaceholder;
            break;
        case 'i':
            result += context.icon;
            break;
        case 'c':
            result += context.name;
            break;
        case 'k':
            result += context.desktopFilePath;
            break;
        case '%':
            result += u'%';
            break;
        default:
            // Deprecated (%d %D %n %N %v %m) and unknown codes expand to nothing.
            break;
        }
    }
    return result;
}

// GNU env options that cannot change which program runs. Anything else, such
// as -S/--split-string, makes the command uninspectable.
bool isKnownEnvLongOption(QStringView name, bool *takesArgument)
{
    *takesArgument = name == "unset"_L1 || name == "chdir"_L1;
    return *takesArgument || name == "ignore-environment"_L1 || name == "null"_L1 || name == "debug"_L1
        || name == "default-signal"_L1 || name == "ignore-signal"_L1 || name == "block-signal"_L1 || name == "list-signal-handling"_L1;
}

// Looks through `env [options] [NAME=VALUE]... program args` wrappers. Fails
// for options it does not know, rather than guess where the program is.
std::optional<CommandTemplate> unwrapEnv(const CommandTemplate &command)
{
    CommandTemplate current = command;
    while (QFileInfo(current.program).fileName() == "env"_L1) {
        qsizetype i = 0;
        bool optionsDone = false;
        QString next;
        for (; i < current.arguments.size(); ++i) {
            const QString &arg = current.arguments.at(i);
            if (!optionsDone && arg == "--"_L1) {
                optionsDone = true;
                continue;
            }
            if (!optionsDone && arg.startsWith("--"_L1)) {
                const qsizetype equals = arg.indexOf(u'=');
                bool takesArgument = false;
                if (!isKnownEnvLongOption(QStringView(arg).sliced(2, (equals < 0 ? arg.size() : equals) - 2), &takesArgument)) {
                    return std::nullopt;
                }
                if (takesArgument && equals < 0) {
                    ++i;
                }
                continue;
            }
            if (!optionsDone && arg.startsWith(u'-')) {
                // Short option cluster such as "-", "-i", "-iu NAME" or "-CDIR".
                for (qsizetype j = 1; j < arg.size(); ++j) {
                    const QChar option = arg.at(j);
                    if (option == u'u' || option == u'C') {
                        if (j + 1 == arg.size()) {
                            ++i;
                        }
                        break;
                    }
                    if (option != u'i' && option != u'0' && option != u'v') {
                        return std::nullopt;
                    }
                }
                continue;
            }
            if (arg.contains(u'=')) {
                continue;
            }
            next = arg;
            break;
        }
        if (next.isEmpty()) {
            return std::nullopt;
        }
        current = {next, current.arguments.mid(i + 1)};
    }
    return current;
}

bool isShell(const QString &program)
{
    static const QStringList shells = {u"sh"_s, u"bash"_s, u"dash"_s, u"ash"_s, u"zsh"_s, u"ksh"_s,
                                       u"mksh"_s, u"fish"_s, u"csh"_s, u"tcsh"_s, u"busybox"_s};
    return shells.contains(QFileInfo(program).fileName());
}

// Deliberately broad: any option that mentions c or C, which covers -c and
// -C clusters, +c, fish's --command and --init-command and their abbreviations.
// Working out which later argument becomes the command string is not attempted.
bool isShellCommandOption(const QString &arg)
{
    return (arg.startsWith(u'-') || arg.startsWith(u'+')) && arg.contains(u'c', Qt::CaseInsensitive);
}

} // namespace

ExecParseResult parseDesktopExec(const QString &exec, const DesktopExecContext &context)
{
    QStringList tokens;
    QString current;
    bool inToken = false;

    for (qsizetype i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (c == u'"') {
            inToken = true;
            for (++i;; ++i) {
                if (i == exec.size()) {
                    return {std::nullopt, u"The command line has an unterminated double quote."_s};
                }
                const QChar q = exec.at(i);
                if (q == u'"') {
                    break;
                }
                if (q == u'\\' && i + 1 < exec.size() && QStringView(u"\"`$\\").contains(exec.at(i + 1))) {
                    current += exec.at(++i);
                } else {
                    current += q;
                }
            }
        } else if (c == u'\'') {
            // Not in the specification, but common in the wild: literal quoting.
            const qsizetype close = exec.indexOf(u'\'', i + 1);
            if (close < 0) {
                return {std::nullopt, u"The command line has an unterminated single quote."_s};
            }
            inToken = true;
            current += QStringView(exec).sliced(i + 1, close - i - 1);
            i = close;
        } else if (c == u'\\') {
            inToken = true;
            current += i + 1 < exec.size() ? exec.at(++i) : c;
        } else if (c == u' ' || c == u'\t' || c == u'\n') {
            if (inToken) {
                tokens.append(current);
                current.clear();
                inToken = false;
            }
        } else {
            inToken = true;
            current += c;
        }
    }
    if (inToken) {
        tokens.append(current);
    }

    QStringList expanded;
    for (const QString &token : std::as_const(tokens)) {
        if (token == "%i"_L1) {
            if (!context.icon.isEmpty()) {
                expanded << u"--icon"_s << context.icon;
            }
            continue;
        }
        const QString value = expandFieldCodes(token, context);
        if (!token.isEmpty() && value.isEmpty()) {
            continue;
        }
        expanded.append(value);
    }

    if (expanded.isEmpty() || expanded.first().isEmpty() || expanded.first().contains(UrlPlaceholder)) {
        return {std::nullopt, u"The command line does not start with a program."_s};
    }
    return {CommandTemplate{expanded.first(), expanded.mid(1)}, QString()};
}

std::optional<LaunchCommand> buildLaunchCommand(const CommandTemplate &command, const QString &url)
{
    if (url.isEmpty() || url.startsWith(u'-')) {
        return std::nullopt;
    }

    const QString placeholder = UrlPlaceholder.toString();
    LaunchCommand launch{command.program, {}};
    bool substituted = false;
    for (const QString &argument : command.arguments) {
        if (argument.contains(placeholder)) {
            launch.arguments.append(QString(argument).replace(placeholder, url));
            substituted = true;
        } else {
            launch.arguments.append(argument);
        }
    }
    if (!substituted) {
        launch.arguments.append(url);
    }
    return launch;
}

QStringList parseArgumentLines(const QString &text)
{
    QStringList arguments;
    for (QStringView line : QStringView(text).tokenize(u'\n')) {
        if (line.endsWith(u'\r')) {
            line.chop(1);
        }
        if (!line.trimmed().isEmpty()) {
            arguments.append(line.toString());
        }
    }
    return arguments;
}

QString formatArgumentLines(const QStringList &arguments)
{
    return arguments.join(u'\n');
}

SelfIdentity SelfIdentity::current()
{
    const QString appId = QStringLiteral(KLINKER_APP_ID);
    SelfIdentity self{{appId + u".desktop"_s, u"klinker.desktop"_s}, {u"klinker"_s, appId}, {}, {appId}};
    if (const HostSystem &host = HostSystem::current(); host.sandboxed && host.appId != appId) {
        self.desktopIds.append(host.appId + u".desktop"_s);
        self.executableNames.append(host.appId);
        self.flatpakIds.append(host.appId);
    }
    if (QCoreApplication::instance()) {
        self.executablePaths.append(QCoreApplication::applicationFilePath());
    }
    return self;
}

QString resolveExecutable(const QString &program)
{
    if (HostSystem::current().sandboxed) {
        return hostExecutable(program);
    }
    if (program.contains(u'/')) {
        const QFileInfo info(program);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(program);
}

namespace
{

// resolveExecutable() with every symlink resolved; on the host when sandboxed.
QString canonicalExecutable(const QString &program)
{
    if (HostSystem::current().sandboxed) {
        return hostCanonicalExecutable(program);
    }
    const QString resolved = resolveExecutable(program);
    return resolved.isEmpty() ? QString() : QFileInfo(resolved).canonicalFilePath();
}

} // namespace

QString effectiveProgram(const CommandTemplate &command)
{
    const auto unwrapped = unwrapEnv(command);
    return unwrapped && !unwrapped->program.contains(UrlPlaceholder) ? unwrapped->program : QString();
}

bool isSelfCommand(const CommandTemplate &command, const SelfIdentity &self)
{
    const auto unwrapped = unwrapEnv(command);
    if (!unwrapped || unwrapped->program.contains(UrlPlaceholder)) {
        return true;
    }
    const QString &program = unwrapped->program;
    // An alias, e.g. ~/bin/browser, is known by what it links to, resolved
    // where it runs: on the host when sandboxed.
    const QString canonical = canonicalExecutable(program);
    QStringList fileNames{QFileInfo(program).fileName()};
    if (!canonical.isEmpty()) {
        fileNames.append(QFileInfo(canonical).fileName());
    }
    if (std::ranges::any_of(fileNames, [&self](const QString &name) {
            return self.executableNames.contains(name);
        })) {
        return true;
    }
    // Any mention of our ID, e.g. `flatpak run --branch=stable <id>` or `<id>//stable`;
    // telling which argument is the application would need flatpak's own parser.
    if (fileNames.contains("flatpak"_L1) && std::ranges::any_of(unwrapped->arguments, [&self](const QString &argument) {
            return std::ranges::any_of(self.flatpakIds, [&argument](const QString &id) {
                return argument == id || argument.startsWith(id + u'/');
            });
        })) {
        return true;
    }
    if (canonical.isEmpty()) {
        return false;
    }
    return std::ranges::any_of(self.executablePaths, [&canonical](const QString &path) {
        return QFileInfo(path).canonicalFilePath() == canonical;
    });
}

CommandValidation validateCommand(const CommandTemplate &command, const SelfIdentity &self)
{
    CommandValidation result;
    if (command.program.trimmed().isEmpty()) {
        result.errors.append(u"No program is set."_s);
        return result;
    }
    if (command.program.contains(UrlPlaceholder)) {
        result.errors.append(u"The program cannot contain the {url} placeholder."_s);
        return result;
    }

    const auto unwrapped = unwrapEnv(command);
    if (!unwrapped || unwrapped->program.contains(UrlPlaceholder)) {
        result.errors.append(u"Cannot tell which program “%1” runs. Remove options such as -S, or run the program directly."_s.arg(command.program));
        return result;
    }

    if (isSelfCommand(command, self)) {
        result.errors.append(u"This command would open Klinker itself."_s);
    }
    if (resolveExecutable(unwrapped->program).isEmpty()) {
        result.warnings.append(unwrapped->program.contains(u'/') ? u"“%1” does not exist or is not executable."_s.arg(unwrapped->program)
                                                                  : u"“%1” was not found in PATH."_s.arg(unwrapped->program));
    }

    // The URL reaches a shell either through {url} or by being appended, so a
    // shell must not be given a command string at all, and {url} must be a
    // whole argument (a positional parameter of a script file).
    if (isShell(unwrapped->program)) {
        const QStringList &arguments = unwrapped->arguments;
        if (std::ranges::any_of(arguments, isShellCommandOption)) {
            result.errors.append(u"The shell would run a command string, which the URL could become part of. Run a script file instead."_s);
        } else if (std::ranges::any_of(arguments, [](const QString &argument) {
                       return argument.contains(UrlPlaceholder) && argument != UrlPlaceholder;
                   })) {
            result.errors.append(u"The shell would interpret the URL as code. Pass {url} as a separate argument instead."_s);
        }
    }
    return result;
}

} // namespace klinker
