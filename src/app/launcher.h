#pragma once

#include "core/browsercatalog.h"
#include "core/execcommand.h"

#include <QString>

namespace klinker
{

struct LaunchResult {
    bool ok = false;
    qint64 pid = 0;
    QString error; // for the user; plain text
    LaunchCommand command;
};

// Starts the browser detached with the URL as a single argv element; no shell
// is involved. Commands that fail validation are refused. Inside a Flatpak
// sandbox the browser is started on the host with Flatpak's HostCommand, and
// success means the host has executed it; `pid` is then the host's PID.
LaunchResult launchBrowser(const Browser &browser, const QString &url, const SelfIdentity &self);

} // namespace klinker
