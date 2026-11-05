#pragma once

#include <QString>

namespace klinker
{

// The host path of a file inside a Flatpak sandbox: files chosen in a file
// dialog or forwarded on the command line arrive as document portal paths
// ($XDG_RUNTIME_DIR/doc/<id>/<name>), and host-os views as /run/host/usr/...
// Browsers run on the host and need the real path. Other paths, and every path
// outside a sandbox, are returned unchanged.
QString hostFilePath(const QString &path);

// hostFilePath() for the path of a file: URL; other links are unchanged.
QString hostFileUrl(const QString &url);

} // namespace klinker
