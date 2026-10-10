#include "testsupport.h"

#include <KColorScheme>
#include <KIconTheme>
#include <KSharedConfig>

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QScrollBar>
#include <QStyle>
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

bool hasStyle(const QString &style)
{
    return QStyleFactory::keys().contains(style, Qt::CaseInsensitive);
}

void applyTheme(bool dark, const QString &style)
{
    if (!style.isEmpty()) {
        QApplication::setStyle(style);
    } else if (hasStyle(u"Breeze"_s)) {
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

QString horizontalStripProblem(const QAbstractItemView *list)
{
    QStringList problems;
    const QScrollBar *vertical = list->verticalScrollBar();
    const QScrollBar *horizontal = list->horizontalScrollBar();
    if (list->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) {
        problems.append(u"the vertical scroll bar is not always off"_s);
    }
    if (vertical->isVisible() || vertical->maximum() != 0) {
        problems.append(u"vertical scroll bar shown %1 with range %2"_s.arg(vertical->isVisible()).arg(vertical->maximum()));
    }
    const int rows = list->model() ? list->model()->rowCount() : 0;
    if (rows == 0) {
        problems.append(u"no tiles"_s);
        return problems.join(u"; "_s);
    }
    const QRect viewport = list->viewport()->rect();
    const QRect first = list->visualRect(list->model()->index(0, 0));
    const QRect last = list->visualRect(list->model()->index(rows - 1, 0));
    const int rowWidth = last.right() - first.left() + 1;
    if (rowWidth != rows * first.width() || last.top() != first.top()) {
        problems.append(u"tiles are not one row: first %1,%2 last %3,%4"_s.arg(first.left()).arg(first.top()).arg(last.left()).arg(last.top()));
    }
    for (int row = 0; row < rows; ++row) {
        const QRect tile = list->visualRect(list->model()->index(row, 0));
        if (tile.top() < viewport.top() || tile.bottom() > viewport.bottom()) {
            problems.append(u"tile %1 (y %2 to %3) is cut off by the %4 px high viewport"_s.arg(row).arg(tile.top()).arg(tile.bottom()).arg(viewport.height()));
            break;
        }
    }
    if (rowWidth <= viewport.width()) {
        if (horizontal->isVisible() || horizontal->maximum() != 0) {
            problems.append(u"a %1 px row fits the %2 px viewport but the horizontal scroll bar is shown %3 with range %4"_s.arg(rowWidth)
                                .arg(viewport.width())
                                .arg(horizontal->isVisible())
                                .arg(horizontal->maximum()));
        }
        if (!viewport.contains(first) || !viewport.contains(last)) {
            problems.append(u"the first or last tile is outside the viewport"_s);
        }
    } else {
        if (!horizontal->isVisible() || horizontal->maximum() != rowWidth - viewport.width()) {
            problems.append(u"a %1 px row in a %2 px viewport needs a horizontal scroll bar, shown %3 with range %4"_s.arg(rowWidth)
                                .arg(viewport.width())
                                .arg(horizontal->isVisible())
                                .arg(horizontal->maximum()));
        }
    }
    const QRect viewportRect = list->viewport()->geometry();
    if (!list->rect().contains(viewportRect)) {
        problems.append(u"the viewport is outside the list"_s);
    }
    if (horizontal->isVisible()) {
        const QRect bar(horizontal->mapTo(list, QPoint()), horizontal->size());
        if (!list->rect().contains(bar)) {
            problems.append(u"the horizontal scroll bar is outside the list"_s);
        }
        if (bar.intersects(viewportRect) && !list->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, horizontal)) {
            problems.append(u"the horizontal scroll bar covers the tiles"_s);
        }
    }
    return problems.join(u"; "_s);
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
