#include "linkpipeline.h"

#include "core/urlcleaner.h"

#include <QUrl>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

QString trackingNote(int removed)
{
    if (removed == 0) {
        return {};
    }
    return removed == 1 ? u"removed 1 tracking parameter"_s : u"removed %1 tracking parameters"_s.arg(removed);
}

QString sentence(QString text)
{
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
        text += u'.';
    }
    return text;
}

bool isHttp(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    return scheme == u"http"_s || scheme == u"https"_s;
}

} // namespace

QString describeRedirectResult(const RedirectResult &result)
{
    const QString tracking = trackingNote(result.removedCount);
    const auto withTracking = [&tracking](const QString &text) {
        return sentence(tracking.isEmpty() ? text : text + u"; "_s + tracking);
    };
    const QString hops = result.hops == 1 ? u"1 redirect"_s : u"%1 redirects"_s.arg(result.hops);

    switch (result.status) {
    case RedirectStatus::NotRequested:
        return tracking.isEmpty() ? u"Opened as received."_s : sentence(tracking);
    case RedirectStatus::Resolved:
        return withTracking(result.hops == 0 ? u"the link does not redirect"_s : u"followed %1"_s.arg(hops));
    case RedirectStatus::UnsupportedUrl:
        return withTracking(u"redirects were not followed: %1"_s.arg(result.errorString));
    case RedirectStatus::TooManyRedirects:
        return withTracking(u"stopped after %1; showing the last address reached"_s.arg(hops));
    case RedirectStatus::RedirectLoop:
        return withTracking(u"the link redirects in a loop; showing the last address before the loop"_s);
    case RedirectStatus::InvalidRedirect:
        return withTracking(u"the server sent an invalid redirect after %1; showing the last valid address"_s.arg(hops));
    case RedirectStatus::UnsafeRedirect:
        return withTracking(u"an unsafe redirect was not followed (%1)"_s.arg(result.errorString));
    case RedirectStatus::NetworkError:
        return withTracking(u"could not check for redirects: %1"_s.arg(result.errorString));
    case RedirectStatus::Timeout:
        return withTracking(u"checking for redirects took too long; showing the last address reached"_s);
    }
    return {};
}

LinkPipeline::LinkPipeline(QObject *parent)
    : QObject(parent)
{
    connect(&m_resolver, &RedirectResolver::finished, this, &LinkPipeline::onResolved);
}

bool LinkPipeline::needsNetwork(const QString &url, const PrivacySettings &privacy)
{
    return privacy.unwrapRedirects && isHttp(url);
}

void LinkPipeline::start(const QString &url, const PrivacySettings &privacy)
{
    m_url = url;
    m_privacy = privacy;
    m_active = true;
    // Local files are never sent to the resolver, so no network check is reported for them.
    m_resolver.start(url, privacy.stripTrackingParameters, needsNetwork(url, privacy));
}

bool LinkPipeline::isActive() const
{
    return m_active;
}

void LinkPipeline::skip()
{
    if (!m_active) {
        return;
    }
    m_resolver.cancel();
    m_active = false;
    ProcessedLink link;
    link.originalUrl = m_url;
    link.finalUrl = m_url;
    int removed = 0;
    if (m_privacy.stripTrackingParameters) {
        const UrlCleanResult cleaned = stripTrackingParameters(m_url);
        link.finalUrl = cleaned.url;
        removed = cleaned.removedCount;
    }
    link.skipped = true;
    link.status = sentence(u"redirects were not followed (skipped)"_s + (removed ? u"; "_s + trackingNote(removed) : QString()));
    Q_EMIT finished(link);
}

void LinkPipeline::cancel()
{
    m_resolver.cancel();
    m_active = false;
}

void LinkPipeline::onResolved(const RedirectResult &result)
{
    if (!m_active) {
        return;
    }
    m_active = false;
    ProcessedLink link;
    link.originalUrl = result.originalUrl;
    link.finalUrl = result.finalUrl;
    link.status = describeRedirectResult(result);
    link.warning = result.error;
    Q_EMIT finished(link);
}

} // namespace kbs
