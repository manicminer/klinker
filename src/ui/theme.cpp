#include "theme.h"

#include "core/hostsystem.h"

#include <QApplication>
#include <QFileInfo>
#include <QPalette>
#include <QStyle>

using namespace Qt::StringLiterals;

// Resources in a static library must be registered explicitly.
static void initResources()
{
    static const bool done = [] {
        Q_INIT_RESOURCE(resources);
        return true;
    }();
    Q_UNUSED(done);
}

namespace klinker
{

QIcon appIcon()
{
    initResources();
    return QIcon::fromTheme(AppId.toString(), QIcon(u":/klinker/app-icon.svg"_s));
}

QIcon browserIcon(const QString &spec)
{
    QString file;
    if (QFileInfo(spec).isAbsolute()) {
        file = sandboxPath(spec);
    } else if (!spec.isEmpty()) {
        if (QIcon::hasThemeIcon(spec)) {
            return QIcon::fromTheme(spec);
        }
        // Inside a Flatpak sandbox the theme sees only the host's /usr/share/icons
        // and per-user Flatpak icons; other host icons are looked up as files.
        if (HostSystem::current().sandboxed) {
            file = hostIconFile(spec);
        }
    }
    if (QFileInfo(file).isFile()) {
        const QIcon icon(file);
        if (!icon.availableSizes().isEmpty() || !icon.pixmap(32).isNull()) {
            return icon;
        }
    }
    return QIcon::fromTheme(u"internet-web-browser"_s, QApplication::style()->standardIcon(QStyle::SP_DesktopIcon));
}

QColor mixColors(const QColor &from, const QColor &to, qreal amount)
{
    const auto mix = [amount](float a, float b) {
        return a + (b - a) * float(amount);
    };
    const QColor a = from.toRgb();
    const QColor b = to.toRgb();
    return QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()),
                            mix(a.alphaF(), b.alphaF()));
}

QColor subduedTextColor(const QPalette &palette)
{
    // Keeps Breeze Light and Dark above the WCAG AA contrast of 4.5:1.
    return mixColors(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.32);
}

} // namespace klinker
