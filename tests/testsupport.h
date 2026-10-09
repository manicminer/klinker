#pragma once

#include <QImage>
#include <QPalette>
#include <QString>
#include <QStringList>

class QWidget;

namespace kbs::test
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
void applyTheme(bool dark);

// Saves widget->grab() as <KBS_SCREENSHOT_DIR>/<name>.png when the variable
// is set; returns the file name or an empty string.
QString saveScreenshot(QWidget *widget, const QString &name);

// Writes <dataDir>/applications/<desktopId> describing an HTTP(S) handler.
void writeBrowserDesktopFile(const QString &dataDir, const QString &desktopId, const QString &name, const QString &exec,
                             const QString &icon = QString());

// Mean lightness (0-255) of the image, ignoring fully transparent pixels.
int meanLightness(const QImage &image);

} // namespace kbs::test
