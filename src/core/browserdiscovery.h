#pragma once

#include "execcommand.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace kbs
{

struct DetectedBrowser {
    QString desktopId;
    QString desktopFilePath;
    QString name;
    QString icon;
    CommandTemplate command;
};

struct DiscoveryResult {
    QList<DetectedBrowser> browsers; // sorted by name
    QStringList warnings; // unusable browser entries, for diagnostics
};

// XDG data directories in precedence order: $XDG_DATA_HOME, $XDG_DATA_DIRS, then
// the user and system Flatpak export directories if they are not already listed.
QStringList defaultDataDirs();

// Desktop file ID of a file below an "applications" directory, e.g. "vendor-app.desktop".
QString desktopIdForPath(const QString &applicationsDir, const QString &filePath);

// Finds applications that handle http or https URLs in <dataDir>/applications.
// The first directory providing a desktop ID wins, even if that file hides it.
DiscoveryResult discoverBrowsers(const QStringList &dataDirs, const SelfIdentity &self);

} // namespace kbs
