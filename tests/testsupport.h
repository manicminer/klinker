#pragma once

#include <QImage>
#include <QPalette>
#include <QString>
#include <QStringList>

class QAbstractItemView;
class QWidget;

namespace klinker::test
{

// Points XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_DATA_DIRS at an empty private
// directory so tests never read or write the user's configuration, colour
// scheme or desktop entries. Call from initMain(), before QApplication exists.
void isolateEnvironment();

// Breeze Light and Breeze Dark colours, used when the colour scheme files are
// not installed.
QPalette breezeLightPalette();
QPalette breezeDarkPalette();

// Applies Breeze Light or Breeze Dark (colour scheme and palette, and the
// Breeze widget style when installed), independent of the desktop's settings.
// With a style name, that widget style is used instead (see hasStyle()).
void applyTheme(bool dark, const QString &style = QString());

// Whether a widget style of this name is installed.
bool hasStyle(const QString &style);

// Applies a widget style (see applyTheme()) until destroyed, then the default
// light theme again.
class ThemeScope
{
public:
    explicit ThemeScope(const QString &style, bool dark = false)
    {
        applyTheme(dark, style);
    }
    ~ThemeScope()
    {
        applyTheme(false);
    }
    ThemeScope(const ThemeScope &) = delete;
    ThemeScope &operator=(const ThemeScope &) = delete;
};

// What is wrong with the horizontal browser strip, or an empty string: it never
// scrolls vertically, every tile is shown at its full height, and it scrolls
// horizontally exactly when the row of tiles is wider than its viewport, with
// the scroll bar below the viewport rather than over the tiles.
QString horizontalStripProblem(const QAbstractItemView *list);

// Saves widget->grab() as <KLINKER_SCREENSHOT_DIR>/<name>.png when the variable
// is set; returns the file name or an empty string.
QString saveScreenshot(QWidget *widget, const QString &name);

// Writes <dataDir>/applications/<desktopId> describing an HTTP(S) handler.
void writeBrowserDesktopFile(const QString &dataDir, const QString &desktopId, const QString &name, const QString &exec,
                             const QString &icon = QString());

// Mean lightness (0-255) of the image, ignoring fully transparent pixels.
int meanLightness(const QImage &image);

// Reports, as the platform plugin would, that another application now has the
// keyboard focus: no window of this process is focused. Only for offscreen
// tests; nothing outside the process is involved.
void focusOtherApplication();

// Reports that the window has the keyboard focus again.
void focusWindow(QWidget *window);

} // namespace klinker::test
