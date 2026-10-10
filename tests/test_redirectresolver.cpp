#include "network/redirectresolver.h"

#include <QElapsedTimer>
#include <QHash>
#include <QNetworkProxy>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <utility>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using klinker::RedirectResolver;
using klinker::RedirectResult;
using klinker::RedirectStatus;

namespace
{

struct Response {
    int status = 200;
    QList<std::pair<QByteArray, QByteArray>> headers;
    QByteArray body;
    qint64 streamedBodySize = 0; // announced in Content-Length and streamed as filler
    bool stall = false; // never answer
};

Response respondWith(int status)
{
    Response response;
    response.status = status;
    return response;
}

Response redirect(int status, const QByteArray &location)
{
    Response response = respondWith(status);
    response.headers.append({"Location", location});
    return response;
}

struct RecordedRequest {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers; // lower-case names
};

// Minimal deterministic HTTP/1.1 server on the loopback interface. Routes match the
// request path without query, optionally per method; unknown paths answer 404.
class HttpFixture : public QObject
{
public:
    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &HttpFixture::accept);
        return m_server.listen(QHostAddress::LocalHost);
    }

    void reset()
    {
        m_routes.clear();
        requests.clear();
        streamedBytes = 0;
        disconnects = 0;
    }

    quint16 port() const
    {
        return m_server.serverPort();
    }

    QString url(const QByteArray &pathAndQuery) const
    {
        return u"http://127.0.0.1:%1"_s.arg(m_server.serverPort()) + QString::fromLatin1(pathAndQuery);
    }

    void on(const QByteArray &path, const Response &response)
    {
        m_routes.insert(path, response);
    }

    void on(const QByteArray &method, const QByteArray &path, const Response &response)
    {
        m_routes.insert(method + ' ' + path, response);
    }

    QList<QByteArray> targets() const
    {
        QList<QByteArray> result;
        for (const auto &request : requests) {
            result.append(request.target);
        }
        return result;
    }

    QList<RecordedRequest> requests;
    qint64 streamedBytes = 0;
    int disconnects = 0;

private:
    void accept()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            socket->setParent(this);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { process(socket); });
            connect(socket, &QTcpSocket::bytesWritten, this, [this, socket](qint64 written) {
                if (m_streaming.contains(socket)) {
                    streamedBytes += written;
                    pump(socket);
                }
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                ++disconnects;
                m_buffers.remove(socket);
                m_streaming.remove(socket);
                socket->deleteLater();
            });
        }
    }

    void process(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        qsizetype end;
        while ((end = buffer.indexOf("\r\n\r\n")) >= 0) {
            const QList<QByteArray> lines = buffer.first(end).split('\n');
            buffer.remove(0, end + 4);

            RecordedRequest request;
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            request.method = requestLine.value(0);
            request.target = requestLine.value(1);
            for (qsizetype i = 1; i < lines.size(); ++i) {
                const qsizetype colon = lines[i].indexOf(':');
                if (colon > 0) {
                    request.headers.insert(lines[i].first(colon).trimmed().toLower(), lines[i].sliced(colon + 1).trimmed());
                }
            }
            requests.append(request);
            respond(socket, request);
        }
    }

    void respond(QTcpSocket *socket, const RecordedRequest &request)
    {
        const QByteArray path = request.target.split('?').first();
        const Response response = m_routes.value(request.method + ' ' + path, m_routes.value(path, respondWith(404)));
        if (response.stall) {
            return;
        }
        const bool streamed = response.streamedBodySize > 0;
        QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\n";
        for (const auto &[name, value] : response.headers) {
            head += name + ": " + value + "\r\n";
        }
        head += "Content-Length: " + QByteArray::number(streamed ? response.streamedBodySize : response.body.size()) + "\r\n\r\n";
        socket->write(head);
        if (request.method == "HEAD") {
            return;
        }
        if (streamed) {
            m_streaming.insert(socket, response.streamedBodySize);
            pump(socket);
        } else {
            socket->write(response.body);
        }
    }

    void pump(QTcpSocket *socket)
    {
        static const QByteArray filler(64 * 1024, 'x');
        qint64 &remaining = m_streaming[socket];
        while (remaining > 0 && socket->bytesToWrite() < 1024 * 1024) {
            const qint64 chunk = std::min<qint64>(remaining, filler.size());
            socket->write(filler.constData(), chunk);
            remaining -= chunk;
        }
    }

    QTcpServer m_server;
    QHash<QByteArray, Response> m_routes;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, qint64> m_streaming;
};

