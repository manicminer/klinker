#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace klinker
{

struct CommandLine {
    enum class Action {
        Run,
        ShowHelp,
        ShowVersion,
        Error, // message in `error`; exit with status 2
    };

    Action action = Action::Run;
    QString error;
    QString helpText;

    bool settings = false; // --settings, or no links given
    bool choose = false; // --choose: always show the chooser
    QString configPath; // --config FILE, empty for the default
    QStringList urls; // validated links in the given order
};

// Parses argv (including the program name). Links are kept byte for byte;
// paths of existing local files become file:// URLs.
CommandLine parseCommandLine(const QStringList &arguments);

// Decodes a raw argv element. Valid UTF-8 is decoded as such; otherwise every
// byte outside ASCII is percent-encoded, so no byte of a link is replaced or lost.
QString decodeArgument(const QByteArray &argument);

// Why a command-line argument cannot be opened, or an empty string. Accepted
// are http and https URLs with a host and file URLs.
QString urlArgumentError(const QString &url);

} // namespace klinker
