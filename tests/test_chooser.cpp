#include "testsupport.h"
#include "ui/chooserwindow.h"
#include "ui/placement.h"
#include "ui/theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QListView>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QWindow>

#ifdef KLINKER_HAVE_LAYERSHELLQT
#include <LayerShellQt/Window>
#endif

#include <cmath>
#include <optional>

using namespace Qt::StringLiterals;
using klinker::ChooserEntry;
using klinker::ChooserWindow;

namespace
{

QList<ChooserEntry> entries(int count)
{
    static const QStringList names{u"Firefox"_s, u"Chromium"_s, u"Vivaldi"_s, u"Brave"_s, u"Konqueror"_s, u"Falkon"_s,
                                   u"LibreWolf"_s, u"Tor Browser"_s, u"Epiphany"_s, u"qutebrowser"_s, u"Opera"_s, u"Zen"_s};
    static const QStringList icons{u"firefox"_s, u"chromium"_s, u"vivaldi"_s, u"brave-browser"_s, u"konqueror"_s, u"falkon"_s};
    QList<ChooserEntry> result;
    for (int i = 0; i < count; ++i) {
        const QString name = i < names.size() ? names[i] : u"Browser %1"_s.arg(i + 1);
        result.append({u"b%1.desktop"_s.arg(i + 1), name, icons.value(i, u"internet-web-browser"_s),
                       u"%1 · Detected"_s.arg(name.toLower().remove(u' '))});
    }
    return result;
}

const QString Url = u"https://example.com/articles/2024/a-very-long-path/that-goes-on/and-on?page=2&sort=asc#comments"_s;

klinker::ChooserSettings horizontal(int iconSize = 48, bool names = true)
{
    return {klinker::ChooserStyle::Horizontal, iconSize, names, false};
}

// The browsers of the reported chooser, whose names fill their tiles.
QList<ChooserEntry> reportedEntries()
{
    QList<ChooserEntry> result;
    for (const QString &name : {u"ChatGPT"_s, u"Chromium"_s, u"Falkon"_s, u"Firefox"_s, u"LibreWolf"_s, u"Switchyard"_s}) {
        result.append({name.toLower() + u".desktop"_s, name, u"internet-web-browser"_s, name.toLower() + u" · Detected"_s});
    }
    return result;
}

// Widget styles the sizing is checked with: Breeze, the default, Oxygen, whose
// scroll bars are outside the frame and so shrink the contents rectangle, and
// Fusion. Rows for styles that are not installed are skipped.
const QStringList Styles{u"Breeze"_s, u"Oxygen"_s, u"Fusion"_s};

klinker::ChooserSettings closingOnFocusLoss()
{
    klinker::ChooserSettings preferences;
    preferences.closeOnFocusLoss = true;
    return preferences;
}

struct Fixture {
    ChooserWindow window;
    QListView *list = nullptr;

    // The list gets keyboard focus on its own; tests do not give it focus.
    explicit Fixture(int count = 3, bool busy = false, const klinker::ChooserSettings &preferences = {})
    {
        window.setPreferences(preferences);
        window.setOriginalUrl(Url);
        if (busy) {
            window.setBusy(u"Following redirects…"_s);
        } else {
            window.setFinalUrl(Url, u"Opened as received."_s);
        }
        window.setBrowsers(entries(count));
        window.resize(window.preferredSize());
        window.show();
        list = window.findChild<QListView *>(u"browserList"_s);
        window.activateWindow();
        if (!QTest::qWaitForWindowActive(&window)) {
            qWarning("chooser window did not become active");
        }
    }

    QRect rowRect(int row) const
    {
        return list->visualRect(list->model()->index(row, 0));
    }
};

// Sizes, places and shows the window as Application::showChooser() does.
void showAsTheApplication(ChooserWindow &window)
{
    klinker::placePopup(&window, [&window](const QSize &maximum) {
        return window.preferredSize(maximum);
    });
    window.show();
    window.raise();
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window)) {
        qWarning("chooser window did not become active");
    }
}

QSize screenMaximum(const QWidget *window)
{
    return klinker::maximumSize(window->screen()->availableGeometry(), 8);
}

// Sends a key to the window as the platform would, so it reaches whichever
// widget has keyboard focus (QTest::keyClick(QWidget *) bypasses focus). With
// accessibility active (a real session), the AT-SPI bridge delivers it
// asynchronously, so check its effect with QTRY_*.
void press(QWidget *window, Qt::Key key)
{
    QTest::keyClick(window->windowHandle(), key);
}

// A widget is reachable when it is inside the window, or inside a scroll area
// (other than the browser list) whose viewport is inside the window and whose
// scroll range covers it. Either way it must not be squeezed below its height.
bool reachable(QWidget *widget, QWidget *window, QString *why)
{
    const QRect windowRect = window->rect();
    const int needed = widget->hasHeightForWidth() ? widget->heightForWidth(widget->width()) : widget->minimumSizeHint().height();
    if (widget->height() < needed) {
        *why = u"%1 is %2 px high but needs %3"_s.arg(widget->objectName()).arg(widget->height()).arg(needed);
        return false;
    }
    for (QWidget *parent = widget->parentWidget(); parent && parent != window; parent = parent->parentWidget()) {
        auto *area = qobject_cast<QScrollArea *>(parent);
        if (!area) {
            continue;
        }
        const QRect viewport(area->viewport()->mapTo(window, QPoint()), area->viewport()->size());
        if (!windowRect.contains(viewport)) {
            *why = u"scroll area of %1 is outside the window"_s.arg(widget->objectName());
            return false;
        }
        QWidget *content = area->widget();
        if (!content) {
            *why = u"scroll area of %1 has no content"_s.arg(widget->objectName());
            return false;
        }
        const int bottom = widget->mapTo(content, QPoint(0, widget->height())).y();
        const int range = area->verticalScrollBar()->maximum() + area->viewport()->height();
        if (bottom > range || bottom > content->height()) {
            *why = u"%1 ends at %2 but the scroll range ends at %3"_s.arg(widget->objectName()).arg(bottom).arg(range);
            return false;
        }
        return true;
    }
    const QRect rect(widget->mapTo(window, QPoint()), widget->size());
    if (!windowRect.contains(rect)) {
        *why = u"%1 at %2,%3 %4x%5 is outside the %6x%7 window"_s.arg(widget->objectName())
                   .arg(rect.x())
                   .arg(rect.y())
                   .arg(rect.width())
                   .arg(rect.height())
                   .arg(windowRect.width())
                   .arg(windowRect.height());
        return false;
    }
    return true;
}

double luminance(const QColor &color)
{
    const auto channel = [](float c) {
        return c <= 0.04045f ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const QColor rgb = color.toRgb();
    return 0.2126 * channel(rgb.redF()) + 0.7152 * channel(rgb.greenF()) + 0.0722 * channel(rgb.blueF());
}

// WCAG 2 contrast ratio.
double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Wayland tests that start another application only run in the private,
// nested KWin of tests/wayland-smoke.sh, never on a desktop.
bool inPrivateWaylandSession(QString *why)
{
    if (!QGuiApplication::platformName().startsWith(u"wayland"_s)) {
        *why = u"needs QT_QPA_PLATFORM=wayland"_s;
        return false;
    }
    if (qEnvironmentVariableIsEmpty("KLINKER_SMOKE_RUNTIME_DIR") || qEnvironmentVariable("WAYLAND_DISPLAY") != u"klinker-smoke"_s) {
        *why = u"only runs in the private KWin of tests/wayland-smoke.sh"_s;
        return false;
    }
    return true;
}

// Another application with a window (tests/fixtures/focusfixture.cpp), in the
// same private Wayland session.
class FocusFixture
{
public:
    ~FocusFixture()
    {
        stop();
    }

    bool start()
    {
        m_process.setProcessChannelMode(QProcess::SeparateChannels);
        m_process.start(QStringLiteral(KLINKER_FOCUS_FIXTURE), {u"15000"_s});
        return m_process.waitForStarted(5000);
    }

    // It printed that it has the keyboard focus.
    bool focused()
    {
        m_output += m_process.readAllStandardOutput();
        return m_output.contains("focused");
    }

    void stop()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(5000);
        }
    }

private:
    QProcess m_process;
    QByteArray m_output;
};

// Keeps WindowActivate events from a widget.
class ActivationBlocker : public QObject
{
public:
    int blocked = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::WindowActivate) {
            ++blocked;
            return true;
        }
        return false;
    }
};

template<typename T>
T *child(QWidget *parent, const QString &name)
{
    T *widget = parent->findChild<T *>(name);
    if (!widget) {
        qWarning() << "missing child" << name;
    }
    return widget;
}

} // namespace

class TestChooser : public QObject
{
    Q_OBJECT

public:
    static void initMain()
    {
        klinker::test::isolateEnvironment();
    }

private Q_SLOTS:
    void initTestCase()
    {
        klinker::test::applyTheme(false);
    }

    void listsBrowsersWithShortcutChips()
    {
        Fixture f(3);
        QVERIFY(f.list);
        QCOMPARE(f.list->model()->rowCount(), 3);
        QCOMPARE(f.window.displayedBrowserIds(), (QStringList{u"b1.desktop"_s, u"b2.desktop"_s, u"b3.desktop"_s}));
        const QModelIndex first = f.list->model()->index(0, 0);
        QCOMPARE(first.data(Qt::DisplayRole).toString(), u"Firefox"_s);
        QCOMPARE(first.data(klinker::BrowserListModel::ShortcutRole).toInt(), 1);
        QVERIFY(first.data(Qt::AccessibleTextRole).toString().contains(u"Firefox"_s));
        QVERIFY(first.data(Qt::AccessibleTextRole).toString().contains(u"1"_s));
        QVERIFY(!f.list->accessibleName().isEmpty());
        QCOMPARE(f.window.windowTitle(), u"Open link with — Klinker"_s);
    }

