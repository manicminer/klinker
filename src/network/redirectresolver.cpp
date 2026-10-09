#include "redirectresolver.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

bool isHttpScheme(const QString &scheme)
{
    return scheme == "http"_L1 || scheme == "https"_L1;
}

bool hasCredentials(const QUrl &url)
{
    return !url.userName().isEmpty() || !url.password().isEmpty();
}

QString unsupportedUrlReason(const QUrl &url)
{
    if (!url.isValid() || !isHttpScheme(url.scheme())) {
        return u"Only http and https URLs can be resolved"_s;
    }
    if (url.host().isEmpty()) {
        return u"URL has no host"_s;
    }
    if (hasCredentials(url)) {
        return u"URLs with embedded credentials are not resolved"_s;
    }
    return {};
}

// Bytes Qt may buffer for a fallback GET before reading from the socket pauses; the
// reply is aborted as soon as the headers arrive, so this only bounds a body race.
constexpr qint64 s_getReadBufferSize = 16 * 1024;

// Identity of a request target for loop detection; fragments are never sent.
QString visitKey(const QUrl &url)
{
    return url.adjusted(QUrl::RemoveFragment | QUrl::NormalizePathSegments).toString(QUrl::FullyEncoded);
}

} // namespace

QString unsafeRedirectReason(const QUrl &from, const QUrl &to)
{
    if (!isHttpScheme(to.scheme())) {
        return u"Refused redirect to unsupported scheme \"%1\""_s.arg(to.scheme());
    }
    if (hasCredentials(to)) {
        return u"Refused redirect containing credentials"_s;
    }
    if (from.scheme() == "https"_L1 && to.scheme() == "http"_L1) {
        return u"Refused redirect from https to http"_s;
    }
    return {};
}

RedirectResolver::RedirectResolver(std::chrono::milliseconds requestTimeout, std::chrono::milliseconds totalTimeout, QObject *parent)
    : QObject(parent)
    , m_requestTimeout(requestTimeout)
    , m_totalTimeout(totalTimeout)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &RedirectResolver::onTimeout);
}

RedirectResolver::~RedirectResolver()
{
    dropReply();
}

void RedirectResolver::start(const QString &originalUrl, bool stripTracking, bool unwrapRedirects)
{
    cancel();
    m_active = true;
    m_stripTracking = stripTracking;
    m_result = RedirectResult();
    m_result.originalUrl = originalUrl;
    const UrlCleanResult cleaned = clean(originalUrl);
    m_current = cleaned.url;
    m_currentUrl = QUrl(m_current);
    m_result.removedCount = cleaned.removedCount;

    if (!unwrapRedirects) {
        finishQueued(RedirectStatus::NotRequested);
        return;
    }
    if (const QString reason = unsupportedUrlReason(m_currentUrl); !reason.isEmpty()) {
        finishQueued(RedirectStatus::UnsupportedUrl, reason);
        return;
    }
    m_visited = {visitKey(m_currentUrl)};
    m_deadline.setRemainingTime(m_totalTimeout);
    sendRequest(false);
}

void RedirectResolver::cancel()
{
    ++m_generation;
    dropReply();
    m_active = false;
}

bool RedirectResolver::isActive() const
{
    return m_active;
}

void RedirectResolver::sendRequest(bool useGet)
{
    m_usingGet = useGet;
    QNetworkRequest request(m_currentUrl.adjusted(QUrl::RemoveFragment));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    QSslConfiguration tls = request.sslConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(tls);

    if (useGet) {
        request.setRawHeader("Range", "bytes=0-0");
        m_reply = m_network.get(request);
        m_reply->setReadBufferSize(s_getReadBufferSize);
    } else {
        m_reply = m_network.head(request);
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(m_deadline.remainingTimeAsDuration());
    m_timer.start(std::min(m_requestTimeout, remaining));
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &RedirectResolver::onReplyProgress);
    connect(m_reply, &QNetworkReply::finished, this, &RedirectResolver::onReplyProgress);
}

