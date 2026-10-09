#include "theme.h"

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

namespace kbs
{

QIcon appIcon()
{
    initResources();
    return QIcon::fromTheme(AppId.toString(), QIcon(u":/kbs/app-icon.svg"_s));
}

QIcon browserIcon(const QString &spec)
{
    if (!spec.isEmpty()) {
        if (QFileInfo(spec).isAbsolute()) {
            if (QFileInfo(spec).isFile()) {
                const QIcon icon(spec);
                if (!icon.availableSizes().isEmpty() || !icon.pixmap(32).isNull()) {
                    return icon;
                }
            }
        } else if (QIcon::hasThemeIcon(spec)) {
            return QIcon::fromTheme(spec);
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

} // namespace kbs