    void numberKeysChooseTheVisibleRow()
    {
        Fixture f(12);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClick(f.list, Qt::Key_1);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b1.desktop"_s);

        Fixture g(12);
        QSignalSpy chosen9(&g.window, &ChooserWindow::browserChosen);
        QTest::keyClick(g.list, Qt::Key_9);
        QCOMPARE(chosen9.size(), 1);
        QCOMPARE(chosen9.takeFirst().at(0).toString(), u"b9.desktop"_s);
        // Rows beyond nine have no number.
        QCOMPARE(g.list->model()->index(9, 0).data(klinker::BrowserListModel::ShortcutRole).toInt(), 0);
    }

    void numberKeyBeyondTheListDoesNothing()
    {
        Fixture f(2);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClick(f.list, Qt::Key_3);
        QTest::keyClick(f.list, Qt::Key_0);
        QCOMPARE(chosen.size(), 0);
    }

    void mouseClickChooses()
    {
        Fixture f(3);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, {}, f.rowRect(1).center());
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b2.desktop"_s);
    }

    void arrowsAndEnterChoose()
    {
        Fixture f(3);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClick(f.list, Qt::Key_Down);
        QTest::keyClick(f.list, Qt::Key_Down);
        QTest::keyClick(f.list, Qt::Key_Up);
        QTest::keyClick(f.list, Qt::Key_Return);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b2.desktop"_s);
    }

    void escapeCancels()
    {
        Fixture f(3);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClick(f.list, Qt::Key_Escape);
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(chosen.size(), 0);
    }

    void typingFiltersAndDigitsAreTextWhileEditing()
    {
        Fixture f(6);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        auto *filter = child<QLineEdit>(&f.window, u"filterEdit"_s);
        QVERIFY(filter);
        QVERIFY(filter->isHidden());

        QTest::keyClicks(f.list, u"o"_s);
        QVERIFY(filter->isVisible());
        QVERIFY(filter->hasFocus());
        QCOMPARE(filter->text(), u"o"_s);
        // "o" matches Firefox, Chromium, Konqueror and Falkon, but not Vivaldi or Brave.
        QCOMPARE(f.window.displayedBrowserIds(), (QStringList{u"b1.desktop"_s, u"b2.desktop"_s, u"b5.desktop"_s, u"b6.desktop"_s}));

        // A digit typed into the filter is text, never a launch.
        QTest::keyClick(filter, Qt::Key_2);
        QCOMPARE(chosen.size(), 0);
        QCOMPARE(filter->text(), u"o2"_s);
        QVERIFY(f.window.displayedBrowserIds().isEmpty());
        QTest::keyClick(filter, Qt::Key_Backspace);

        // Down moves into the list, where digits choose among the filtered rows.
        QTest::keyClick(filter, Qt::Key_Down);
        QVERIFY(f.list->hasFocus());
        QCOMPARE(f.list->model()->index(1, 0).data(klinker::BrowserListModel::ShortcutRole).toInt(), 2);
        QTest::keyClick(f.list, Qt::Key_3);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b5.desktop"_s);
    }

    void escapeClearsTheFilterBeforeCancelling()
    {
        Fixture f(6);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        auto *filter = child<QLineEdit>(&f.window, u"filterEdit"_s);
        QTest::keyClicks(f.list, u"fire"_s);
        QCOMPARE(f.window.displayedBrowserIds(), QStringList{u"b1.desktop"_s});
        QTest::keyClick(filter, Qt::Key_Escape);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(filter->isHidden());
        QCOMPARE(f.window.displayedBrowserIds().size(), 6);
        QVERIFY(f.list->hasFocus());
        QTest::keyClick(f.list, Qt::Key_Escape);
        QCOMPARE(cancelled.size(), 1);
    }

    void enterInTheFilterChoosesTheFirstMatch()
    {
        Fixture f(6);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClicks(f.list, u"kon"_s);
        QTest::keyClick(child<QLineEdit>(&f.window, u"filterEdit"_s), Qt::Key_Return);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b5.desktop"_s);
    }

    void busyBlocksChoosingUntilTheFinalUrlIsKnown()
    {
        Fixture f(3);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        f.window.setBusy(u"Following redirects…"_s);
        QVERIFY(f.window.isBusy());
        QVERIFY(!f.list->isEnabled());
        auto *busy = child<QWidget>(&f.window, u"busyRow"_s);
        QVERIFY(busy && busy->isVisible());
        QTest::keyClick(&f.window, Qt::Key_1);
        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, {}, f.rowRect(0).center());
        QCOMPARE(chosen.size(), 0);

        f.window.setFinalUrl(u"https://example.org/final"_s, u"Followed 2 redirects."_s);
        QVERIFY(!f.window.isBusy());
        QVERIFY(f.list->isEnabled());
        QVERIFY(busy->isHidden());
        QVERIFY(f.list->hasFocus());
        press(&f.window, Qt::Key_1);
        QTRY_COMPARE(chosen.size(), 1);
    }

    void theListHasKeyboardFocusWhenShown()
    {
        Fixture f(3);
        QVERIFY(QApplication::activeWindow() == &f.window);
        QCOMPARE(QApplication::focusWidget(), f.list);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        press(&f.window, Qt::Key_Down);
        press(&f.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b2.desktop"_s);
    }

    // Focus moves away from the list while it is disabled; the keyboard must
    // work again as soon as the link is ready, without a click.
    void theListGetsFocusBackWhenTheLinkIsReady_data()
    {
        QTest::addColumn<bool>("shownBusy");
        QTest::addColumn<Qt::Key>("key");
        QTest::addColumn<QString>("expected");
        QTest::newRow("busy before showing, Enter") << true << Qt::Key_Return << u"b2.desktop"_s;
        QTest::newRow("busy before showing, 3") << true << Qt::Key_3 << u"b3.desktop"_s;
        QTest::newRow("busy while shown, Enter") << false << Qt::Key_Return << u"b2.desktop"_s;
        QTest::newRow("busy while shown, 3") << false << Qt::Key_3 << u"b3.desktop"_s;
    }

    void theListGetsFocusBackWhenTheLinkIsReady()
    {
        QFETCH(bool, shownBusy);
        QFETCH(Qt::Key, key);
        QFETCH(QString, expected);
        Fixture f(3, shownBusy);
        if (!shownBusy) {
            QCOMPARE(QApplication::focusWidget(), f.list);
            f.window.setBusy(u"Following redirects…"_s);
        }
        QVERIFY(!f.list->hasFocus());
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QSignalSpy settings(&f.window, &ChooserWindow::settingsRequested);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy skipped(&f.window, &ChooserWindow::resolutionSkipped);

        f.window.setFinalUrl(u"https://example.org/final"_s, u"Followed 1 redirect."_s);
        QCOMPARE(QApplication::focusWidget(), f.list);
        if (key == Qt::Key_Return) {
            press(&f.window, Qt::Key_Down);
        }
        press(&f.window, key);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), expected);
        QCOMPARE(settings.size(), 0);
        QCOMPARE(cancelled.size(), 0);
        QCOMPARE(skipped.size(), 0);
    }

    void skippingReturnsFocusToTheList()
    {
        Fixture f(3, true);
        auto *skip = child<QPushButton>(&f.window, u"skipButton"_s);
        connect(&f.window, &ChooserWindow::resolutionSkipped, &f.window, [&f] {
            f.window.setFinalUrl(Url, u"Skipped."_s);
        });
        QTest::mouseClick(skip, Qt::LeftButton);
        QVERIFY(!f.window.isBusy());
        QCOMPARE(QApplication::focusWidget(), f.list);
    }

    // A control the user moved to while waiting keeps the focus.
    void readinessDoesNotTakeFocusFromAChosenControl()
    {
        Fixture f(3, true);
        auto *details = child<QToolButton>(&f.window, u"detailsButton"_s);
        for (int i = 0; i < 10 && QApplication::focusWidget() != details; ++i) {
            QWidget *before = QApplication::focusWidget();
            press(&f.window, Qt::Key_Tab);
            QTRY_VERIFY(QApplication::focusWidget() != before);
        }
        QCOMPARE(QApplication::focusWidget(), details);
        f.window.setFinalUrl(u"https://example.org/final"_s, u"Followed 1 redirect."_s);
        QCOMPARE(QApplication::focusWidget(), details);
    }

    void skipEndsResolutionOnRequest()
    {
        Fixture f(3);
        f.window.setBusy(u"Following redirects…"_s);
        QSignalSpy skipped(&f.window, &ChooserWindow::resolutionSkipped);
        auto *skip = child<QPushButton>(&f.window, u"skipButton"_s);
        QVERIFY(skip && skip->isVisible());
        QTest::mouseClick(skip, Qt::LeftButton);
        QCOMPARE(skipped.size(), 1);
    }

    void urlsArePlainTextElidedAndCopyable()
    {
        Fixture f(2);
        const QString nasty = u"https://example.com/<b>bold</b>/&amp;/"_s + QString(400, u'x');
        f.window.setOriginalUrl(nasty);
        f.window.setFinalUrl(nasty, u"Opened as received."_s);
        auto *label = child<klinker::ElidedLabel>(&f.window, u"finalUrlLabel"_s);
        QVERIFY(label);
        QCOMPARE(label->text(), nasty);
        QCOMPARE(f.window.finalUrl(), nasty);
        QVERIFY(label->displayedText().contains(u'…'));
        QVERIFY(label->displayedText().startsWith(u"https://example.com/"_s));
        QVERIFY(label->displayedText().endsWith(u"xxx"_s));
        QCOMPARE(label->accessibleName(), nasty);

        auto *copy = child<QToolButton>(&f.window, u"copyUrlButton"_s);
        QVERIFY(copy);
        QVERIFY(!copy->accessibleName().isEmpty());
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), nasty);
    }

    void detailsDiscloseTheOriginalUrlAndRedirectStatus()
    {
        Fixture f(2);
        f.window.setOriginalUrl(u"https://t.example/r/abc"_s);
        f.window.setFinalUrl(u"https://example.org/target"_s, u"Followed 1 redirect."_s);
        auto *details = child<QToolButton>(&f.window, u"detailsButton"_s);
        auto *original = child<klinker::ElidedLabel>(&f.window, u"originalUrlLabel"_s);
        auto *status = child<QLabel>(&f.window, u"redirectStatusLabel"_s);
        QVERIFY(details && original && status);
        QVERIFY(!original->isVisible());
        QTest::mouseClick(details, Qt::LeftButton);
        QVERIFY(original->isVisible());
        QCOMPARE(original->text(), u"https://t.example/r/abc"_s);
        QCOMPARE(status->text(), u"Followed 1 redirect."_s);
        QCOMPARE(status->textFormat(), Qt::PlainText);

        // The two links are told apart by name, and each can be copied in full.
        QStringList captions;
        for (QLabel *label : f.window.findChild<QWidget *>(u"detailsPanel"_s)->findChildren<QLabel *>()) {
            captions.append(label->text());
        }
        QVERIFY2(captions.contains(u"Original"_s), qPrintable(captions.join(u", "_s)));
        QVERIFY(!captions.contains(u"Received"_s));
        QVERIFY(original->accessibleDescription().contains(u"Original"_s));
        auto *final = child<klinker::ElidedLabel>(&f.window, u"finalUrlLabel"_s);
        QVERIFY(final->accessibleDescription().contains(u"Destination"_s));
        QVERIFY(child<QToolButton>(&f.window, u"copyUrlButton"_s)->accessibleName().contains(u"destination"_s));

        auto *copyOriginal = child<QToolButton>(&f.window, u"copyOriginalButton"_s);
        QVERIFY(copyOriginal && copyOriginal->isVisible());
        QVERIFY(copyOriginal->accessibleName().contains(u"original"_s));
        QVERIFY(copyOriginal->focusPolicy() & Qt::TabFocus);
        QGuiApplication::clipboard()->clear();
        copyOriginal->setFocus(Qt::TabFocusReason);
        press(&f.window, Qt::Key_Space);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), u"https://t.example/r/abc"_s);
    }

    // The whole card, not only the browser list, has to fit small screens:
    // long errors and details scroll, and the footer stays reachable.
    void theWholeCardFitsASmallScreen_data()
    {
        QTest::addColumn<qreal>("fontScale");
        QTest::newRow("normal font") << 1.0;
        QTest::newRow("large font") << 1.5;
    }

    void theWholeCardFitsASmallScreen()
    {
        QFETCH(qreal, fontScale);
        // Like a larger font chosen in System Settings.
        const QFont normalFont = QApplication::font();
        const auto restoreFont = qScopeGuard([normalFont] {
            QApplication::setFont(normalFont);
        });
        QFont font = normalFont;
        font.setPointSizeF(font.pointSizeF() * fontScale);
        QApplication::setFont(font);
        ChooserWindow window;
        window.setOriginalUrl(u"https://l.example.net/?u="_s + QString(300, u'x'));
        window.setFinalUrl(Url, u"Followed 3 redirects.\nRemoved 2 tracking parameters.\nThe last request took a while."_s);
        window.setBrowsers(entries(6));
        window.showError(u"Could not start Vivaldi.\n"_s + QStringList(12, u"A long line of error output that wraps across the card."_s).join(u'\n'));

        const QRect small(0, 0, 400, 280);
        klinker::PlacementRequest request;
        request.preferredSize = window.preferredSize();
        request.screens = {{u"small"_s, small, small}};
        request.primaryOutput = u"small"_s;
        const klinker::Placement placement = klinker::planPlacement(request);
        QVERIFY(placement.isValid());
        QVERIFY(request.preferredSize.height() > placement.geometry.height()); // the content really does not fit
        window.resize(placement.geometry.size());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        child<QToolButton>(&window, u"detailsButton"_s)->click();
        window.resize(placement.geometry.size());
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), placement.geometry.size());
        QVERIFY(small.adjusted(8, 8, -8, -8).contains(placement.geometry));

        QString why;
        for (const QString &name : {u"heading"_s, u"finalUrlLabel"_s, u"copyUrlButton"_s, u"detailsButton"_s, u"originalUrlLabel"_s,
                                    u"redirectStatusLabel"_s, u"messageWidget"_s, u"settingsButton"_s, u"cancelButton"_s}) {
            QWidget *widget = child<QWidget>(&window, name);
            QVERIFY(widget && widget->isVisible());
            QVERIFY2(reachable(widget, &window, &why), qPrintable(why));
        }

        // At least one browser row shows, and the keyboard still chooses.
        auto *list = child<QListView>(&window, u"browserList"_s);
        QVERIFY(reachable(list, &window, &why));
        QVERIFY2(list->viewport()->height() >= list->sizeHintForRow(0),
                 qPrintable(u"list viewport %1, row %2"_s.arg(list->viewport()->height()).arg(list->sizeHintForRow(0))));
        QCOMPARE(QApplication::focusWidget(), list);
        QSignalSpy chosen(&window, &ChooserWindow::browserChosen);
        for (int i = 0; i < 5; ++i) {
            press(&window, Qt::Key_Down);
        }
        press(&window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b6.desktop"_s);
        // The message's text itself must have room for all of its lines.
        int wrapped = 0;
        for (QLabel *label : child<QWidget>(&window, u"messageWidget"_s)->findChildren<QLabel *>()) {
            if (label->isVisible() && label->wordWrap()) {
                ++wrapped;
                QVERIFY2(reachable(label, &window, &why), qPrintable(why));
            }
        }
        QCOMPARE(wrapped, 1);
        klinker::test::saveScreenshot(&window, u"chooser-small-screen-%1"_s.arg(fontScale > 1 ? u"large-font"_s : u"normal-font"_s));
    }

    // Ordinary content needs no scrolling at the preferred size.
    void ordinaryContentDoesNotScroll()
    {
        Fixture f(3);
        child<QToolButton>(&f.window, u"detailsButton"_s)->click();
        f.window.resize(f.window.preferredSize());
        QCoreApplication::processEvents();
        for (QScrollArea *area : f.window.findChildren<QScrollArea *>()) {
            QCOMPARE(area->verticalScrollBar()->maximum(), 0);
        }
        QString why;
        QVERIFY2(reachable(child<QWidget>(&f.window, u"cancelButton"_s), &f.window, &why), qPrintable(why));
    }

    void secondaryTextIsReadable_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    // Detail lines and number chips stay secondary but meet WCAG AA (4.5:1).
    void secondaryTextIsReadable()
    {
        QFETCH(bool, dark);
        klinker::test::applyTheme(dark);
        const QPalette palette = QApplication::palette();
        const double ratio = contrast(klinker::subduedTextColor(palette), palette.color(QPalette::Window));
        klinker::test::applyTheme(false);
        QVERIFY2(ratio >= 4.5, qPrintable(u"contrast %1:1"_s.arg(ratio, 0, 'f', 2)));
        QVERIFY2(ratio < contrast(palette.color(QPalette::WindowText), palette.color(QPalette::Window)),
                 "secondary text must stay less prominent than normal text");
    }

    void errorsArePersistentPlainText()
    {
        Fixture f(2);
        const QString error = u"Could not start <b>Firefox</b>: No such file"_s;
        auto *message = child<QWidget>(&f.window, u"messageWidget"_s);
        QVERIFY(message);
        QVERIFY(!message->isVisible());
        QVERIFY(f.window.messageText().isEmpty());
        f.window.showError(error);
        QVERIFY(message->isVisible());
        QCOMPARE(f.window.messageText(), error);
        // Shown literally, not as rich text, and named for assistive technology.
        const QList<QLabel *> labels = message->findChildren<QLabel *>();
        const auto shown = std::ranges::find(labels, error, &QLabel::text);
        QVERIFY(shown != labels.cend());
        QCOMPARE((*shown)->textFormat(), Qt::PlainText);
        QCOMPARE(message->accessibleName(), u"Error"_s);
        QCOMPARE(message->accessibleDescription(), error);
        // Refreshing the list keeps the error.
        f.window.setBrowsers(entries(3));
        QVERIFY(message->isVisible());
        QCOMPARE(f.window.messageText(), error);
        // ... and the chooser still works for a retry.
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QCOMPARE(QApplication::focusWidget(), f.list);
        press(&f.window, Qt::Key_1);
        QTRY_COMPARE(chosen.size(), 1);
        f.window.clearError();
        QVERIFY(!message->isVisible());
        QVERIFY(f.window.messageText().isEmpty());
    }

    void refreshedListHasNoStaleShortcuts()
    {
        Fixture f(3);
        QList<ChooserEntry> reordered = entries(3);
        reordered.removeAt(1);
        std::swap(reordered[0], reordered[1]);
        f.window.setBrowsers(reordered);
        QCOMPARE(f.window.displayedBrowserIds(), (QStringList{u"b3.desktop"_s, u"b1.desktop"_s}));
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QCOMPARE(QApplication::focusWidget(), f.list);
        press(&f.window, Qt::Key_1);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b3.desktop"_s);
        press(&f.window, Qt::Key_2);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b1.desktop"_s);
        // 3 chooses nothing: the next choice is the one made with 1.
        press(&f.window, Qt::Key_3);
        press(&f.window, Qt::Key_1);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b3.desktop"_s);
    }

    void emptyStateOffersSettings()
    {
        ChooserWindow window;
        window.setFinalUrl(Url, QString());
        window.setBrowsers({}, 2);
        window.resize(window.preferredSize());
        window.show();
        auto *empty = child<QWidget>(&window, u"emptyState"_s);
        auto *text = child<QLabel>(&window, u"emptyStateText"_s);
        QVERIFY(empty && empty->isVisible());
        QVERIFY(text->text().contains(u"hidden"_s));
        QVERIFY(!child<QListView>(&window, u"browserList"_s)->isVisible());

        QSignalSpy settings(&window, &ChooserWindow::settingsRequested);
        QTest::mouseClick(child<QPushButton>(&window, u"emptyStateSettingsButton"_s), Qt::LeftButton);
        QCOMPARE(settings.size(), 1);

        window.setBrowsers({}, 0);
        QVERIFY(text->text().contains(u"No web browsers"_s));
        klinker::test::saveScreenshot(&window, u"chooser-empty-light"_s);
    }

    void footerSettingsAndCancel()
    {
        Fixture f(2);
        QSignalSpy settings(&f.window, &ChooserWindow::settingsRequested);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        auto *settingsButton = child<QPushButton>(&f.window, u"settingsButton"_s);
        auto *cancelButton = child<QPushButton>(&f.window, u"cancelButton"_s);
        QVERIFY(settingsButton && cancelButton);
        QVERIFY(!settingsButton->accessibleName().isEmpty());
        QTest::mouseClick(settingsButton, Qt::LeftButton);
        QCOMPARE(settings.size(), 1);
        QTest::mouseClick(cancelButton, Qt::LeftButton);
        QCOMPARE(cancelled.size(), 1);
    }

    void closingTheWindowCancelsOnce()
    {
        Fixture f(2);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        f.window.close();
        QCOMPARE(cancelled.size(), 1);
    }

    void longListsScrollInsideSmallWindows()
    {
        Fixture f(30);
        QCOMPARE(f.window.preferredSize().width(), ChooserWindow::CardWidth);
        const QSize nine = f.window.preferredSize();
        Fixture g(9);
        QCOMPARE(g.window.preferredSize(), nine);

        f.window.resize(ChooserWindow::CardWidth, 260);
        QTRY_VERIFY(f.list->verticalScrollBar()->maximum() > 0);
        for (int i = 0; i < 25; ++i) {
            QTest::keyClick(f.list, Qt::Key_Down);
        }
        QVERIFY(f.list->viewport()->rect().intersects(f.rowRect(25)));
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QTest::keyClick(f.list, Qt::Key_Return);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b26.desktop"_s);
    }

    void preferredSizeFollowsTheFont()
    {
        Fixture f(4);
        const int normal = f.window.preferredSize().height();
        QFont large = f.window.font();
        large.setPointSizeF(large.pointSizeF() * 1.6);
        f.window.setFont(large);
        QVERIFY(f.window.preferredSize().height() > normal);
    }

    void screenshots_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void screenshots()
    {
        QFETCH(bool, dark);
        klinker::test::applyTheme(dark);
        const QString suffix = dark ? u"dark"_s : u"light"_s;
        {
            Fixture f(5);
            f.window.setOriginalUrl(u"https://l.example.net/?u=https%3A%2F%2Fkde.org%2Fannouncements%2F&utm_source=feed"_s);
            f.window.setFinalUrl(u"https://kde.org/announcements/plasma/6/6.7.0/"_s, u"Followed 1 redirect; removed 1 tracking parameter."_s);
            QTest::keyClick(f.list, Qt::Key_Down);
            const QImage image = f.window.grab().toImage();
            QVERIFY(!image.isNull());
            const int lightness = klinker::test::meanLightness(image);
            QVERIFY2(dark ? lightness < 110 : lightness > 150, qPrintable(u"mean lightness %1"_s.arg(lightness)));
            // Rounded corners are transparent.
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
            klinker::test::saveScreenshot(&f.window, u"chooser-"_s + suffix);

            child<QToolButton>(&f.window, u"detailsButton"_s)->click();
            f.window.showError(u"Could not start Vivaldi: “vivaldi-stable” was not found in PATH."_s);
            f.window.resize(f.window.preferredSize());
            klinker::test::saveScreenshot(&f.window, u"chooser-details-error-"_s + suffix);
        }
        {
            Fixture f(4);
            f.window.setBusy(u"Following redirects…"_s);
            klinker::test::saveScreenshot(&f.window, u"chooser-busy-"_s + suffix);
        }
        {
            Fixture f(14);
            f.window.resize(ChooserWindow::CardWidth, 420);
            QTest::keyClicks(f.list, u"r"_s);
            klinker::test::saveScreenshot(&f.window, u"chooser-filtered-scrolling-"_s + suffix);
        }
        klinker::test::applyTheme(false);
    }

    // Horizontal chooser

    void verticalIsTheDefault()
    {
        ChooserWindow window;
        QCOMPARE(window.preferences(), klinker::ChooserSettings());
        Fixture f(3);
        QCOMPARE(f.list->flow(), QListView::TopToBottom);
        QCOMPARE(f.window.preferredSize().width(), ChooserWindow::CardWidth);
        QCOMPARE(f.rowRect(1).left(), f.rowRect(0).left());
        QCOMPARE(f.rowRect(1).top(), f.rowRect(0).bottom() + 1);
    }

    void horizontalTilesSitSideBySide_data()
    {
        QTest::addColumn<int>("iconSize");
        QTest::addColumn<bool>("names");
        QTest::newRow("48 with names") << 48 << true;
        QTest::newRow("48 icons only") << 48 << false;
        QTest::newRow("24 with names") << 24 << true;
        QTest::newRow("24 icons only") << 24 << false;
        QTest::newRow("128 with names") << 128 << true;
        QTest::newRow("128 icons only") << 128 << false;
    }

    void horizontalTilesSitSideBySide()
    {
        QFETCH(int, iconSize);
        QFETCH(bool, names);
        Fixture f(5, false, horizontal(iconSize, names));
        QCOMPARE(f.window.preferences(), horizontal(iconSize, names));
        QCOMPARE(f.list->flow(), QListView::LeftToRight);
        QVERIFY(!f.list->isWrapping());
        const QRect first = f.rowRect(0);
        for (int i = 1; i < 5; ++i) {
            const QRect tile = f.rowRect(i);
            QCOMPARE(tile.size(), first.size());
            QCOMPARE(tile.top(), first.top());
            QCOMPARE(tile.left(), first.left() + i * first.width());
        }
        // At the natural size every tile is in view and nothing scrolls.
        QCOMPARE(f.list->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(f.list->verticalScrollBar()->maximum(), 0);
        QVERIFY(f.list->viewport()->rect().contains(f.rowRect(4)));
        QVERIFY(first.width() >= iconSize);
        const int nameHeight = f.list->fontMetrics().height();
        if (names) {
            QVERIFY(first.height() >= iconSize + nameHeight);
            QVERIFY(first.width() >= iconSize + 8);
        } else {
            // Square, with padding but no room for a name.
            QCOMPARE(first.width(), first.height());
            Fixture named(5, false, horizontal(iconSize, true));
            QVERIFY(first.height() + nameHeight <= named.rowRect(0).height());
            QVERIFY2(first.height() - iconSize <= 32, qPrintable(u"tile %1 for icon %2"_s.arg(first.height()).arg(iconSize)));
        }
    }

    void horizontalWidthFollowsTheIcons()
    {
        Fixture two(2, false, horizontal(48, false));
        Fixture eight(8, false, horizontal(48, false));
        Fixture large(8, false, horizontal(96, false));
        const int width = two.window.preferredSize().width();
        QVERIFY2(width < ChooserWindow::CardWidth, qPrintable(QString::number(width)));
        QVERIFY(eight.window.preferredSize().width() > width);
        QVERIFY(large.window.preferredSize().width() > eight.window.preferredSize().width());
        // A few icons are not stretched across the card, but centred.
        QCOMPARE(two.list->width(), 2 * two.rowRect(0).width() + 2 * two.list->frameWidth());
        QVERIFY(std::abs(two.list->geometry().center().x() - two.window.rect().center().x()) <= 1);
    }

    void horizontalNamesAreAlwaysInTooltipsAndAccessibleNames()
    {
        Fixture f(12, false, horizontal(48, false));
        const QModelIndex first = f.list->model()->index(0, 0);
        QCOMPARE(first.data(Qt::AccessibleTextRole).toString(), u"Firefox, press 1"_s);
        const QString tip = first.data(Qt::ToolTipRole).toString();
        QVERIFY2(tip.contains(u"Firefox"_s) && tip.contains(u"1"_s) && tip.contains(u"firefox · Detected"_s), qPrintable(tip));
        const QModelIndex tenth = f.list->model()->index(9, 0);
        QCOMPARE(tenth.data(Qt::AccessibleTextRole).toString(), u"qutebrowser"_s);
        QVERIFY(tenth.data(Qt::ToolTipRole).toString().contains(u"qutebrowser"_s));
        // Markup in a name is shown literally.
        f.window.setBrowsers({{u"x.desktop"_s, u"<b>Bold</b>"_s, QString(), u"x"_s}});
        QVERIFY(f.list->model()->index(0, 0).data(Qt::ToolTipRole).toString().contains(u"&lt;b&gt;Bold"_s));
    }

    void horizontalKeysAndClicksChoose()
    {
        Fixture f(12, false, horizontal(48, false));
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        QCOMPARE(QApplication::focusWidget(), f.list);
        press(&f.window, Qt::Key_9);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b9.desktop"_s);

        QTest::mouseClick(f.list->viewport(), Qt::LeftButton, {}, f.rowRect(1).center());
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b2.desktop"_s);

        // Left and right move along the strip.
        press(&f.window, Qt::Key_Right);
        press(&f.window, Qt::Key_Right);
        press(&f.window, Qt::Key_Left);
        press(&f.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b3.desktop"_s);

        // Numbers follow what is shown after filtering.
        QTest::keyClicks(f.list, u"o"_s);
        auto *filter = child<QLineEdit>(&f.window, u"filterEdit"_s);
        QVERIFY(filter->hasFocus());
        QTest::keyClick(filter, Qt::Key_Down);
        QVERIFY(f.list->hasFocus());
        QTest::keyClick(f.list, Qt::Key_3);
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b5.desktop"_s);
    }

    void horizontalLongListsScroll()
    {
        Fixture f(30, false, horizontal(64, true));
        const QSize natural = f.window.preferredSize();
        const QSize narrow = f.window.preferredSize(QSize(600, 2000));
        QCOMPARE(narrow.width(), 600);
        // When the icons do not fit, there is room for a scroll bar below them.
        QVERIFY(narrow.height() > natural.height());
        QCOMPARE(f.window.preferredSize(QSize(natural.width(), 2000)), natural);
        f.window.resize(narrow);
        QTRY_VERIFY(f.list->horizontalScrollBar()->maximum() > 0);
        QCOMPARE(f.list->verticalScrollBar()->maximum(), 0);
        QVERIFY(f.list->viewport()->height() >= f.rowRect(0).height());
        QString why;
        QVERIFY2(reachable(child<QWidget>(&f.window, u"cancelButton"_s), &f.window, &why), qPrintable(why));

        QCOMPARE(QApplication::focusWidget(), f.list);
        for (int i = 0; i < 25; ++i) {
            press(&f.window, Qt::Key_Right);
        }
        QTRY_COMPARE(f.list->currentIndex().row(), 25);
        QVERIFY(f.list->viewport()->rect().contains(f.rowRect(25)));
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        press(&f.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b26.desktop"_s);
        press(&f.window, Qt::Key_Home);
        QTRY_VERIFY(f.list->viewport()->rect().contains(f.rowRect(0)));
    }

    void horizontalCardFitsASmallScreen_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<int>("iconSize");
        QTest::addColumn<QRect>("screen");
        for (const QString &style : Styles) {
            QTest::addRow("%s, 48 px on 400x320", qPrintable(style)) << style << 48 << QRect(0, 0, 400, 320);
            QTest::addRow("%s, 128 px on 400x280", qPrintable(style)) << style << 128 << QRect(-400, -50, 400, 280);
        }
    }

    // Large icons on a small screen: the window stays on the screen and every
    // browser, the link and the footer can still be reached. The row is never
    // scrolled vertically: if a whole tile would not fit, the icons are shown
    // smaller, while the preference is kept.
    void horizontalCardFitsASmallScreen()
    {
        QFETCH(QString, style);
        QFETCH(int, iconSize);
        QFETCH(QRect, screen);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style);
        ChooserWindow window;
        window.setPreferences(horizontal(iconSize, true));
        window.setOriginalUrl(Url);
        window.setFinalUrl(Url, u"Opened as received."_s);
        window.setBrowsers(entries(12));

        klinker::PlacementRequest request;
        request.preferredSize = window.preferredSize(klinker::maximumSize(screen, 8));
        request.screens = {{u"small"_s, screen, screen}};
        request.primaryOutput = u"small"_s;
        const klinker::Placement placement = klinker::planPlacement(request);
        QVERIFY(screen.adjusted(8, 8, -8, -8).contains(placement.geometry));
        window.resize(placement.geometry.size());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QCOMPARE(window.size(), placement.geometry.size());
        QCOMPARE(window.preferences(), horizontal(iconSize, true));
        // 48 px fit; 128 px tiles would not, so smaller icons are shown.
        QVERIFY(window.shownIconSize() >= klinker::ChooserSettings::MinIconSize);
        QVERIFY(iconSize == 48 ? window.shownIconSize() == iconSize : window.shownIconSize() < iconSize);

        QString why;
        for (const QString &name : {u"finalUrlLabel"_s, u"copyUrlButton"_s, u"detailsButton"_s, u"settingsButton"_s, u"cancelButton"_s}) {
            QVERIFY2(reachable(child<QWidget>(&window, name), &window, &why), qPrintable(why));
        }
        auto *list = child<QListView>(&window, u"browserList"_s);
        const QRect listRect(list->mapTo(&window, QPoint()), list->size());
        QVERIFY(window.rect().contains(listRect));
        const QString problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QVERIFY(list->horizontalScrollBar()->maximum() > 0);
        QCOMPARE(QApplication::focusWidget(), list);
        QSignalSpy chosen(&window, &ChooserWindow::browserChosen);
        press(&window, Qt::Key_End);
        QTRY_VERIFY(list->viewport()->rect().contains(list->visualRect(list->model()->index(11, 0))));
        press(&window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b12.desktop"_s);
        klinker::test::saveScreenshot(&window, u"chooser-horizontal-small-screen-%1-%2"_s.arg(iconSize).arg(style.toLower()));
    }

    void preferencesApplyToAShownChooser()
    {
        Fixture f(4);
        press(&f.window, Qt::Key_Down);
        QTRY_COMPARE(f.list->currentIndex().row(), 1);
        const QSize maximum = klinker::maximumSize(f.window.screen()->availableGeometry(), 8);

        f.window.setPreferences(horizontal(64, false));
        QCOMPARE(f.list->flow(), QListView::LeftToRight);
        QCOMPARE(f.rowRect(1).top(), f.rowRect(0).top());
        QCOMPARE(f.list->currentIndex().row(), 1);
        QCOMPARE(QApplication::focusWidget(), f.list);
        QTRY_COMPARE(f.window.size(), f.window.preferredSize(maximum));
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        press(&f.window, Qt::Key_Right);
        press(&f.window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"b3.desktop"_s);

        f.window.setPreferences({});
        QCOMPARE(f.list->flow(), QListView::TopToBottom);
        QCOMPARE(f.rowRect(1).left(), f.rowRect(0).left());
        QCOMPARE(f.window.preferredSize().width(), ChooserWindow::CardWidth);
        QTRY_COMPARE(f.window.size(), f.window.preferredSize(maximum));
    }

    void refitKeepsTheChooserOnItsScreen()
    {
        Fixture f(3);
        QScreen *screen = f.window.screen();
        const QRect available = screen->availableGeometry();
        f.window.move(available.right() - f.window.width() - 20, available.top() + 40);
        f.window.hide();
        f.window.setPreferences(horizontal(96, true));
        f.window.setBrowsers(entries(12));
        f.window.refit();
        QCOMPARE(f.window.screen(), screen);
        QVERIFY(f.window.width() > ChooserWindow::CardWidth);
        QCOMPARE(f.window.size(), f.window.preferredSize(klinker::maximumSize(available, 8)));
        QVERIFY(available.adjusted(8, 8, -8, -8).contains(f.window.geometry()));
    }

    void horizontalScreenshots_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void horizontalScreenshots()
    {
        QFETCH(bool, dark);
        klinker::test::applyTheme(dark);
        const QString suffix = dark ? u"dark"_s : u"light"_s;
        const auto check = [dark](ChooserWindow *window) {
            const QImage image = window->grab().toImage();
            const int lightness = klinker::test::meanLightness(image);
            QVERIFY2(dark ? lightness < 110 : lightness > 150, qPrintable(u"mean lightness %1"_s.arg(lightness)));
            QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        };
        {
            Fixture f(6, false, horizontal(48, true));
            f.window.setFinalUrl(u"https://kde.org/announcements/plasma/6/6.7.0/"_s, u"Opened as received."_s);
            press(&f.window, Qt::Key_Right);
            QTRY_COMPARE(f.list->currentIndex().row(), 1);
            check(&f.window);
            klinker::test::saveScreenshot(&f.window, u"chooser-horizontal-names-"_s + suffix);
        }
        {
            Fixture f(6, false, horizontal(96, false));
            check(&f.window);
            klinker::test::saveScreenshot(&f.window, u"chooser-horizontal-icons-96-"_s + suffix);
        }
        {
            Fixture f(6, false, horizontal(24, false));
            check(&f.window);
            klinker::test::saveScreenshot(&f.window, u"chooser-horizontal-icons-24-"_s + suffix);
        }
        {
            Fixture f(30, false, horizontal(64, true));
            f.window.resize(f.window.preferredSize(QSize(640, 2000)));
            for (int i = 0; i < 8; ++i) {
                press(&f.window, Qt::Key_Right);
            }
            QTRY_COMPARE(f.list->currentIndex().row(), 8);
            check(&f.window);
            klinker::test::saveScreenshot(&f.window, u"chooser-horizontal-many-constrained-"_s + suffix);
        }
        klinker::test::applyTheme(false);
    }

    // With the Oxygen widget style and the Breeze Dark colours.
    void horizontalOxygenScreenshots_data()
    {
        QTest::addColumn<bool>("names");
        QTest::newRow("names") << true;
        QTest::newRow("icons") << false;
    }

    void horizontalOxygenScreenshots()
    {
        QFETCH(bool, names);
        if (!klinker::test::hasStyle(u"Oxygen"_s)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(u"Oxygen"_s, true);
        Fixture f(5, false, horizontal(names ? 48 : 64, names));
        f.window.setFinalUrl(u"https://kde.org/announcements/plasma/6/6.7.0/"_s, u"Opened as received."_s);
        press(&f.window, Qt::Key_Right);
        QTRY_COMPARE(f.list->currentIndex().row(), 1);
        const QString problem = klinker::test::horizontalStripProblem(f.list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        const QImage image = f.window.grab().toImage();
        const int lightness = klinker::test::meanLightness(image);
        QVERIFY2(lightness < 110, qPrintable(u"mean lightness %1"_s.arg(lightness)));
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        klinker::test::saveScreenshot(&f.window, u"chooser-horizontal-%1-oxygen-dark"_s.arg(names ? u"names"_s : u"icons"_s));
    }

    void horizontalStripFitsInEveryStyle_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("iconSize");
        QTest::addColumn<bool>("names");
        for (const QString &style : Styles) {
            const QByteArray s = style.toLatin1();
            QTest::addRow("%s, reported six named, 48", s.constData()) << style << 6 << 48 << true;
            QTest::addRow("%s, one named, 48", s.constData()) << style << 1 << 48 << true;
            QTest::addRow("%s, nine named, 24", s.constData()) << style << 9 << 24 << true;
            QTest::addRow("%s, nine icons only, 48", s.constData()) << style << 9 << 48 << false;
            QTest::addRow("%s, six named, 128", s.constData()) << style << 6 << 128 << true;
            QTest::addRow("%s, many named, 64", s.constData()) << style << 30 << 64 << true;
        }
    }

    // Shown as the application does: as wide as the whole row of tiles, so
    // nothing scrolls, unless the screen is too narrow for it. Then the window
    // is as wide as the screen allows and only scrolls horizontally, with the
    // scroll bar below whole tiles.
    void horizontalStripFitsInEveryStyle()
    {
        QFETCH(QString, style);
        QFETCH(int, count);
        QFETCH(int, iconSize);
        QFETCH(bool, names);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style);
        ChooserWindow window;
        window.setPreferences(horizontal(iconSize, names));
        window.setOriginalUrl(u"https://www.kde.org/"_s);
        window.setFinalUrl(u"https://www.kde.org/"_s, u"Opened as received."_s);
        window.setBrowsers(count == 6 ? reportedEntries() : entries(count));
        const QSize natural = window.preferredSize();
        showAsTheApplication(window);
        auto *list = child<QListView>(&window, u"browserList"_s);
        const QSize maximum = screenMaximum(&window);
        const bool fits = natural.width() <= maximum.width();

        QCOMPARE(window.width(), std::min(natural.width(), maximum.width()));
        QCOMPARE(window.size(), window.preferredSize(maximum));
        QCOMPARE(window.shownIconSize(), iconSize);
        const QString problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(list->horizontalScrollBar()->isVisible(), !fits);
        const QRect tile = list->visualRect(list->model()->index(0, 0));
        QCOMPARE(list->viewport()->height(), tile.height());
        if (fits) {
            QCOMPARE(list->width(), count * tile.width() + 2 * list->frameWidth());
        }
        if (names) {
            QVERIFY(tile.height() >= iconSize + list->fontMetrics().height());
        }
        QString why;
        for (const QString &name : {u"finalUrlLabel"_s, u"detailsButton"_s, u"settingsButton"_s, u"cancelButton"_s}) {
            QVERIFY2(reachable(child<QWidget>(&window, name), &window, &why), qPrintable(why));
        }

        // Both ends can be reached with the keyboard.
        press(&window, Qt::Key_End);
        QTRY_COMPARE(list->currentIndex().row(), count - 1);
        QVERIFY(list->viewport()->rect().contains(list->visualRect(list->currentIndex())));
        press(&window, Qt::Key_Home);
        QTRY_COMPARE(list->currentIndex().row(), 0);
        QVERIFY(list->viewport()->rect().contains(list->visualRect(list->currentIndex())));
        if (count == 6 && iconSize == 48) {
            klinker::test::saveScreenshot(&window, u"chooser-horizontal-reported-six-%1"_s.arg(style.toLower()));
        } else if (count == 30) {
            klinker::test::saveScreenshot(&window, u"chooser-horizontal-screen-limited-%1"_s.arg(style.toLower()));
        }
    }

    void horizontalScrollsOnlyBeyondTheNaturalWidth_data()
    {
        QTest::addColumn<QString>("style");
        for (const QString &style : Styles) {
            QTest::newRow(qPrintable(style)) << style;
        }
    }

    // At exactly the width of the row nothing scrolls; one pixel less and the
    // strip scrolls horizontally, with exactly the scroll bar's height added.
    void horizontalScrollsOnlyBeyondTheNaturalWidth()
    {
        QFETCH(QString, style);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style);
        ChooserWindow window;
        window.setPreferences(horizontal(48, true));
        window.setOriginalUrl(u"https://www.kde.org/"_s);
        window.setFinalUrl(u"https://www.kde.org/"_s, u"Opened as received."_s);
        window.setBrowsers(reportedEntries());
        const QSize natural = window.preferredSize();
        QCOMPARE(window.preferredSize(QSize(natural.width(), 2000)), natural);
        const QSize narrower = window.preferredSize(QSize(natural.width() - 1, 2000));
        QCOMPARE(narrower.width(), natural.width() - 1);
        QVERIFY(narrower.height() > natural.height());

        // Placed on screens exactly wide enough, and one pixel narrower.
        for (const int extra : {0, -1}) {
            const QRect screen(-1500, 120, natural.width() + 16 + extra, 900);
            klinker::PlacementRequest request;
            request.preferredSize = window.preferredSize(klinker::maximumSize(screen, 8));
            request.screens = {{u"side"_s, screen, screen}};
            const klinker::Placement placement = klinker::planPlacement(request);
            QCOMPARE(placement.geometry.size(), extra == 0 ? natural : narrower);
            QVERIFY(screen.adjusted(8, 8, -8, -8).contains(placement.geometry));
        }

        window.resize(natural);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *list = child<QListView>(&window, u"browserList"_s);
        const int tileHeight = list->visualRect(list->model()->index(0, 0)).height();
        QString problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QVERIFY(!list->horizontalScrollBar()->isVisible());
        QCOMPARE(list->viewport()->height(), tileHeight);

        window.resize(narrower);
        QTRY_VERIFY(list->horizontalScrollBar()->isVisible());
        problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(list->horizontalScrollBar()->maximum(), 1);
        QCOMPARE(list->viewport()->height(), tileHeight);
        QSignalSpy chosen(&window, &ChooserWindow::browserChosen);
        press(&window, Qt::Key_End);
        QTRY_VERIFY(list->viewport()->rect().contains(list->visualRect(list->model()->index(5, 0))));
        press(&window, Qt::Key_Return);
        QTRY_COMPARE(chosen.size(), 1);
        QCOMPARE(chosen.takeFirst().at(0).toString(), u"switchyard.desktop"_s);

        // Wide enough again: no scroll bar is left behind.
        window.resize(natural);
        QTRY_VERIFY(!list->horizontalScrollBar()->isVisible());
        problem = klinker::test::horizontalStripProblem(list);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
    }

    void horizontalWidthFollowsTheBrowsersShown_data()
    {
        horizontalScrollsOnlyBeyondTheNaturalWidth_data();
    }

    // Shown, the window grows with every browser added (as after a rescan) and
    // follows filtering, always without scrolling.
    void horizontalWidthFollowsTheBrowsersShown()
    {
        QFETCH(QString, style);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style);
        Fixture f(1, false, horizontal(48, true));
        const QSize maximum = screenMaximum(&f.window);
        // The window is resized at once, the strip once its layout request is handled.
        const auto check = [&f, &maximum](const QString &step) {
            QTRY_COMPARE(f.window.size(), f.window.preferredSize(maximum));
            QTRY_VERIFY2(klinker::test::horizontalStripProblem(f.list).isEmpty(),
                         qPrintable(step + u": "_s + klinker::test::horizontalStripProblem(f.list)));
        };
        check(u"one"_s);
        int previous = f.window.width();
        for (int count = 2; count <= 6; ++count) {
            f.window.setBrowsers(entries(count));
            check(u"%1 browsers"_s.arg(count));
            QVERIFY(f.window.width() >= previous);
            if (count * f.rowRect(0).width() > ChooserWindow::CardWidth) {
                QVERIFY2(f.window.width() > previous, qPrintable(u"%1 browsers"_s.arg(count)));
            }
            previous = f.window.width();
        }

        // Typing filters, and shows the filter field above the strip.
        QTest::keyClicks(f.list, u"fi"_s);
        auto *filter = child<QLineEdit>(&f.window, u"filterEdit"_s);
        QVERIFY(filter->isVisible());
        QCOMPARE(f.window.displayedBrowserIds(), QStringList{u"b1.desktop"_s});
        check(u"filtered"_s);
        QString why;
        QVERIFY2(reachable(filter, &f.window, &why), qPrintable(why));
        QVERIFY2(reachable(child<QWidget>(&f.window, u"cancelButton"_s), &f.window, &why), qPrintable(why));
        QTest::keyClick(filter, Qt::Key_Escape);
        QVERIFY(!filter->isVisible());
        check(u"filter cleared"_s);
        QCOMPARE(f.window.width(), previous);
    }

    void horizontalRefitsWhenItsLookChanges_data()
    {
        horizontalScrollsOnlyBeyondTheNaturalWidth_data();
    }

    // Preferences, the font and the widget style can change while the chooser
    // is shown, or hidden for the settings; it is refitted every time.
    void horizontalRefitsWhenItsLookChanges()
    {
        QFETCH(QString, style);
        if (!klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        klinker::test::ThemeScope theme(style);
        Fixture f(6);
        const QSize maximum = screenMaximum(&f.window);
        // The window is resized at once, the strip once its layout request is handled.
        const auto check = [&f, &maximum](const QString &step) {
            QTRY_COMPARE(f.window.size(), f.window.preferredSize(maximum));
            QTRY_VERIFY2(klinker::test::horizontalStripProblem(f.list).isEmpty(),
                         qPrintable(step + u": "_s + klinker::test::horizontalStripProblem(f.list)));
        };
        f.window.setPreferences(horizontal(48, true));
        check(u"horizontal"_s);
        const QSize named48 = f.rowRect(0).size();
        f.window.setPreferences(horizontal(96, true));
        check(u"96 px"_s);
        QVERIFY(f.rowRect(0).height() > named48.height());
        f.window.setPreferences(horizontal(96, false));
        check(u"96 px without names"_s);
        f.window.setPreferences(horizontal(48, true));
        check(u"48 px again"_s);
        QCOMPARE(f.rowRect(0).size(), named48);

        const QFont original = f.window.font();
        QFont larger = original;
        larger.setPointSizeF(original.pointSizeF() * 1.5);
        f.window.setFont(larger);
        check(u"larger font"_s);
        QVERIFY(f.rowRect(0).height() > named48.height());
        f.window.setFont(original);
        check(u"font restored"_s);
        QCOMPARE(f.rowRect(0).size(), named48);

        const QString other = style == u"Oxygen"_s ? u"Fusion"_s : u"Oxygen"_s;
        if (klinker::test::hasStyle(other)) {
            QApplication::setStyle(other);
            check(u"switched to "_s + other);
            QApplication::setStyle(style);
            check(u"switched back"_s);
        }

        // Hidden for the settings, then shown with more browsers.
        f.window.hide();
        f.window.setBrowsers(entries(9));
        f.window.refit();
        f.window.show();
        check(u"shown again"_s);

        // The vertical list is unchanged.
        f.window.setPreferences({});
        QTRY_COMPARE(f.window.size(), f.window.preferredSize(maximum));
        QCOMPARE(f.window.width(), ChooserWindow::CardWidth);
        QCOMPARE(f.list->flow(), QListView::TopToBottom);
        QCOMPARE(f.list->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QCOMPARE(f.list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    }

    // Closing on focus loss

    void focusLossIsIgnoredByDefault()
    {
        Fixture f(3);
        QVERIFY(!f.window.preferences().closeOnFocusLoss);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        klinker::test::focusOtherApplication();
        QVERIFY(!f.window.isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(f.window.isVisible());
    }

    void focusLossCancelsOnce()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QVERIFY(f.window.isActiveWindow());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy chosen(&f.window, &ChooserWindow::browserChosen);
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
        klinker::test::focusWindow(&f.window);
        QTRY_VERIFY(f.window.isActiveWindow());
        klinker::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(chosen.size(), 0);
    }

    void focusLossWhileBusyCancels()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QVERIFY(f.window.isBusy());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    // The window may never get the focus (for example while it starts, or if
    // the compositor refuses): then there is nothing it could have lost.
    void aChooserThatWasNeverActiveStays()
    {
        ChooserWindow window;
        window.setPreferences(closingOnFocusLoss());
        window.setFinalUrl(Url, QString());
        window.setBrowsers(entries(3));
        // The offscreen platform activates every window it shows; here the
        // window is never told, as if the compositor had not given it the focus.
        ActivationBlocker blocker;
        window.installEventFilter(&blocker);
        QSignalSpy cancelled(&window, &ChooserWindow::cancelled);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        klinker::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QVERIFY(blocker.blocked > 0);
        QCOMPARE(cancelled.size(), 0);

        // Once it has had the focus, losing it counts.
        window.removeEventFilter(&blocker);
        klinker::test::focusWindow(&window);
        QTRY_VERIFY(window.isActiveWindow());
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    void movingBetweenControlsKeepsTheChooser()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        for (int i = 0; i < 8; ++i) {
            press(&f.window, Qt::Key_Tab);
        }
        QTest::mouseClick(child<QToolButton>(&f.window, u"detailsButton"_s), Qt::LeftButton);
        QTest::mouseClick(child<QToolButton>(&f.window, u"copyOriginalButton"_s), Qt::LeftButton);
        QTest::mouseClick(child<QToolButton>(&f.window, u"copyUrlButton"_s), Qt::LeftButton);
        QTest::keyClicks(f.list, u"fi"_s);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(f.window.isActiveWindow());
    }

    void ownMenusAndDialogsKeepTheChooser()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);

        QMenu menu(&f.window);
        menu.addAction(u"Copy"_s);
        menu.popup(f.window.mapToGlobal(QPoint(20, 20)));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        menu.close();
        QCOMPARE(cancelled.size(), 0);

        QDialog dialog(&f.window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QVERIFY(!f.window.isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
        dialog.close();
        klinker::test::focusWindow(&f.window);
        QTRY_VERIFY(f.window.isActiveWindow());

        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    // The focus can leave and come back at once, e.g. while the compositor
    // reconfigures the window: only the state after a moment counts.
    void focusThatComesBackKeepsTheChooser()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        klinker::test::focusOtherApplication();
        klinker::test::focusWindow(&f.window);
        QVERIFY(f.window.isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
    }

    // Settings hide the chooser on purpose; it is shown again afterwards.
    void hidingAndShowingAgainStartsOver()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        f.window.hide();
        klinker::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);

        f.window.show();
        QTRY_VERIFY(f.window.isActiveWindow());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    // A check started before hiding belongs to the old showing: it must not
    // close the window shown again before that has had the focus.
    void aStaleCheckCannotCloseTheChooserShownAgain()
    {
        Fixture f(3, false, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        klinker::test::focusOtherApplication();
        f.window.hide();
        ActivationBlocker blocker;
        f.window.installEventFilter(&blocker);
        f.window.show();
        QCoreApplication::processEvents();
        klinker::test::focusOtherApplication();
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(f.window.isVisible());

        f.window.removeEventFilter(&blocker);
        klinker::test::focusWindow(&f.window);
        QTRY_VERIFY(f.window.isActiveWindow());
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    void turningTheOptionOnAppliesToAShownChooser()
    {
        Fixture f(3);
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        f.window.setPreferences(closingOnFocusLoss());
        klinker::test::focusOtherApplication();
        QTRY_COMPARE(cancelled.size(), 1);
    }

    // While a focus loss is being decided the caller holds back automatic
    // routing; every pending decision ends exactly once, either cancelled or
    // resolved.
    void aPendingFocusLossEndsCancelled()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy resolved(&f.window, &ChooserWindow::focusLossResolved);
        QVERIFY(!f.window.isFocusLossPending());
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        QTRY_COMPARE(cancelled.size(), 1);
        QVERIFY(!f.window.isFocusLossPending());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(resolved.size(), 0);
        QCOMPARE(cancelled.size(), 1);
    }

    void aPendingFocusLossIsResolvedWhenTheFocusComesBack()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy resolved(&f.window, &ChooserWindow::focusLossResolved);
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        klinker::test::focusWindow(&f.window);
        QVERIFY(!f.window.isFocusLossPending());
        QCOMPARE(resolved.size(), 1);

        // Lost again: a new decision, and the old check stays stale.
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        QTRY_COMPARE(cancelled.size(), 1);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(cancelled.size(), 1);
    }

    void aPendingFocusLossIsResolvedByHiding()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy resolved(&f.window, &ChooserWindow::focusLossResolved);
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        f.window.hide();
        QVERIFY(!f.window.isFocusLossPending());
        QCOMPARE(resolved.size(), 1);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(cancelled.size(), 0);

        // Shown again it starts over; nothing is pending until it loses the focus.
        f.window.show();
        QTRY_VERIFY(f.window.isActiveWindow());
        QVERIFY(!f.window.isFocusLossPending());
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        QTRY_COMPARE(cancelled.size(), 1);
        QCOMPARE(resolved.size(), 1);
    }

    // A window of this application took the focus: resolved after the delay.
    void aPendingFocusLossIsResolvedByAnOwnDialog()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy resolved(&f.window, &ChooserWindow::focusLossResolved);
        QDialog dialog(&f.window);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        // Only a pending decision is resolved.
        QTRY_COMPARE(resolved.size(), 1);
        QVERIFY(!f.window.isFocusLossPending());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(cancelled.size(), 0);
    }

    void nothingIsPendingWithoutTheOptionOrPriorFocus()
    {
        {
            Fixture f(3, true);
            klinker::test::focusOtherApplication();
            QVERIFY(!f.window.isFocusLossPending());
        }
        ChooserWindow window;
        window.setPreferences(closingOnFocusLoss());
        window.setBusy(u"Following redirects…"_s);
        window.setBrowsers(entries(3));
        ActivationBlocker blocker;
        window.installEventFilter(&blocker);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        klinker::test::focusOtherApplication();
        QVERIFY(!window.isFocusLossPending());
    }

    // Dismissed by the caller (after a choice): the decision is dropped silently.
    void dismissingDropsAPendingFocusLoss()
    {
        Fixture f(3, true, closingOnFocusLoss());
        QSignalSpy cancelled(&f.window, &ChooserWindow::cancelled);
        QSignalSpy resolved(&f.window, &ChooserWindow::focusLossResolved);
        klinker::test::focusOtherApplication();
        QVERIFY(f.window.isFocusLossPending());
        f.window.dismiss();
        QVERIFY(!f.window.isFocusLossPending());
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 150);
        QCOMPARE(resolved.size(), 0);
        QCOMPARE(cancelled.size(), 0);
    }

    void waylandLayerShellPopup_data()
    {
        QTest::addColumn<bool>("busy");
        QTest::addColumn<klinker::ChooserSettings>("preferences");
        QTest::addColumn<QString>("style");
        QTest::newRow("ready") << false << klinker::ChooserSettings() << QString();
        QTest::newRow("busy, then ready") << true << klinker::ChooserSettings() << QString();
        QTest::newRow("horizontal") << false << horizontal(64, true) << QString();
        QTest::newRow("horizontal, busy, then ready") << true << horizontal(48, false) << QString();
        QTest::newRow("horizontal, reported six") << false << horizontal(48, true) << u"Breeze"_s;
        QTest::newRow("horizontal, reported six, Oxygen") << false << horizontal(48, true) << u"Oxygen"_s;
    }

    // Wayland only: QT_QPA_PLATFORM=wayland ./test_chooser waylandLayerShellPopup
    void waylandLayerShellPopup()
    {
        if (!QGuiApplication::platformName().startsWith(u"wayland"_s)) {
            QSKIP("needs QT_QPA_PLATFORM=wayland");
        }
#ifndef KLINKER_HAVE_LAYERSHELLQT
        QSKIP("built without LayerShellQt");
#else
        QFETCH(bool, busy);
        QFETCH(klinker::ChooserSettings, preferences);
        QFETCH(QString, style);
        if (!style.isEmpty() && !klinker::test::hasStyle(style)) {
            QSKIP("style not installed");
        }
        std::optional<klinker::test::ThemeScope> theme;
        if (!style.isEmpty()) {
            theme.emplace(style);
        }
        ChooserWindow window;
        window.setPreferences(preferences);
        window.setOriginalUrl(Url);
        if (busy) {
            window.setBusy(u"Following redirects…"_s);
        } else {
            window.setFinalUrl(Url, u"Opened as received."_s);
        }
        window.setBrowsers(style.isEmpty() ? entries(5) : reportedEntries());
        const klinker::PopupPlacement placed = klinker::placePopup(&window, [&window](const QSize &maximum) {
            return window.preferredSize(maximum);
        });
        QVERIFY(placed.layerShell);
        QCOMPARE(placed.placement.mode, klinker::PlacementMode::Centered);
        // Under KWin the active output comes from D-Bus; elsewhere the primary screen is used.
        const QString active = klinker::kwinActiveOutputName();
        qInfo() << "KWin active output:" << active;
        const auto names = QGuiApplication::screens();
        if (std::ranges::any_of(names, [&active](QScreen *s) {
                return s->name() == active;
            })) {
            QCOMPARE(placed.placement.screenName, active);
        } else {
            QCOMPARE(placed.placement.screenName, QGuiApplication::primaryScreen()->name());
        }
        auto *layer = LayerShellQt::Window::get(window.windowHandle());
        QCOMPARE(layer->anchors(), LayerShellQt::Window::Anchors());
        QCOMPARE(layer->layer(), LayerShellQt::Window::LayerTop);
        QCOMPARE(layer->exclusionZone(), 0);
        QCOMPARE(layer->keyboardInteractivity(), LayerShellQt::Window::KeyboardInteractivityOnDemand);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QScreen *screen = window.windowHandle()->screen();
        QVERIFY(screen);
        qInfo() << "placed on" << placed.placement.screenName << "screen" << screen->name() << "size" << window.size();
        QVERIFY(window.width() <= screen->availableGeometry().width());
        QVERIFY(window.height() <= screen->availableGeometry().height());
        QCOMPARE(window.size(), placed.placement.geometry.size());

        // The compositor itself must give the layer surface keyboard focus
        // (QWindow::isActive() follows wl_keyboard enter/leave).
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *list = window.findChild<QListView *>(u"browserList"_s);
        if (busy) {
            window.setFinalUrl(u"https://example.org/final"_s, u"Followed 1 redirect."_s);
        }
        QTRY_COMPARE(QApplication::focusWidget(), list);

        if (preferences.style == klinker::ChooserStyle::Horizontal) {
            // The private KWin's output is wide enough for the whole row.
            QTRY_COMPARE(window.size(), window.preferredSize(screenMaximum(&window)));
            QVERIFY(window.width() < screenMaximum(&window).width());
            const QString problem = klinker::test::horizontalStripProblem(list);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
            QVERIFY(!list->horizontalScrollBar()->isVisible());
        }

        QSignalSpy chosen(&window, &ChooserWindow::browserChosen);
        press(&window, Qt::Key_2);
        // With accessibility active, the AT-SPI bridge delivers key events asynchronously.
        QTRY_COMPARE(chosen.size(), 1);
        const QString second = style.isEmpty() ? u"b2.desktop"_s : u"chromium.desktop"_s;
        QCOMPARE(chosen.takeFirst().at(0).toString(), second);
        if (preferences.style == klinker::ChooserStyle::Horizontal) {
            QCOMPARE(list->flow(), QListView::LeftToRight);
            press(&window, Qt::Key_Right);
            press(&window, Qt::Key_Right);
            press(&window, Qt::Key_Left);
            press(&window, Qt::Key_Return);
            QTRY_COMPARE(chosen.size(), 1);
            QCOMPARE(chosen.takeFirst().at(0).toString(), second);
        }
        klinker::test::saveScreenshot(&window, u"chooser-wayland-layershell-%1"_s.arg(QString::fromLatin1(QTest::currentDataTag()).replace(u", "_s, u"-"_s).replace(u' ', u'-')));
#endif
    }

    // Wayland only, in the private KWin of tests/wayland-smoke.sh: another
    // application really takes the keyboard focus from the layer-shell chooser.
    void waylandFocusLoss_data()
    {
        QTest::addColumn<bool>("closeOnFocusLoss");
        QTest::newRow("option off") << false;
        QTest::newRow("option on") << true;
    }

    void waylandFocusLoss()
    {
        if (QString why; !inPrivateWaylandSession(&why)) {
            QSKIP(qPrintable(why));
        }
#ifndef KLINKER_HAVE_LAYERSHELLQT
        QSKIP("built without LayerShellQt");
#else
        QFETCH(bool, closeOnFocusLoss);
        ChooserWindow window;
        klinker::ChooserSettings preferences = horizontal(48, true);
        preferences.closeOnFocusLoss = closeOnFocusLoss;
        window.setPreferences(preferences);
        window.setOriginalUrl(Url);
        window.setFinalUrl(Url, u"Opened as received."_s);
        window.setBrowsers(entries(4));
        QVERIFY(klinker::placePopup(&window, window.preferredSize()).layerShell);
        QSignalSpy cancelled(&window, &ChooserWindow::cancelled);
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        // Moving between its own controls is not losing the focus.
        press(&window, Qt::Key_Tab);
        press(&window, Qt::Key_Tab);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 300);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(window.isActiveWindow());

        FocusFixture other;
        QVERIFY2(other.start(), "the focus fixture did not start");
        QTRY_VERIFY_WITH_TIMEOUT(other.focused(), 10000);
        QTRY_VERIFY(!window.isActiveWindow());
        if (closeOnFocusLoss) {
            QTRY_COMPARE(cancelled.size(), 1);
        } else {
            QTest::qWait(ChooserWindow::FocusLossDelayMs + 500);
            QCOMPARE(cancelled.size(), 0);
            QVERIFY(window.isVisible());
        }
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 300);
        QCOMPARE(cancelled.size(), closeOnFocusLoss ? 1 : 0);