// Throwaway self-signed certificate for 127.0.0.1, used only to prove that untrusted
// certificates are rejected.
constexpr char s_selfSignedCertificate[] = R"(-----BEGIN CERTIFICATE-----
MIIBkTCCATagAwIBAgIUKVVEqSWB0wqCcr/RpKbJJnTuEZcwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJMTI3LjAuMC4xMCAXDTI2MTAwOTA5MjI0MloYDzIxMjYwOTE1
MDkyMjQyWjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAATOeyokepD0Xc+Bs0+1WgEp9yy2iN522NCyCj3zapUT9bkX8tDkXOga
vBSGiRd1pn+mpAPqljLqwpwdbYWhV2+Do2QwYjAdBgNVHQ4EFgQUzFm6e/LtBMvu
q4iCJCUNQzsigiIwHwYDVR0jBBgwFoAUzFm6e/LtBMvuq4iCJCUNQzsigiIwDwYD
VR0TAQH/BAUwAwEB/zAPBgNVHREECDAGhwR/AAABMAoGCCqGSM49BAMCA0kAMEYC
IQDymR/o8ECgIzkwmviz5cxgwv71uY6XmQWBFlldTU4WHgIhAOYz/666L4ynqhsh
5EouPGnZXdk0/gAZ9PQR3d9dqfTF
-----END CERTIFICATE-----
)";
constexpr char s_selfSignedKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgkOTj8cbnR5FBnz2A
x/6SZ0F2KPwo137E+w46ZGB5ayKhRANCAATOeyokepD0Xc+Bs0+1WgEp9yy2iN52
2NCyCj3zapUT9bkX8tDkXOgavBSGiRd1pn+mpAPqljLqwpwdbYWhV2+D
-----END PRIVATE KEY-----
)";

// A loopback URL nobody listens on.
QString refusedUrl(const QByteArray &path)
{
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    const quint16 port = server.serverPort();
    server.close();
    return u"http://127.0.0.1:%1"_s.arg(port) + QString::fromLatin1(path);
}

Response stalled()
{
    Response response;
    response.stall = true;
    return response;
}

RedirectResult resultAt(const QSignalSpy &spy, qsizetype index = 0)
{
    return spy.at(index).at(0).value<RedirectResult>();
}

} // namespace

