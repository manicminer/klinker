#include "ui/placement.h"

#include <QTest>

using namespace Qt::StringLiterals;
using kbs::Placement;
using kbs::PlacementMode;
using kbs::PlacementRequest;
using kbs::ScreenInfo;

namespace
{

// The desktop this was developed on: a 4K monitor and a laptop panel to its
// right, offset downwards, each with a 44 px bottom panel.
QList<ScreenInfo> twoScreens()
{
    return {
        {u"DP-9"_s, QRect(0, 0, 3840, 2160), QRect(0, 0, 3840, 2116)},
        {u"eDP-1"_s, QRect(3840, 473, 2560, 1600), QRect(3840, 473, 2560, 1556)},
    };
}

// A monitor left of and above the primary one gives negative coordinates.
QList<ScreenInfo> negativeOrigin()
{
    return {
        {u"HDMI-1"_s, QRect(-1920, -300, 1920, 1080), QRect(-1920, -300, 1920, 1040)},
        {u"DP-1"_s, QRect(0, 0, 2560, 1440), QRect(0, 0, 2560, 1440)},
    };
}

PlacementRequest request(QSize size, std::optional<QPoint> cursor, const QList<ScreenInfo> &screens)
{
    PlacementRequest r;
    r.preferredSize = size;
    r.cursor = cursor;
    r.screens = screens;
    r.primaryOutput = screens.isEmpty() ? QString() : screens.first().name;
    r.margin = 8;
    return r;
}

const QSize Card(430, 360);

} // namespace

