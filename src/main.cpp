#include "app/application.h"
#include "app/commandline.h"
#include "ui/theme.h"

#include <KIconTheme>

#include <QApplication>

#include <cstdio>

using namespace Qt::StringLiterals;

namespace
{

void setApplicationMetadata()
{
    QCoreApplication::setApplicationName(u"klinker"_s);
    QCoreApplication::setApplicationVersion(QStringLiteral(KLINKER_VERSION));
    QCoreApplication::setOrganizationDomain(u"manicminer.github.io"_s);
}

void print(FILE *stream, const QString &text)
{
    const QByteArray bytes = text.toLocal8Bit();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
    std::fflush(stream);
}

} // namespace

int main(int argc, char **argv)
{
    // Decode argv ourselves so that links with invalid UTF-8 are not altered.
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.append(klinker::decodeArgument(QByteArray(argv[i])));
    }
    klinker::CommandLine commandLine = klinker::parseCommandLine(arguments);

    if (commandLine.action != klinker::CommandLine::Action::Run) {
        // No display is needed to print help or errors.
        QCoreApplication core(argc, argv);
        setApplicationMetadata();
        commandLine = klinker::parseCommandLine(arguments);
        switch (commandLine.action) {
        case klinker::CommandLine::Action::ShowHelp:
            print(stdout, commandLine.helpText);
            return 0;
        case klinker::CommandLine::Action::ShowVersion:
            print(stdout, u"klinker %1\n"_s.arg(QStringLiteral(KLINKER_VERSION)));
            return 0;
        default:
            print(stderr, u"klinker: %1\nTry “klinker --help”.\n"_s.arg(commandLine.error));
            return 2;
        }
    }

    // Breeze icons recoloured for the palette, also outside Plasma.
    KIconTheme::initTheme();
    QApplication app(argc, argv);
    setApplicationMetadata();
    QGuiApplication::setApplicationDisplayName(klinker::AppDisplayName.toString());
    QGuiApplication::setDesktopFileName(klinker::AppId.toString());
    QApplication::setWindowIcon(klinker::appIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    klinker::Application::Options options;
    options.configPath = commandLine.configPath;
    options.alwaysChoose = commandLine.choose;
    klinker::Application controller(options);
    QObject::connect(&controller, &klinker::Application::finished, &app, &QCoreApplication::quit, Qt::QueuedConnection);

    if (commandLine.settings) {
        controller.openSettings();
    }
    controller.openUrls(commandLine.urls);
    if (controller.isIdle()) {
        return 0;
    }
    return app.exec();
}
