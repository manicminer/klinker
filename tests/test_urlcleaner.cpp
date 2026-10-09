#include "core/urlcleaner.h"

#include <QTest>

using namespace Qt::StringLiterals;
using kbs::stripTrackingParameters;

class TestUrlCleaner : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void untouchedUrlsArePreservedExactly_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("plain") << u"https://example.com/"_s;
        QTest::newRow("encoded octets") << u"https://example.com/a%2Fb%20c?q=%E2%82%AC%2B&x=a+b#frag%20x"_s;
        QTest::newRow("duplicates and order") << u"https://example.com/?b=2&a=1&b=3&a=1"_s;
        QTest::newRow("empty separators") << u"https://example.com/?&&a=1&&"_s;
        QTest::newRow("empty query") << u"https://example.com/?"_s;
        QTest::newRow("generic ref and id kept") << u"https://example.com/?ref=home&id=42&source=x"_s;
        QTest::newRow("lookalike names") << u"https://example.com/?utm=1&xutm_source=2&gclid_x=3&fbclid2=4"_s;
        QTest::newRow("uppercase not stripped") << u"https://example.com/?UTM_SOURCE=x"_s;
        QTest::newRow("tracking in fragment only") << u"https://example.com/#/page?utm_source=x"_s;
        QTest::newRow("non-http scheme") << u"mailto:a@example.com?utm_source=x"_s;
        QTest::newRow("ftp scheme") << u"ftp://example.com/?utm_source=x"_s;
        QTest::newRow("unicode") << u"https://bücher.example/straße?q=ä#ö"_s;
        QTest::newRow("shell metacharacters") << u"https://example.com/?a=$(id)&b=`x`;c|d"_s;
    }

    void untouchedUrlsArePreservedExactly()
    {
        QFETCH(QString, url);
        const auto result = stripTrackingParameters(url);
        QCOMPARE(result.url, url);
        QCOMPARE(result.removedCount, 0);
    }

    void trackingParametersAreRemoved_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("removed");

        QTest::newRow("only tracking") << u"https://example.com/p?utm_source=a&utm_medium=b"_s << u"https://example.com/p"_s << 2;
        QTest::newRow("only tracking keeps fragment") << u"https://example.com/p?fbclid=x#top"_s << u"https://example.com/p#top"_s << 1;
        QTest::newRow("mixed keeps order and encoding")
            << u"https://example.com/?b=%2F&utm_campaign=x&a=1+2&gclid=y&c=%E2%82%AC#f?utm_source=z"_s
            << u"https://example.com/?b=%2F&a=1+2&c=%E2%82%AC#f?utm_source=z"_s << 2;
        QTest::newRow("duplicate kept params") << u"https://example.com/?a=1&utm_source=x&a=1&a=2"_s << u"https://example.com/?a=1&a=1&a=2"_s << 1;
        QTest::newRow("duplicate tracking params") << u"https://example.com/?utm_source=x&k=v&utm_source=y"_s << u"https://example.com/?k=v"_s << 2;
        QTest::newRow("empty components kept between") << u"https://example.com/?a=1&&utm_source=x&b=2"_s << u"https://example.com/?a=1&&b=2"_s << 1;
        QTest::newRow("only empty components left") << u"https://example.com/?&utm_source=x&"_s << u"https://example.com/?&"_s << 1;
        QTest::newRow("one empty component left") << u"https://example.com/?utm_source=x&#f"_s << u"https://example.com/?#f"_s << 1;
        QTest::newRow("empty component before tracking") << u"https://example.com/p?&gclid=1"_s << u"https://example.com/p?"_s << 1;
        QTest::newRow("encoded name") << u"https://example.com/?utm%5Fsource=x&a=1"_s << u"https://example.com/?a=1"_s << 1;
        QTest::newRow("encoded name lowercase hex") << u"https://example.com/?%66bclid=x&a=1"_s << u"https://example.com/?a=1"_s << 1;
        QTest::newRow("name without value") << u"https://example.com/?a&msclkid"_s << u"https://example.com/?a"_s << 1;
        QTest::newRow("empty value") << u"https://example.com/?mc_cid=&mc_eid=&z="_s << u"https://example.com/?z="_s << 2;
        QTest::newRow("uppercase scheme") << u"HTTPS://example.com/?dclid=1"_s << u"HTTPS://example.com/"_s << 1;
        QTest::newRow("http") << u"http://example.com/?utm_term=x&q=1"_s << u"http://example.com/?q=1"_s << 1;
        QTest::newRow("value containing equals") << u"https://example.com/?next=/a?b=c&utm_id=1"_s << u"https://example.com/?next=/a?b=c"_s << 1;
    }

    void trackingParametersAreRemoved()
    {
        QFETCH(QString, url);
        QFETCH(QString, expected);
        QFETCH(int, removed);
        const auto result = stripTrackingParameters(url);
        QCOMPARE(result.url, expected);
        QCOMPARE(result.removedCount, removed);
    }

    void trackingNameList_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("tracking");
        for (const auto name : {u"utm_source", u"utm_medium", u"utm_campaign", u"utm_term", u"utm_content", u"utm_whatever", u"gclid",
                                u"dclid", u"gbraid", u"wbraid", u"gclsrc", u"fbclid", u"msclkid", u"mc_cid", u"mc_eid", u"yclid",
                                u"twclid", u"ttclid", u"igshid", u"_hsenc", u"_hsmi", u"mkt_tok"}) {
            QTest::newRow(qPrintable(QString(name))) << QString(name) << true;
        }
        for (const auto name : {u"ref", u"id", u"source", u"utm", u"utm_", u"q", u"page", u"session", u"token", u"si", u"s"}) {
            QTest::newRow(qPrintable(QString(name))) << QString(name) << false;
        }
    }

    void trackingNameList()
    {
        QFETCH(QString, name);
        QFETCH(bool, tracking);
        QCOMPARE(kbs::isTrackingParameter(name), tracking);
    }
};

QTEST_GUILESS_MAIN(TestUrlCleaner)
#include "test_urlcleaner.moc"
