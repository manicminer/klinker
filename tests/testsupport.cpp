#include "testsupport.h"

#include <KColorScheme>
#include <KIconTheme>
#include <KSharedConfig>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QWidget>
#include <QWindow>

#include <qpa/qwindowsysteminterface.h>

using namespace Qt::StringLiterals;

namespace kbs::test
{

void isolateEnvironment()
{
    static QTemporaryDir root(QDir::tempPath() + u"/kbs-test-XXXXXX"_s);
    const QString base = root.path();
    QDir().mkpath(base + u"/config"_s);
    QDir().mkpath(base + u"/data"_s);
    QDir().mkpath(base + u"/system-data"_s);
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(base + u"/config"_s));
    qputenv("XDG_DATA_HOME", QFile::encodeName(base + u"/data"_s));
    // The system directories stay available for icon themes; tests that discover
    // browsers pass their own directories explicitly.
    qputenv("XDG_DATA_DIRS", QFile::encodeName(base + u"/system-data:/usr/local/share:/usr/share"_s));
    // As in main(): icons follow the palette, so dark screenshots get light icons.
    KIconTheme::initTheme();
}

namespace
{

QPalette makePalette(const char *window, const char *windowText, const char *base, const char *alternateBase, const char *text,
                     const char *button, const char *highlight, const char *highlightedText, const char *disabledText,
                     const char *placeholder)
{
    const auto c = [](const char *name) {
        return QColor(QString::fromLatin1(name));
    };
    QPalette p;
    p.setColor(QPalette::Window, c(window));
    p.setColor(QPalette::WindowText, c(windowText));
    p.setColor(QPalette::Base, c(base));
    p.setColor(QPalette::AlternateBase, c(alternateBase));
    p.setColor(QPalette::Text, c(text));
    p.setColor(QPalette::Button, c(button));
    p.setColor(QPalette::ButtonText, c(windowText));
    p.setColor(QPalette::ToolTipBase, c(window));
    p.setColor(QPalette::ToolTipText, c(windowText));
    p.setColor(QPalette::Highlight, c(highlight));
    p.setColor(QPalette::HighlightedText, c(highlightedText));
    p.setColor(QPalette::Link, c(highlight));
    p.setColor(QPalette::PlaceholderText, c(placeholder));
    p.setColor(QPalette::Light, c(base).lighter(130));
    p.setColor(QPalette::Midlight, c(button).lighter(115));
    p.setColor(QPalette::Mid, c(button).darker(130));
    p.setColor(QPalette::Dark, c(button).darker(160));
    p.setColor(QPalette::Shadow, Qt::black);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, c(disabledText));
    }
    return p;
}

} // namespace

QPalette breezeLightPalette()
{
    return makePalette("#eff0f1", "#232629", "#ffffff", "#f7f7f7", "#232629", "#fcfcfc", "#3daee9", "#ffffff", "#a0a1a3",
                       "#707275");
}

QPalette breezeDarkPalette()
{
    return makePalette("#202326", "#fcfcfc", "#141618", "#1d1f22", "#fcfcfc", "#292c30", "#3daee9", "#fcfcfc", "#6e7175",
                       "#a1a9b1");
}

void applyTheme(bool dark)
{
    if (QStyleFactory::keys().contains(u"Breeze"_s, Qt::CaseInsensitive)) {
        QApplication::setStyle(u"Breeze"_s);
    } else {
        QApplication::setStyle(u"Fusion"_s);
    }
    // Prefer the real colour scheme: it sets the palette and the colour sets
    // KDE widgets read from kdeglobals (such as KPageView's header) together.
    const QString scheme = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                                  dark ? u"color-schemes/BreezeDark.colors"_s : u"color-schemes/BreezeLight.colors"_s);
    if (!scheme.isEmpty()) {
        const QString kdeglobals = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/kdeglobals"_s;
        QFile::remove(kdeglobals);
        QFile::copy(scheme, kdeglobals);
        QFile::setPermissions(kdeglobals, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        KSharedConfig::openConfig()->reparseConfiguration();
        QApplication::setPalette(KColorScheme::createApplicationPalette(KSharedConfig::openConfig()));
    } else {
        QApplication::setPalette(dark ? breezeDarkPalette() : breezeLightPalette());
    }
}

QString saveScreenshot(QWidget *widget, const QString &name)
{
    const QString dir = qEnvironmentVariable("KBS_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        return {};
    }
    QDir().mkpath(dir);
    const QString file = dir + u'/' + name + u".png"_s;
    return widget->grab().save(file) ? file : QString();
}

void writeBrowserDesktopFile(const QString &dataDir, const QString &desktopId, const QString &name, const QString &exec,
                             const QString &icon)
{
    QDir().mkpath(dataDir + u"/applications"_s);
    QFile file(dataDir + u"/applications/"_s + desktopId);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("cannot write desktop file");
    }
    QString text = u"[Desktop Entry]\nType=Application\nName=%1\nExec=%2\nMimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;\n"_s
                       .arg(name, exec);
    if (!icon.isEmpty()) {
        text += u"Icon=%1\n"_s.arg(icon);
    }
    file.write(text.toUtf8());
}

int meanLightness(const QImage &image)
{
    qint64 sum = 0;
    qint64 count = 0;
    const QImage converted = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < converted.height(); ++y) {
        for (int x = 0; x < converted.width(); ++x) {
            const QColor color = converted.pixelColor(x, y);
            if (color.alpha() == 0) {
                continue;
            }
            sum += color.lightness();
            ++count;
        }
    }
    return count ? int(sum / count) : -1;
}

void focusOtherApplication()
{
    QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(nullptr, Qt::ActiveWindowFocusReason);
}

void focusWindow(QWidget *window)
{
    QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(window->windowHandle(),
                                                                                              Qt::ActiveWindowFocusReason);
}

} // namespace kbs::test
