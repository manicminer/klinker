#pragma once

#include "core/config.h"
#include "network/redirectresolver.h"

#include <QObject>
#include <QString>

namespace kbs
{

struct ProcessedLink {
    QString originalUrl;
    QString finalUrl; // what rules see and the browser receives
    QString status; // human readable, plain text
    bool warning = false; // preprocessing failed; finalUrl is the last safe URL
    bool skipped = false;
};

// Plain-text description of a redirect result for the chooser.
QString describeRedirectResult(const RedirectResult &result);

// Applies the privacy settings to a link: tracking parameter removal and, for
// http(s) links, redirect resolution in the background. Emits finished()
// exactly once per start(), never from within start().
class LinkPipeline : public QObject
{
    Q_OBJECT

public:
    explicit LinkPipeline(QObject *parent = nullptr);

    void start(const QString &url, const PrivacySettings &privacy);
    // True when start() will contact the network for this link.
    static bool needsNetwork(const QString &url, const PrivacySettings &privacy);
    bool isActive() const;
    // Stops following redirects and finishes with the link as received
    // (tracking parameters still removed when enabled).
    void skip();
    // Stops without emitting finished().
    void cancel();

Q_SIGNALS:
    void finished(const kbs::ProcessedLink &link);

private:
    void onResolved(const RedirectResult &result);

    RedirectResolver m_resolver;
    QString m_url;
    PrivacySettings m_privacy;
    bool m_active = false;
};

} // namespace kbs
