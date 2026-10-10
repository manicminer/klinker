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
    QCoreApplication::setApplicationVersion(QStringLiteral(KBS_VERSION));
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
        arguments.append(kbs::decodeArgument(QByteArray(argv[i])));
    }
    kbs::CommandLine commandLine = kbs::parseCommandLine(arguments);

    if (commandLine.action != kbs::CommandLine::Action::Run) {
        // No display is needed to print help or errors.
        QCoreApplication core(argc, argv);
        setApplicationMetadata();
        commandLine = kbs::parseCommandLine(arguments);
        switch (commandLine.action) {
        case kbs::CommandLine::Action::ShowHelp:
            print(stdout, commandLine.helpText);
            return 0;
        case kbs::CommandLine::Action::ShowVersion:
            print(stdout, u"klinker %1\n"_s.arg(QStringLiteral(KBS_VERSION)));
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
    QGuiApplication::setApplicationDisplayName(kbs::AppDisplayName.toString());
    QGuiApplication::setDesktopFileName(kbs::AppId.toString());
    QApplication::setWindowIcon(kbs::appIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    kbs::Application::Options options;
    options.configPath = commandLine.configPath;
    options.alwaysChoose = commandLine.choose;
    kbs::Application controller(options);
    QObject::connect(&controller, &kbs::Application::finished, &app, &QCoreApplication::quit, Qt::QueuedConnection);

    if (commandLine.settings) {
        controller.openSettings();
    }
    controller.openUrls(commandLine.urls);
    if (controller.isIdle()) {
        return 0;
    }
    return app.exec();
}