class TestRedirectResolver : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Never let an environment proxy intercept loopback requests.
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QVERIFY(m_http.listen());
    }

    void init()
    {
        m_http.reset();
    }

    void disabledUnwrapPreservesRawUrlWithoutNetwork_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("served url with tracking") << m_http.url("/a?utm_source=x&b=1#f");
        QTest::newRow("encoded octets") << u"https://example.com/a%2Fb%20c?q=%E2%82%AC%2B&x=a+b#frag%20x"_s;
        QTest::newRow("unicode") << u"https://bücher.example/straße?q=ä#ö"_s;
        QTest::newRow("non-http") << u"mailto:a@example.com?utm_source=x"_s;
        QTest::newRow("garbage") << u"not a url"_s;
    }

    void disabledUnwrapPreservesRawUrlWithoutNetwork()
    {
        QFETCH(QString, url);
        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);

        resolver.start(url, false, false);
        QCOMPARE(spy.count(), 0); // queued, so a caller can connect after start()
        QVERIFY(resolver.isActive());
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.originalUrl, url);
        QCOMPARE(result.finalUrl, url);
        QCOMPARE(result.status, RedirectStatus::NotRequested);
        QCOMPARE(result.hops, 0);
        QCOMPARE(result.removedCount, 0);
        QVERIFY(!result.error);
        QVERIFY(!resolver.isActive());
        QTest::qWait(50);
        QCOMPARE(spy.count(), 1);
        QVERIFY(m_http.requests.isEmpty());
    }

    void disabledUnwrapStillStripsTracking()
    {
        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        const QString url = m_http.url("/a?utm_source=x&b=%41&fbclid=1#f?utm_medium=y");

        resolver.start(url, true, false);
        QCOMPARE(spy.count(), 0);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.originalUrl, url);
        QCOMPARE(result.finalUrl, m_http.url("/a?b=%41#f?utm_medium=y"));
        QCOMPARE(result.removedCount, 2);
        QCOMPARE(result.status, RedirectStatus::NotRequested);
        QVERIFY(!result.error);
        QVERIFY(m_http.requests.isEmpty());
    }

    void unsupportedOriginalUrlsAreNeverRequested_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("ftp") << u"ftp://127.0.0.1/"_s;
        QTest::newRow("mailto") << u"mailto:a@example.com"_s;
        QTest::newRow("javascript") << u"javascript:alert(1)"_s;
        QTest::newRow("file") << u"file:///etc/passwd"_s;
        QTest::newRow("user and password") << m_http.url("/a").replace(u"//"_s, u"//user:pw@"_s);
        QTest::newRow("user only") << m_http.url("/a").replace(u"//"_s, u"//user@"_s);
        QTest::newRow("no host") << u"https:///path"_s;
        QTest::newRow("relative") << u"/just/a/path"_s;
        QTest::newRow("garbage") << u"not a url"_s;
        QTest::newRow("empty") << QString();
    }

    void unsupportedOriginalUrlsAreNeverRequested()
    {
        QFETCH(QString, url);
        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);

        resolver.start(url, true, true);
        QCOMPARE(spy.count(), 0);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.originalUrl, url);
        QCOMPARE(result.finalUrl, url);
        QCOMPARE(result.status, RedirectStatus::UnsupportedUrl);
        QVERIFY(result.error);
        QVERIFY(!result.errorString.isEmpty());
        QTest::qWait(50);
        QVERIFY(m_http.requests.isEmpty());
    }

    void plainResponseKeepsRawUrlAndSendsOneHead()
    {
        m_http.on("/plain", respondWith(200));
        const QString url = m_http.url("/plain?q=%E2%82%AC+x&b=1&b=1&utm_source=x#Frag%20");
        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);

        resolver.start(url, false, true);
        QVERIFY(resolver.isActive());
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.originalUrl, url);
        QCOMPARE(result.finalUrl, url);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(result.hops, 0);
        QCOMPARE(result.httpStatus, 200);
        QVERIFY(!result.error);
        QVERIFY(!resolver.isActive());
        QCOMPARE(m_http.requests.size(), 1);
        QCOMPARE(m_http.requests[0].method, "HEAD");
        QCOMPARE(m_http.requests[0].target, "/plain?q=%E2%82%AC+x&b=1&b=1&utm_source=x"); // fragment never sent
    }

    void redirectChainIsFollowed_data()
    {
        QTest::addColumn<int>("redirects");
        QTest::addColumn<int>("code");
        QTest::newRow("0") << 0 << 0;
        QTest::newRow("1 via 301") << 1 << 301;
        QTest::newRow("1 via 302") << 1 << 302;
        QTest::newRow("1 via 303") << 1 << 303;
        QTest::newRow("1 via 307") << 1 << 307;
        QTest::newRow("1 via 308") << 1 << 308;
        QTest::newRow("2 via 302") << 2 << 302;
        QTest::newRow("3 via 301") << 3 << 301;
    }

    void redirectChainIsFollowed()
    {
        QFETCH(int, redirects);
        QFETCH(int, code);
        QList<QByteArray> expectedTargets;
        for (int i = 0; i < redirects; ++i) {
            m_http.on("/hop" + QByteArray::number(i), redirect(code, m_http.url("/hop" + QByteArray::number(i + 1)).toLatin1()));
            expectedTargets.append("/hop" + QByteArray::number(i));
        }
        m_http.on("/hop" + QByteArray::number(redirects), respondWith(200));
        expectedTargets.append("/hop" + QByteArray::number(redirects));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/hop0"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(result.hops, redirects);
        QCOMPARE(result.finalUrl, m_http.url("/hop" + QByteArray::number(redirects)));
        QCOMPARE(result.httpStatus, 200);
        QVERIFY(!result.error);
        QCOMPARE(m_http.targets(), expectedTargets);
        for (const auto &request : std::as_const(m_http.requests)) {
            QCOMPARE(request.method, "HEAD");
        }
    }

    void fourthRedirectIsNotFollowed()
    {
        for (int i = 0; i < 5; ++i) {
            m_http.on("/hop" + QByteArray::number(i), redirect(302, "/hop" + QByteArray::number(i + 1)));
        }
        m_http.on("/hop5", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/hop0"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::TooManyRedirects);
        QVERIFY(result.error);
        QVERIFY(!result.errorString.isEmpty());
        QCOMPARE(result.hops, RedirectResolver::MaxRedirects);
        QCOMPARE(result.finalUrl, m_http.url("/hop3"));
        QCOMPARE(result.httpStatus, 302);
        QCOMPARE(m_http.targets(), (QList<QByteArray>{"/hop0", "/hop1", "/hop2", "/hop3"}));
    }

    void relativeLocationIsResolved_data()
    {
        QTest::addColumn<QByteArray>("location");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("absolute path") << QByteArray("/abs?x=1") << QByteArray("/abs?x=1");
        QTest::newRow("sibling") << QByteArray("next") << QByteArray("/dir/next");
        QTest::newRow("dot sibling") << QByteArray("./sib") << QByteArray("/dir/sib");
        QTest::newRow("parent") << QByteArray("../up?y=2") << QByteArray("/up?y=2");
        QTest::newRow("network path") << ("//127.0.0.1:" + m_http.url({}).section(u':', 2).toLatin1() + "/net") << QByteArray("/net");
        QTest::newRow("surrounding whitespace") << QByteArray("  /abs  ") << QByteArray("/abs");
    }

    void relativeLocationIsResolved()
    {
        QFETCH(QByteArray, location);
        QFETCH(QByteArray, expected);
        m_http.on("/dir/start", redirect(302, location));
        for (const QByteArray path : {"/abs", "/dir/next", "/dir/sib", "/up", "/net"}) {
            m_http.on(path, respondWith(204));
        }

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/dir/start"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(result.finalUrl, m_http.url(expected));
        QCOMPARE(result.hops, 1);
        QCOMPARE(result.httpStatus, 204);
        QCOMPARE(m_http.targets(), (QList<QByteArray>{"/dir/start", expected}));
    }

    void nonRedirectResponsesEndTheChain_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<QByteArray>("extraHeader");
        QTest::newRow("300") << 300 << QByteArray("Location");
        QTest::newRow("304") << 304 << QByteArray("Location");
        QTest::newRow("305") << 305 << QByteArray("Location");
        QTest::newRow("306") << 306 << QByteArray("Location");
        QTest::newRow("200 with Location") << 200 << QByteArray("Location");
        QTest::newRow("200 with Refresh") << 200 << QByteArray("Refresh");
        QTest::newRow("401 auth challenge") << 401 << QByteArray("WWW-Authenticate");
        QTest::newRow("404") << 404 << QByteArray("Location");
        QTest::newRow("500") << 500 << QByteArray("Location");
    }

    void nonRedirectResponsesEndTheChain()
    {
        QFETCH(int, code);
        QFETCH(QByteArray, extraHeader);
        Response response = respondWith(code);
        const QByteArray value = extraHeader == "Refresh" ? "0; url=/other" : extraHeader == "WWW-Authenticate" ? "Basic realm=\"x\"" : "/other";
        response.headers.append({extraHeader, value});
        m_http.on("/start", response);
        m_http.on("/other", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/start?k=v"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QVERIFY(!result.error);
        QCOMPARE(result.finalUrl, m_http.url("/start?k=v"));
        QCOMPARE(result.hops, 0);
        QCOMPARE(result.httpStatus, code);
        QCOMPARE(m_http.targets(), QList<QByteArray>{"/start?k=v"});
    }

    void redirectLoopIsDetected_data()
    {
        QTest::addColumn<QByteArray>("start");
        QTest::addColumn<bool>("strip");
        QTest::addColumn<QByteArray>("expectedFinal");
        QTest::addColumn<int>("expectedHops");
        QTest::addColumn<QList<QByteArray>>("expectedTargets");
        QTest::newRow("self") << QByteArray("/self") << false << QByteArray("/self") << 0 << QList<QByteArray>{"/self"};
        QTest::newRow("self with other fragment") << QByteArray("/selffrag") << false << QByteArray("/selffrag") << 0 << QList<QByteArray>{"/selffrag"};
        QTest::newRow("two step") << QByteArray("/a") << false << QByteArray("/b") << 1 << QList<QByteArray>{"/a", "/b"};
        QTest::newRow("only tracking differs")
            << QByteArray("/track?utm_source=1") << true << QByteArray("/track") << 0 << QList<QByteArray>{"/track"};
    }

    void redirectLoopIsDetected()
    {
        QFETCH(QByteArray, start);
        QFETCH(bool, strip);
        QFETCH(QByteArray, expectedFinal);
        QFETCH(int, expectedHops);
        QFETCH(QList<QByteArray>, expectedTargets);
        m_http.on("/self", redirect(301, "/self"));
        m_http.on("/selffrag", redirect(301, "/selffrag#elsewhere"));
        m_http.on("/a", redirect(302, "/b"));
        m_http.on("/b", redirect(302, m_http.url("/a").toLatin1()));
        m_http.on("/track", redirect(302, "/track?utm_source=2"));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url(start), strip, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::RedirectLoop);
        QVERIFY(result.error);
        QVERIFY(!result.errorString.isEmpty());
        QCOMPARE(result.finalUrl, m_http.url(expectedFinal));
        QCOMPARE(result.hops, expectedHops);
        QCOMPARE(m_http.targets(), expectedTargets);
    }

    void badLocationKeepsLastSafeUrl_data()
    {
        QTest::addColumn<bool>("hasLocation");
        QTest::addColumn<QByteArray>("location");
        QTest::addColumn<RedirectStatus>("status");
        const QByteArray port = QByteArray::number(m_http.port());
        QTest::newRow("missing") << false << QByteArray() << RedirectStatus::InvalidRedirect;
        QTest::newRow("empty") << true << QByteArray() << RedirectStatus::InvalidRedirect;
        QTest::newRow("broken ipv6") << true << QByteArray("http://[::1/x") << RedirectStatus::InvalidRedirect;
        QTest::newRow("port out of range") << true << QByteArray("http://127.0.0.1:99999/") << RedirectStatus::InvalidRedirect;
        QTest::newRow("no host") << true << QByteArray("http:///x") << RedirectStatus::InvalidRedirect;
        QTest::newRow("javascript") << true << QByteArray("javascript:alert(1)") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("file") << true << QByteArray("file:///etc/passwd") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("ftp") << true << QByteArray("ftp://127.0.0.1/x") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("data") << true << QByteArray("data:text/html,<b>x</b>") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("app scheme") << true << QByteArray("intent://x#Intent;end") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("user and password") << true << ("http://user:pw@127.0.0.1:" + port + "/ok") << RedirectStatus::UnsafeRedirect;
        QTest::newRow("user only") << true << ("http://user@127.0.0.1:" + port + "/ok") << RedirectStatus::UnsafeRedirect;
    }

    void badLocationKeepsLastSafeUrl()
    {
        QFETCH(bool, hasLocation);
        QFETCH(QByteArray, location);
        QFETCH(RedirectStatus, status);
        m_http.on("/first", redirect(307, "/start?a=1#keep"));
        m_http.on("/start", hasLocation ? redirect(302, location) : respondWith(302));
        m_http.on("/ok", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/first"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, status);
        QVERIFY(result.error);
        QVERIFY(!result.errorString.isEmpty());
        QCOMPARE(result.finalUrl, m_http.url("/start?a=1#keep"));
        QCOMPARE(result.hops, 1);
        QCOMPARE(result.httpStatus, 302);
        QCOMPARE(m_http.targets(), (QList<QByteArray>{"/first", "/start?a=1"}));
    }

    void unsafeRedirectReason_data()
    {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("to");
        QTest::addColumn<bool>("unsafe");
        QTest::newRow("http to http") << u"http://a.example/"_s << u"http://b.example/"_s << false;
        QTest::newRow("http to https") << u"http://a.example/"_s << u"https://b.example/"_s << false;
        QTest::newRow("https to https") << u"https://a.example/"_s << u"https://b.example/x"_s << false;
        QTest::newRow("https to http") << u"https://a.example/"_s << u"http://a.example/"_s << true;
        QTest::newRow("https to HTTP") << u"https://a.example/"_s << u"HTTP://a.example/"_s << true;
        QTest::newRow("https to ftp") << u"https://a.example/"_s << u"ftp://a.example/"_s << true;
        QTest::newRow("credentials") << u"https://a.example/"_s << u"https://u:p@a.example/"_s << true;
        QTest::newRow("password only") << u"https://a.example/"_s << u"https://:p@a.example/"_s << true;
    }

    void unsafeRedirectReason()
    {
        QFETCH(QString, from);
        QFETCH(QString, to);
        QFETCH(bool, unsafe);
        QCOMPARE(!klinker::unsafeRedirectReason(QUrl(from), QUrl(to)).isEmpty(), unsafe);
    }

    void trackingIsStrippedBeforeEveryRequest_data()
    {
        QTest::addColumn<bool>("strip");
        QTest::addColumn<QList<QByteArray>>("expectedTargets");
        QTest::addColumn<QByteArray>("expectedFinal");
        QTest::addColumn<int>("expectedRemoved");
        QTest::newRow("on") << true << QList<QByteArray>{"/start?keep=1", "/next?k=2", "/last"} << QByteArray("/last#f?utm_source=frag") << 4;
        QTest::newRow("off") << false << QList<QByteArray>{"/start?utm_source=a&keep=1", "/next?fbclid=b&k=2&utm_medium=c", "/last?gclid=z"}
                             << QByteArray("/last?gclid=z#f?utm_source=frag") << 0;
    }

    void trackingIsStrippedBeforeEveryRequest()
    {
        QFETCH(bool, strip);
        QFETCH(QList<QByteArray>, expectedTargets);
        QFETCH(QByteArray, expectedFinal);
        QFETCH(int, expectedRemoved);
        m_http.on("/start", redirect(302, "/next?fbclid=b&k=2&utm_medium=c"));
        m_http.on("/next", redirect(301, m_http.url("/last?gclid=z").toLatin1()));
        m_http.on("/last", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/start?utm_source=a&keep=1#f?utm_source=frag"), strip, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(m_http.targets(), expectedTargets);
        QCOMPARE(result.finalUrl, m_http.url(expectedFinal));
        QCOMPARE(result.removedCount, expectedRemoved);
        QCOMPARE(result.hops, 2);
    }

    void fragmentsFollowRedirectSemantics_data()
    {
        QTest::addColumn<QByteArray>("startFragment");
        QTest::addColumn<QByteArray>("expectedFinal");
        QTest::newRow("inherited then replaced then inherited") << QByteArray("#orig") << QByteArray("/d#loc");
        QTest::newRow("none") << QByteArray() << QByteArray("/d#loc");
    }

    void fragmentsFollowRedirectSemantics()
    {
        QFETCH(QByteArray, startFragment);
        QFETCH(QByteArray, expectedFinal);
        m_http.on("/a", redirect(302, "/b"));
        m_http.on("/b", redirect(302, "c#loc"));
        m_http.on("/c", redirect(302, "/d"));
        m_http.on("/d", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/a?x=1" + startFragment), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(result.finalUrl, m_http.url(expectedFinal));
        QCOMPARE(m_http.targets(), (QList<QByteArray>{"/a?x=1", "/b", "/c", "/d"}));
    }

    void encodedFragmentIsInheritedVerbatim()
    {
        m_http.on("/a", redirect(302, "/b"));
        m_http.on("/b", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/a#x%20y%23z"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        QCOMPARE(resultAt(spy).finalUrl, m_http.url("/b#x%20y%23z"));
    }

    void headRefusalFallsBackToGet_data()
    {
        QTest::addColumn<int>("headStatus");
        QTest::addColumn<int>("getStatus");
        QTest::addColumn<QByteArray>("getLocation");
        QTest::addColumn<QByteArray>("getBody");
        QTest::addColumn<QByteArray>("expectedFinal");
        QTest::addColumn<int>("expectedStatus");
        QTest::addColumn<QList<QByteArray>>("expectedRequests");
        const QList<QByteArray> fallbackOnly{"HEAD /start", "GET /start"};
        const QList<QByteArray> fallbackThenHead{"HEAD /start", "GET /start", "HEAD /next"};
        QTest::newRow("405 then 200") << 405 << 200 << QByteArray() << QByteArray("ok") << QByteArray("/start") << 200 << fallbackOnly;
        QTest::newRow("501 then 206") << 501 << 206 << QByteArray() << QByteArray("o") << QByteArray("/start") << 206 << fallbackOnly;
        QTest::newRow("405 then 405") << 405 << 405 << QByteArray() << QByteArray() << QByteArray("/start") << 405 << fallbackOnly;
        QTest::newRow("405 then redirect") << 405 << 302 << QByteArray("/next") << QByteArray() << QByteArray("/next") << 200 << fallbackThenHead;
        QTest::newRow("meta refresh body ignored") << 405 << 200 << QByteArray()
                                                   << QByteArray("<meta http-equiv=\"refresh\" content=\"0;url=/next\"><script>location='/next'</script>")
                                                   << QByteArray("/start") << 200 << fallbackOnly;
    }

    void headRefusalFallsBackToGet()
    {
        QFETCH(int, headStatus);
        QFETCH(int, getStatus);
        QFETCH(QByteArray, getLocation);
        QFETCH(QByteArray, getBody);
        QFETCH(QByteArray, expectedFinal);
        QFETCH(int, expectedStatus);
        QFETCH(QList<QByteArray>, expectedRequests);
        m_http.on("HEAD", "/start", respondWith(headStatus));
        Response get = getLocation.isEmpty() ? respondWith(getStatus) : redirect(getStatus, getLocation);
        get.body = getBody;
        m_http.on("GET", "/start", get);
        m_http.on("/next", respondWith(200));

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/start"), false, true);
        QTRY_COMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QCOMPARE(result.finalUrl, m_http.url(expectedFinal));
        QCOMPARE(result.httpStatus, expectedStatus);
        QList<QByteArray> requests;
        for (const auto &request : std::as_const(m_http.requests)) {
            requests.append(request.method + ' ' + request.target);
            if (request.method == "GET") {
                QCOMPARE(request.headers.value("range"), "bytes=0-0");
            }
        }
        QCOMPARE(requests, expectedRequests);
    }

    void fallbackGetStopsAtHeaders()
    {
        constexpr qint64 bodySize = 1024LL * 1024 * 1024;
        Response huge = respondWith(200);
        huge.streamedBodySize = bodySize;
        m_http.on("HEAD", "/big", respondWith(405));
        m_http.on("GET", "/big", huge);

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        QElapsedTimer elapsed;
        elapsed.start();
        resolver.start(m_http.url("/big"), false, true);
        QTRY_COMPARE(spy.count(), 1);
        QVERIFY2(elapsed.elapsed() < 3000, QByteArray::number(elapsed.elapsed()).constData());

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Resolved);
        QVERIFY(!result.error); // the deliberate abort is not a network error
        QCOMPARE(result.httpStatus, 200);
        QCOMPARE(result.finalUrl, m_http.url("/big"));

        // The connection is torn down instead of draining the body.
        QTRY_VERIFY(m_http.disconnects >= 1);
        QTest::qWait(200);
        qInfo("server sent %lld body bytes before the client aborted", m_http.streamedBytes);
        QVERIFY2(m_http.streamedBytes < 64 * 1024 * 1024, QByteArray::number(m_http.streamedBytes).constData());
    }

    void requestsCarryNoCookiesReferrerOrCredentials()
    {
        Response setsCookie = redirect(302, "/b");
        setsCookie.headers.append({QByteArray("Set-Cookie"), QByteArray("session=secret; Path=/")});
        m_http.on("/a", setsCookie);
        Response challenge = respondWith(401);
        challenge.headers.append({QByteArray("WWW-Authenticate"), QByteArray("Basic realm=\"x\"")});
        m_http.on("/b", challenge);

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/a"), false, true);
        QTRY_COMPARE(spy.count(), 1);
        resolver.start(m_http.url("/a"), false, true); // nothing persists between runs
        QTRY_COMPARE(spy.count(), 2);

        QCOMPARE(resultAt(spy, 1).httpStatus, 401);
        QCOMPARE(m_http.requests.size(), 4);
        for (const auto &request : std::as_const(m_http.requests)) {
            QVERIFY2(!request.headers.contains("cookie"), request.target.constData());
            QVERIFY(!request.headers.contains("referer"));
            QVERIFY(!request.headers.contains("authorization"));
            QVERIFY(!request.headers.contains("proxy-authorization"));
        }
    }

    void connectionFailureIsNetworkError()
    {
        const QString deadUrl = refusedUrl("/dead?x=1#f");
        m_http.on("/a", redirect(302, deadUrl.toLatin1()));

        for (const QString &start : {deadUrl, m_http.url("/a")}) {
            RedirectResolver resolver;
            QSignalSpy spy(&resolver, &RedirectResolver::finished);
            resolver.start(start, false, true);
            QTRY_COMPARE(spy.count(), 1);

            const RedirectResult result = resultAt(spy);
            QCOMPARE(result.status, RedirectStatus::NetworkError);
            QVERIFY(result.error);
            QVERIFY(!result.errorString.isEmpty());
            QCOMPARE(result.finalUrl, deadUrl); // the last safe URL, not the original
            QCOMPARE(result.hops, start == deadUrl ? 0 : 1);
        }
    }

    void untrustedCertificateIsRejected()
    {
        if (!QSslSocket::supportsSsl()) {
            QSKIP("No TLS backend");
        }
        QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
        tls.setLocalCertificate(QSslCertificate(QByteArray(s_selfSignedCertificate)));
        tls.setPrivateKey(QSslKey(QByteArray(s_selfSignedKey), QSsl::Ec));
        QVERIFY(!tls.localCertificate().isNull());
        QVERIFY(!tls.privateKey().isNull());
        QSslServer server;
        server.setSslConfiguration(tls);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        bool handshakeStarted = false;
        connect(&server, &QSslServer::startedEncryptionHandshake, this, [&handshakeStarted] { handshakeStarted = true; });

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        const QString url = u"https://127.0.0.1:%1/secure"_s.arg(server.serverPort());
        resolver.start(url, false, true);
        QTRY_COMPARE(spy.count(), 1);

        QVERIFY(handshakeStarted);
        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::NetworkError);
        QVERIFY(!result.errorString.isEmpty());
        QCOMPARE(result.finalUrl, url);
    }

    void stalledRequestTimesOut()
    {
        m_http.on("/stall", stalled());
        m_http.on("/a", redirect(302, "/stall"));

        RedirectResolver resolver(200ms);
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        QElapsedTimer elapsed;
        elapsed.start();
        resolver.start(m_http.url("/a"), false, true);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QVERIFY(elapsed.elapsed() >= 150);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.status, RedirectStatus::Timeout);
        QVERIFY(result.error);
        QVERIFY(!result.errorString.isEmpty());
        QCOMPARE(result.finalUrl, m_http.url("/stall"));
        QCOMPARE(result.hops, 1);
        QTRY_VERIFY(m_http.disconnects >= 1); // the request was abandoned
    }

    // A stalled server must not hold up the chooser for long by default.
    void defaultRequestTimeoutIsTwoSeconds()
    {
        m_http.on("/stall", stalled());

        RedirectResolver resolver;
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        QElapsedTimer elapsed;
        elapsed.start();
        resolver.start(m_http.url("/stall"), false, true);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3500);

        QVERIFY2(elapsed.elapsed() >= 1900, QByteArray::number(elapsed.elapsed()).constData());
        QCOMPARE(resultAt(spy).status, RedirectStatus::Timeout);
        QCOMPARE(resultAt(spy).finalUrl, m_http.url("/stall"));
    }

    void totalDeadlineBoundsTheWholeRun()
    {
        m_http.on("/stall", stalled());

        RedirectResolver resolver(10s, 300ms);
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        QElapsedTimer elapsed;
        elapsed.start();
        resolver.start(m_http.url("/stall"), false, true);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);

        QVERIFY2(elapsed.elapsed() < 2000, QByteArray::number(elapsed.elapsed()).constData());
        QCOMPARE(resultAt(spy).status, RedirectStatus::Timeout);
    }

    void cancelSuppressesCompletion_data()
    {
        QTest::addColumn<bool>("unwrap");
        QTest::newRow("pending request") << true;
        QTest::newRow("queued no-network result") << false;
    }

    void cancelSuppressesCompletion()
    {
        QFETCH(bool, unwrap);
        m_http.on("/stall", stalled());

        RedirectResolver resolver(100ms);
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/stall"), false, unwrap);
        if (unwrap) {
            QTRY_COMPARE(m_http.requests.size(), 1);
        }
        resolver.cancel();
        QVERIFY(!resolver.isActive());
        resolver.cancel(); // idempotent

        QTest::qWait(400); // past the request timeout too
        QCOMPARE(spy.count(), 0);
        if (unwrap) {
            QTRY_VERIFY(m_http.disconnects >= 1);
        }
    }

    void restartSupersedesPreviousRun_data()
    {
        QTest::addColumn<bool>("firstUnwraps");
        QTest::newRow("pending request") << true;
        QTest::newRow("queued no-network result") << false;
    }

    void restartSupersedesPreviousRun()
    {
        QFETCH(bool, firstUnwraps);
        m_http.on("/stall", stalled());
        m_http.on("/fast", redirect(301, "/done"));
        m_http.on("/done", respondWith(200));

        RedirectResolver resolver(150ms);
        QSignalSpy spy(&resolver, &RedirectResolver::finished);
        resolver.start(m_http.url("/stall"), false, firstUnwraps);
        resolver.start(m_http.url("/fast"), false, true);
        QTRY_COMPARE(spy.count(), 1);
        QTest::qWait(400);
        QCOMPARE(spy.count(), 1);

        const RedirectResult result = resultAt(spy);
        QCOMPARE(result.originalUrl, m_http.url("/fast"));
        QCOMPARE(result.finalUrl, m_http.url("/done"));
        QCOMPARE(result.status, RedirectStatus::Resolved);
    }

    void canRestartFromFinishedHandler()
    {
        m_http.on("/a", redirect(302, "/b"));
        m_http.on("/b", respondWith(200));

        RedirectResolver resolver;
        QList<RedirectResult> results;
        connect(&resolver, &RedirectResolver::finished, this, [&](const RedirectResult &result) {
            results.append(result);
            if (results.size() == 1) {
                resolver.start(m_http.url("/b"), false, true);
            }
        });
        resolver.start(m_http.url("/a"), false, true);
        QTRY_COMPARE(results.size(), 2);
        QTest::qWait(100);
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].finalUrl, m_http.url("/b"));
        QCOMPARE(results[1].originalUrl, m_http.url("/b"));
        QCOMPARE(results[1].hops, 0);
    }

    void restartWithFinalUrlKeepsResultsIntact_data()
    {
        QTest::addColumn<bool>("unwrap");
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("finalUrl");
        QTest::addColumn<int>("firstHops");
        QTest::newRow("queued no-network result") << false << u"https://example.com/path"_s << u"https://example.com/path"_s << 0;
        QTest::newRow("resolved redirect") << true << m_http.url("/a") << m_http.url("/b") << 1;
    }

    // A handler restarting with result.finalUrl must neither clear its own input nor
    // change the completed result seen by subscribers connected after it.
    void restartWithFinalUrlKeepsResultsIntact()
    {
        QFETCH(bool, unwrap);
        QFETCH(QString, url);
        QFETCH(QString, finalUrl);
        QFETCH(int, firstHops);
        m_http.on("/a", redirect(302, "/b"));
        m_http.on("/b", respondWith(200));
        const RedirectStatus status = unwrap ? RedirectStatus::Resolved : RedirectStatus::NotRequested;

        RedirectResolver resolver;
        QList<RedirectResult> restarterSaw;
        connect(&resolver, &RedirectResolver::finished, this, [&](const RedirectResult &result) {
            if (restarterSaw.isEmpty()) {
                resolver.start(result.finalUrl, false, unwrap);
            }
            restarterSaw.append(result); // after the restart, to catch aliasing
        });
        QList<RedirectResult> laterSaw;
        connect(&resolver, &RedirectResolver::finished, this, [&](const RedirectResult &result) {
            laterSaw.append(result);
        });

        resolver.start(url, false, unwrap);
        QTRY_COMPARE(laterSaw.size(), 2);
        QTest::qWait(100);
        QCOMPARE(restarterSaw.size(), 2);
        QCOMPARE(laterSaw.size(), 2);

        for (const QList<RedirectResult> &seen : {restarterSaw, laterSaw}) {
            QCOMPARE(seen[0].originalUrl, url);
            QCOMPARE(seen[0].finalUrl, finalUrl);
            QCOMPARE(seen[0].status, status);
            QCOMPARE(seen[0].hops, firstHops);
            QVERIFY(!seen[0].error);

            QCOMPARE(seen[1].originalUrl, finalUrl);
            QCOMPARE(seen[1].finalUrl, finalUrl);
            QCOMPARE(seen[1].status, status);
            QCOMPARE(seen[1].hops, 0);
            QVERIFY(!seen[1].error);
        }
        QVERIFY(!resolver.isActive());
    }

private:
    HttpFixture m_http;
};

QTEST_GUILESS_MAIN(TestRedirectResolver)
#include "test_redirectresolver.moc"