#endif
    }

    // Wayland only, in the private KWin: the chooser's own popup menu and a
    // dialog of the application take the focus without closing it, and while
    // it is hidden for the settings, another application may have the focus.
    void waylandOwnPopupKeepsTheChooser()
    {
        if (QString why; !inPrivateWaylandSession(&why)) {
            QSKIP(qPrintable(why));
        }
#ifndef KLINKER_HAVE_LAYERSHELLQT
        QSKIP("built without LayerShellQt");
#else
        ChooserWindow window;
        window.setPreferences(closingOnFocusLoss());
        window.setOriginalUrl(Url);
        window.setFinalUrl(Url, u"Opened as received."_s);
        window.setBrowsers(entries(4));
        QVERIFY(klinker::placePopup(&window, window.preferredSize()).layerShell);
        QSignalSpy cancelled(&window, &ChooserWindow::cancelled);
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        QMenu menu(&window);
        menu.addAction(u"One"_s);
        menu.addAction(u"Two"_s);
        menu.popup(window.mapToGlobal(QPoint(40, 40)));
        QVERIFY(QTest::qWaitForWindowExposed(&menu));
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 300);
        QCOMPARE(cancelled.size(), 0);
        menu.close();
        QTRY_VERIFY(window.isActiveWindow());

        // Like the settings: the chooser steps aside for a window of its own.
        window.hide();
        QDialog dialog;
        dialog.setWindowTitle(u"Settings stand-in"_s);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        FocusFixture other;
        QVERIFY2(other.start(), "the focus fixture did not start");
        QTRY_VERIFY_WITH_TIMEOUT(other.focused(), 10000);
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 300);
        QCOMPARE(cancelled.size(), 0);
        other.stop();
        dialog.close();

        // Shown again, it has the focus and losing it counts once more.
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::qWait(ChooserWindow::FocusLossDelayMs + 300);
        QCOMPARE(cancelled.size(), 0);
        FocusFixture again;
        QVERIFY2(again.start(), "the focus fixture did not start");
        QTRY_VERIFY_WITH_TIMEOUT(again.focused(), 10000);
        QTRY_COMPARE(cancelled.size(), 1);
#endif
    }
};

QTEST_MAIN(TestChooser)

#include "test_chooser.moc"
