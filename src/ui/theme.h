#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QPalette;

namespace kbs
{

// Desktop file ID without ".desktop"; also the icon name.
inline constexpr QStringView AppId = u"" KBS_APP_ID;
inline constexpr QStringView AppDisplayName = u"Klinker";

// The themed application icon, or the bundled SVG when it is not installed.
QIcon appIcon();

// A browser icon from a theme icon name or an image file path, with a generic
// web-browser icon when neither resolves.
QIcon browserIcon(const QString &spec);

// Linear interpolation between two colours; amount 0 gives `from`.
QColor mixColors(const QColor &from, const QColor &to, qreal amount);

// Secondary text on the window background, derived from the palette.
QColor subduedTextColor(const QPalette &palette);

} // namespace kbs