class TestPlacement : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void cursorInTheMiddleAnchorsTheCard()
    {
        const Placement p = kbs::planPlacement(request(Card, QPoint(1000, 800), twoScreens()));
        QCOMPARE(p.mode, PlacementMode::NearCursor);
        QCOMPARE(p.screenName, u"DP-9"_s);
        QCOMPARE(p.geometry.size(), Card);
        QCOMPARE(p.geometry.topLeft(), QPoint(1000 - Card.width() / 2, 800 - kbs::CursorOffsetY));
    }

    void clampsToEveryEdge_data()
    {
        QTest::addColumn<QPoint>("cursor");
        QTest::addColumn<QPoint>("expectedTopLeft");
        // eDP-1 available area: x 3840..6399, y 473..2028; margin 8.
        QTest::newRow("top-left") << QPoint(3840, 473) << QPoint(3848, 481);
        QTest::newRow("top-right") << QPoint(6399, 473) << QPoint(6400 - 8 - 430, 481);
        QTest::newRow("bottom-left") << QPoint(3841, 2027) << QPoint(3848, 2029 - 8 - 360);
        QTest::newRow("bottom-right") << QPoint(6399, 2027) << QPoint(6400 - 8 - 430, 2029 - 8 - 360);
        QTest::newRow("in bottom panel") << QPoint(5000, 2070) << QPoint(5000 - 215, 2029 - 8 - 360);
    }

    void clampsToEveryEdge()
    {
        QFETCH(QPoint, cursor);
        QFETCH(QPoint, expectedTopLeft);
        const Placement p = kbs::planPlacement(request(Card, cursor, twoScreens()));
        QCOMPARE(p.screenName, u"eDP-1"_s);
        QCOMPARE(p.geometry.topLeft(), expectedTopLeft);
        QVERIFY(twoScreens().at(1).available.contains(p.geometry));
    }

    void negativeOriginsStayOnTheirScreen()
    {
        const auto screens = negativeOrigin();
        const Placement topLeft = kbs::planPlacement(request(Card, QPoint(-1920, -300), screens));
        QCOMPARE(topLeft.screenName, u"HDMI-1"_s);
        QCOMPARE(topLeft.geometry.topLeft(), QPoint(-1912, -292));

        // Just left of the shared edge: must not spill onto DP-1.
        const Placement edge = kbs::planPlacement(request(Card, QPoint(-1, 100), screens));
        QCOMPARE(edge.screenName, u"HDMI-1"_s);
        QCOMPARE(edge.geometry.right(), -1 - 8);
        QVERIFY(screens.at(0).available.contains(edge.geometry));
    }

    void originIsAValidCursorPosition()
    {
        const Placement p = kbs::planPlacement(request(Card, QPoint(0, 0), negativeOrigin()));
        QCOMPARE(p.mode, PlacementMode::NearCursor);
        QCOMPARE(p.screenName, u"DP-1"_s);
        QCOMPARE(p.geometry.topLeft(), QPoint(8, 8));
    }

    void unknownCursorCentresOnActiveOutput()
    {
        PlacementRequest r = request(Card, std::nullopt, twoScreens());
        r.activeOutput = u"eDP-1"_s;
        const Placement p = kbs::planPlacement(r);
        QCOMPARE(p.mode, PlacementMode::Centered);
        QCOMPARE(p.screenName, u"eDP-1"_s);
        QCOMPARE(p.geometry, QRect(QPoint(3840 + (2560 - 430) / 2, 473 + (1556 - 360) / 2), Card));
    }

    void unknownActiveOutputFallsBackToPrimary()
    {
        PlacementRequest r = request(Card, std::nullopt, twoScreens());
        r.activeOutput = u"HDMI-7"_s; // stale or unknown name
        r.primaryOutput = u"eDP-1"_s;
        QCOMPARE(kbs::planPlacement(r).screenName, u"eDP-1"_s);

        r.primaryOutput.clear();
        QCOMPARE(kbs::planPlacement(r).screenName, u"DP-9"_s);
    }

    void cursorInAGapBetweenScreensCentresInstead()
    {
        // eDP-1 starts at y 473, so (5000, 100) is on no screen.
        PlacementRequest r = request(Card, QPoint(5000, 100), twoScreens());
        r.activeOutput = u"eDP-1"_s;
        const Placement p = kbs::planPlacement(r);
        QCOMPARE(p.mode, PlacementMode::Centered);
        QCOMPARE(p.screenName, u"eDP-1"_s);
    }

    void smallOutputShrinksTheCard()
    {
        const QList<ScreenInfo> tiny{{u"VIRT"_s, QRect(-100, -50, 400, 300), QRect(-100, -50, 400, 280)}};
        const Placement nearCursor = kbs::planPlacement(request(Card, QPoint(0, 0), tiny));
        QCOMPARE(nearCursor.geometry, QRect(-92, -42, 384, 264));

        const Placement centred = kbs::planPlacement(request(Card, std::nullopt, tiny));
        QCOMPARE(centred.geometry, QRect(-92, -42, 384, 264));
    }

    void outputSmallerThanTheMarginsStillGetsAWindow()
    {
        const QList<ScreenInfo> silly{{u"X"_s, QRect(0, 0, 12, 10), QRect(0, 0, 12, 10)}};
        const Placement p = kbs::planPlacement(request(Card, QPoint(3, 3), silly));
        QCOMPARE(p.geometry, QRect(0, 0, 12, 10));
    }

    void noScreensIsInvalid()
    {
        QVERIFY(!kbs::planPlacement(request(Card, QPoint(1, 1), {})).isValid());
    }

    void maximumSizeIsTheUsableArea()
    {
        QCOMPARE(kbs::maximumSize(QRect(-100, -50, 400, 280), 8), QSize(384, 264));
        QCOMPARE(kbs::maximumSize(QRect(0, 0, 12, 10), 8), QSize(12, 10));
    }

    void cursorIsOnlyTrustedWhereThePlatformReportsIt()
    {
        QCOMPARE(kbs::reliableCursorPosition(u"xcb"_s, QPoint(0, 0)), std::optional<QPoint>(QPoint(0, 0)));
        QCOMPARE(kbs::reliableCursorPosition(u"xcb"_s, QPoint(-5, 7)), std::optional<QPoint>(QPoint(-5, 7)));
        QCOMPARE(kbs::reliableCursorPosition(u"wayland"_s, QPoint(10, 10)), std::nullopt);
        QCOMPARE(kbs::reliableCursorPosition(u"wayland-egl"_s, QPoint(10, 10)), std::nullopt);
        QCOMPARE(kbs::reliableCursorPosition(u"offscreen"_s, QPoint(10, 10)), std::nullopt);
    }
};

QTEST_GUILESS_MAIN(TestPlacement)

#include "test_placement.moc"