// Called on response headers or completion, whichever comes first; the reply is
// discarded as soon as a status code is known so no body is ever consumed.
void RedirectResolver::onReplyProgress()
{
    const QVariant statusCode = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!statusCode.isValid()) {
        if (m_reply->isFinished()) {
            finish(RedirectStatus::NetworkError, m_reply->errorString());
        }
        return;
    }

    const int status = statusCode.toInt();
    const QByteArray location = m_reply->rawHeader("Location");
    dropReply();
    m_result.httpStatus = status;

    if (!m_usingGet && (status == 405 || status == 501)) {
        sendRequest(true);
    } else if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        finish(RedirectStatus::Resolved);
    } else if (m_result.hops >= MaxRedirects) {
        finish(RedirectStatus::TooManyRedirects, u"Stopped after %1 redirects"_s.arg(MaxRedirects));
    } else {
        followRedirect(location);
    }
}

void RedirectResolver::onTimeout()
{
    finish(RedirectStatus::Timeout, u"Timed out waiting for %1"_s.arg(m_currentUrl.host()));
}

void RedirectResolver::followRedirect(const QByteArray &location)
{
    const QByteArray trimmed = location.trimmed();
    const QUrl reference = QUrl::fromEncoded(trimmed);
    if (trimmed.isEmpty() || !reference.isValid()) {
        finish(RedirectStatus::InvalidRedirect, u"Redirect without a valid Location"_s);
        return;
    }

    QUrl target = m_currentUrl.resolved(reference);
    if (!target.isValid()) {
        finish(RedirectStatus::InvalidRedirect, u"Redirect without a valid Location"_s);
        return;
    }
    if (const QString reason = unsafeRedirectReason(m_currentUrl, target); !reason.isEmpty()) {
        finish(RedirectStatus::UnsafeRedirect, reason);
        return;
    }
    if (target.host().isEmpty()) {
        finish(RedirectStatus::InvalidRedirect, u"Redirect to a URL without host"_s);
        return;
    }
    // RFC 9110 section 10.2.2: a Location without fragment inherits the current one.
    if (!target.hasFragment() && m_currentUrl.hasFragment()) {
        target.setFragment(m_currentUrl.fragment(QUrl::FullyEncoded), QUrl::TolerantMode);
    }

    const UrlCleanResult next = clean(target.toString(QUrl::FullyEncoded));
    const QUrl nextUrl(next.url);
    const QString key = visitKey(nextUrl);
    if (m_visited.contains(key)) {
        finish(RedirectStatus::RedirectLoop, u"Redirect loop detected"_s);
        return;
    }
    m_visited.insert(key);
    m_current = next.url;
    m_currentUrl = nextUrl;
    m_result.removedCount += next.removedCount;
    ++m_result.hops;
    sendRequest(false);
}

UrlCleanResult RedirectResolver::clean(const QString &url) const
{
    return m_stripTracking ? stripTrackingParameters(url) : UrlCleanResult{url, 0};
}

void RedirectResolver::finish(RedirectStatus status, const QString &errorString)
{
    dropReply();
    m_active = false;
    m_result.finalUrl = m_current;
    m_result.status = status;
    m_result.error = status != RedirectStatus::NotRequested && status != RedirectStatus::Resolved;
    m_result.errorString = errorString;
    // Emit a copy: a handler may call start(), which resets m_result while later
    // handlers (and the restarting one's own arguments) still refer to it.
    const RedirectResult completed = m_result;
    Q_EMIT finished(completed);
}

void RedirectResolver::finishQueued(RedirectStatus status, const QString &errorString)
{
    QMetaObject::invokeMethod(
        this,
        [this, generation = m_generation, status, errorString] {
            if (generation == m_generation) {
                finish(status, errorString);
            }
        },
        Qt::QueuedConnection);
}

void RedirectResolver::dropReply()
{
    m_timer.stop();
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

} // namespace kbs
