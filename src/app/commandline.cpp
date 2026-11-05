#include "commandline.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QStringDecoder>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

bool hasForbiddenCharacters(const QString &text)
{
    return std::ranges::any_of(text, [](QChar c) {
        return c.isSpace() || c.category() == QChar::Other_Control || c.category() == QChar::Other_Format;
    });
}

// A path to an existing local file, as given on the command line by file
// managers for text/html files.
QString localFileUrl(const QString &argument)
{
    if (argument.isEmpty() || argument.contains(u"://"_s)) {
        return {};
    }
    const QFileInfo info(QDir::current(), argument);
    if (!info.isFile()) {
        return {};
    }
    return QUrl::fromLocalFile(info.absoluteFilePath()).toString(QUrl::FullyEncoded);
}

// KIO desktop launches can deliver literal ASCII spaces via QUrl::PrettyDecoded;
// re-encode those while preserving the rest of the received argument.
QString encodeSpaces(const QString &argument)
{
    const qsizetype colon = argument.indexOf(u':');
    const QString scheme = argument.left(colon).toLower();
    if (colon < 0 || (scheme != u"http"_s && scheme != u"https"_s && scheme != u"file"_s)) {
        return argument;
    }
    return QString(argument).replace(u' ', u"%20"_s);
}

} // namespace

QString decodeArgument(const QByteArray &argument)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString decoded = utf8.decode(argument);
    if (!utf8.hasError()) {
        return decoded;
    }
    QString result;
    for (const char byte : argument) {
        const auto value = uchar(byte);
        if (value < 0x80) {
            result.append(QLatin1Char(byte));
        } else {
            result.append(u'%').append(QString::number(value, 16).toUpper().rightJustified(2, u'0'));
        }
    }
    return result;
}

QString urlArgumentError(const QString &url)
{
    if (url.isEmpty()) {
        return u"An empty argument is not a link."_s;
    }
    if (url.startsWith(u'-')) {
        return u"“%1” is not a link."_s.arg(url);
    }
    if (hasForbiddenCharacters(url)) {
        return u"“%1” contains spaces or control characters; links must be percent-encoded."_s.arg(url.simplified());
    }
    const QUrl parsed(url, QUrl::TolerantMode);
    if (!parsed.isValid()) {
        return u"“%1” is not a valid link: %2"_s.arg(url, parsed.errorString());
    }
    const QString scheme = parsed.scheme().toLower();
    if (scheme == u"http"_s || scheme == u"https"_s) {
        if (parsed.host().isEmpty()) {
            return u"“%1” has no host name."_s.arg(url);
        }
        return {};
    }
    if (scheme == u"file"_s) {
        if (!parsed.host().isEmpty() && parsed.host().compare(u"localhost"_s, Qt::CaseInsensitive) != 0) {
            return u"“%1” refers to a file on another computer."_s.arg(url);
        }
        return {};
    }
    if (scheme.isEmpty()) {
        return u"“%1” is neither a link with http:// or https:// nor an existing file."_s.arg(url);
    }
    return u"“%1” is not a web link; only http, https and file links can be opened."_s.arg(url);
}

CommandLine parseCommandLine(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(u"Opens links in the browser chosen by your rules, or asks which browser to use."_s);
    // Own options instead of addHelpOption(), which also adds --help-all for Qt's
    // generic options; those are not meant for this application.
    const QCommandLineOption help({u"h"_s, u"help"_s}, u"Show this help."_s);
    const QCommandLineOption version({u"v"_s, u"version"_s}, u"Show the version."_s);
    parser.addOption(help);
    parser.addOption(version);
    const QCommandLineOption settings(u"settings"_s, u"Open the settings window (also the default without links)."_s);
    const QCommandLineOption choose(u"choose"_s, u"Always show the chooser, ignoring routing rules."_s);
    const QCommandLineOption config(u"config"_s, u"Use this configuration file instead of the default one."_s, u"file"_s);
    const QCommandLineOption list(u"list-browsers"_s, u"List the browsers, their commands and any problems, then exit."_s);
    parser.addOptions({settings, choose, config, list});
    parser.addPositionalArgument(u"links"_s, u"http(s) or file links to open, one after another."_s, u"[link…]"_s);

    CommandLine result;
    result.helpText = parser.helpText();
    if (!parser.parse(arguments)) {
        result.action = CommandLine::Action::Error;
        result.error = parser.errorText();
        return result;
    }
    if (parser.isSet(help)) {
        result.action = CommandLine::Action::ShowHelp;
        return result;
    }
    if (parser.isSet(version)) {
        result.action = CommandLine::Action::ShowVersion;
        return result;
    }

    result.choose = parser.isSet(choose);
    result.configPath = parser.value(config);
    if (parser.isSet(config) && result.configPath.isEmpty()) {
        result.action = CommandLine::Action::Error;
        result.error = u"--config needs a file name."_s;
        return result;
    }
    if (parser.isSet(list)) {
        result.action = parser.positionalArguments().isEmpty() && !parser.isSet(settings) && !parser.isSet(choose) ? CommandLine::Action::ListBrowsers
                                                                                                                    : CommandLine::Action::Error;
        result.error = u"--list-browsers cannot be combined with links, --settings or --choose."_s;
        return result;
    }
    for (const QString &argument : parser.positionalArguments()) {
        if (const QString file = localFileUrl(argument); !file.isEmpty()) {
            result.urls.append(file);
            continue;
        }
        const QString url = encodeSpaces(argument);
        if (const QString error = urlArgumentError(url); !error.isEmpty()) {
            result.action = CommandLine::Action::Error;
            result.error = error;
            return result;
        }
        result.urls.append(url);
    }
    result.settings = parser.isSet(settings) || result.urls.isEmpty();
    return result;
}

} // namespace klinker
