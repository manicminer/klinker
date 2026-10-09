#pragma once

#include <QDeadlineTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "core/urlcleaner.h"

#include <chrono>

class QNetworkReply;

namespace kbs
{

enum class RedirectStatus {
    NotRequested, // unwrapping disabled: no network access happened
    Resolved, // a non-redirect response (any status code) ended the chain
    UnsupportedUrl, // the original URL is not a credential-free http(s) URL
    TooManyRedirects,
    RedirectLoop,
    InvalidRedirect, // missing or unparsable Location
    UnsafeRedirect, // Location with another scheme, credentials or an https -> http downgrade
    NetworkError, // includes TLS failures
    Timeout,
};

struct RedirectResult {
    QString originalUrl; // exactly as passed to start()
    QString finalUrl; // URL to launch: the last safe URL reached, cleaned when requested
    int hops = 0; // redirects followed
    int removedCount = 0; // tracking parameters removed over all URLs
    int httpStatus = 0; // last HTTP status received, 0 if none
    RedirectStatus status = RedirectStatus::NotRequested;
    bool error = false;
    QString errorString;
};

// Reason why following a redirect from `from` to `to` would be unsafe; empty when allowed.
QString unsafeRedirectReason(const QUrl &from, const QUrl &to);

// Resolves at most MaxRedirects HTTP redirects with HEAD requests (falling back to a
// header-only GET on 405/501), stripping tracking parameters before every request when
// asked. Exactly one finished() is emitted per start(), always asynchronously, unless the
// run is cancelled or superseded by another start().
class RedirectResolver : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxRedirects = 3;

    explicit RedirectResolver(std::chrono::milliseconds requestTimeout = std::chrono::seconds(5),
                              std::chrono::milliseconds totalTimeout = std::chrono::seconds(15),
                              QObject *parent = nullptr);
    ~RedirectResolver() override;

    void start(const QString &originalUrl, bool stripTracking, bool unwrapRedirects);
    void cancel();
    bool isActive() const;

Q_SIGNALS:
    void finished(const kbs::RedirectResult &result);

private:
    void sendRequest(bool useGet);
    void onReplyProgress();
    void onTimeout();
    void followRedirect(const QByteArray &location);
    UrlCleanResult clean(const QString &url) const;
    void finish(RedirectStatus status, const QString &errorString = {});
    void finishQueued(RedirectStatus status, const QString &errorString = {});
    void dropReply();

    QNetworkAccessManager m_network;
    QTimer m_timer;
    std::chrono::milliseconds m_requestTimeout;
    std::chrono::milliseconds m_totalTimeout;
    QDeadlineTimer m_deadline;
    QPointer<QNetworkReply> m_reply;
    quint64 m_generation = 0;
    bool m_active = false;
    bool m_usingGet = false;
    bool m_stripTracking = false;
    QString m_current;
    QUrl m_currentUrl;
    QSet<QString> m_visited;
    RedirectResult m_result;
};

} // namespace kbs
