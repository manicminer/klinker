#include "core/browsercatalog.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace kbs;

namespace
{
DetectedBrowser detected(const QString &id, const QString &name, const QString &program)
{
    return {id, u"/usr/share/applications/"_s + id, name, program + u"-icon"_s, {program, {u"{url}"_s}}};
}

QList<DetectedBrowser> system()
{
    return {detected(u"chromium.desktop"_s, u"Chromium"_s, u"chromium"_s),
            detected(u"firefox.desktop"_s, u"Firefox"_s, u"firefox"_s),
            detected(u"vivaldi.desktop"_s, u"Vivaldi"_s, u"vivaldi"_s)};
}

QStringList ids(const QList<Browser> &browsers)
{
    QStringList result;
    for (const auto &browser : browsers) {
        result.append(browser.id);
    }
    return result;
}
} // namespace

class TestBrowserCatalog : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultsFollowDetection()
    {
        const BrowserCatalog catalog(system());
        const auto browsers = catalog.browsers();
        QCOMPARE(ids(browsers), (QStringList{u"chromium.desktop"_s, u"firefox.desktop"_s, u"vivaldi.desktop"_s}));
        QCOMPARE(browsers[1].name, u"Firefox"_s);
        QCOMPARE(browsers[1].icon, u"firefox-icon"_s);
        QCOMPARE(browsers[1].command, (CommandTemplate{u"firefox"_s, {u"{url}"_s}}));
        QCOMPARE(browsers[1].source, BrowserSource::Detected);
        QVERIFY(!browsers[1].hidden);
        QVERIFY(!browsers[1].customized);
        QCOMPARE(ids(catalog.visibleBrowsers()), ids(browsers));
        QVERIFY(catalog.settings().isEmpty());
    }

    void configuredOrderThenNewlyDetected()
    {
        const BrowserCatalog catalog(system(), {{.id = u"vivaldi.desktop"_s}, {.id = u"firefox.desktop"_s}});
        QCOMPARE(ids(catalog.browsers()), (QStringList{u"vivaldi.desktop"_s, u"firefox.desktop"_s, u"chromium.desktop"_s}));
    }

    void overridesApplyAndUnchangedFieldsFollowRescan()
    {
        BrowserSetting setting{.id = u"firefox.desktop"_s};
        setting.name = u"Firefox (Work)"_s;
        setting.arguments = QStringList{u"-P"_s, u"work"_s, u"{url}"_s};
        BrowserCatalog catalog(system(), {setting});

        auto firefox = catalog.browser(u"firefox.desktop"_s);
        QVERIFY(firefox);
        QCOMPARE(firefox->name, u"Firefox (Work)"_s);
        QCOMPARE(firefox->command, (CommandTemplate{u"firefox"_s, {u"-P"_s, u"work"_s, u"{url}"_s}}));
        QVERIFY(firefox->customized);

        auto rescanned = system();
        rescanned[1].command.program = u"/usr/lib/firefox/firefox"_s;
        rescanned[1].icon = u"new-icon"_s;
        catalog.setDetected(rescanned);
        firefox = catalog.browser(u"firefox.desktop"_s);
        QCOMPARE(firefox->name, u"Firefox (Work)"_s);
        QCOMPARE(firefox->icon, u"new-icon"_s);
        QCOMPARE(firefox->command.program, u"/usr/lib/firefox/firefox"_s);
        QCOMPARE(firefox->command.arguments, (QStringList{u"-P"_s, u"work"_s, u"{url}"_s}));
    }

    void hideKeepsOrderAndRoutingTarget()
    {
        BrowserCatalog catalog(system());
        QVERIFY(catalog.setHidden(u"chromium.desktop"_s, true));
        QCOMPARE(ids(catalog.browsers()), (QStringList{u"chromium.desktop"_s, u"firefox.desktop"_s, u"vivaldi.desktop"_s}));
        QCOMPARE(ids(catalog.visibleBrowsers()), (QStringList{u"firefox.desktop"_s, u"vivaldi.desktop"_s}));
        QVERIFY(catalog.browserIds().contains(u"chromium.desktop"_s));
        QVERIFY(catalog.browser(u"chromium.desktop"_s)->hidden);

        // Hiding a later entry must not move it ahead of earlier unlisted ones.
        BrowserCatalog other(system());
        QVERIFY(other.setHidden(u"vivaldi.desktop"_s, true));
        QCOMPARE(ids(other.browsers()), (QStringList{u"chromium.desktop"_s, u"firefox.desktop"_s, u"vivaldi.desktop"_s}));

        QVERIFY(catalog.setHidden(u"chromium.desktop"_s, false));
        QCOMPARE(catalog.visibleBrowsers().size(), 3);
    }

    void removedDetectedBrowserStaysRemovedAfterRescan()
    {
        BrowserCatalog catalog(system());
        QVERIFY(catalog.remove(u"firefox.desktop"_s));
        QCOMPARE(ids(catalog.browsers()), (QStringList{u"chromium.desktop"_s, u"vivaldi.desktop"_s}));
        QVERIFY(!catalog.browserIds().contains(u"firefox.desktop"_s));

        catalog.setDetected(system());
        QCOMPARE(ids(catalog.browsers()), (QStringList{u"chromium.desktop"_s, u"vivaldi.desktop"_s}));

        // The tombstone survives a round trip through settings.
        const BrowserCatalog reloaded(system(), catalog.settings());
        QCOMPARE(ids(reloaded.browsers()), (QStringList{u"chromium.desktop"_s, u"vivaldi.desktop"_s}));
        QVERIFY(!reloaded.browser(u"firefox.desktop"_s));
        QVERIFY(!catalog.remove(u"firefox.desktop"_s));
    }

    void manualBrowsers()
    {
        BrowserCatalog catalog(system());
        const QString id = catalog.addManual({u"Work"_s, u"web-browser"_s, {u"/opt/work/browser"_s, {u"--profile"_s, u"{url}"_s}}});
        QVERIFY(id.startsWith(u"manual-"_s));
        QVERIFY(!system().isEmpty() && id != system().first().desktopId);
        const QString second = catalog.addManual({u"Other"_s, QString(), {u"other"_s, {}}});
        QVERIFY(second != id);

        QCOMPARE(ids(catalog.browsers()).last(2), (QStringList{id, second}));
        auto work = catalog.browser(id);
        QVERIFY(work);
        QCOMPARE(work->source, BrowserSource::Manual);
        QCOMPARE(work->name, u"Work"_s);
        QCOMPARE(work->command, (CommandTemplate{u"/opt/work/browser"_s, {u"--profile"_s, u"{url}"_s}}));

        QVERIFY(catalog.edit(id, {u"Work 2"_s, u"icon2"_s, {u"/opt/work2"_s, {}}}));
        work = catalog.browser(id);
        QCOMPARE(work->name, u"Work 2"_s);
        QCOMPARE(work->icon, u"icon2"_s);
        QCOMPARE(work->command, (CommandTemplate{u"/opt/work2"_s, {}}));
        QVERIFY(!catalog.resetToDefaults(id));

        // Manual browsers are independent of rescans.
        catalog.setDetected({});
        QCOMPARE(ids(catalog.browsers()), (QStringList{id, second}));

        QVERIFY(catalog.remove(id));
        QVERIFY(!catalog.browser(id));
        QVERIFY(std::ranges::none_of(catalog.settings(), [&id](const BrowserSetting &s) {
            return s.id == id;
        }));
    }

    void editDetectedStoresOnlyDifferences()
    {
        BrowserCatalog catalog(system());
        QVERIFY(catalog.edit(u"firefox.desktop"_s, {u"Firefox"_s, u"firefox-icon"_s, {u"firefox"_s, {u"--private-window"_s, u"{url}"_s}}}));
        const auto settings = catalog.settings();
        const auto it = std::ranges::find(settings, u"firefox.desktop"_s, &BrowserSetting::id);
        QVERIFY(it != settings.end());
        QCOMPARE(it->name, std::nullopt);
        QCOMPARE(it->icon, std::nullopt);
        QCOMPARE(it->program, std::nullopt);
        QCOMPARE(it->arguments, (QStringList{u"--private-window"_s, u"{url}"_s}));
        QVERIFY(catalog.browser(u"firefox.desktop"_s)->customized);
    }

    void resetRestoresDetectedDefaults()
    {
        BrowserCatalog catalog(system());
        QVERIFY(catalog.edit(u"vivaldi.desktop"_s, {u"V"_s, u"v"_s, {u"v"_s, {}}}));
        QVERIFY(catalog.setHidden(u"vivaldi.desktop"_s, true));
        QVERIFY(catalog.reorder({u"vivaldi.desktop"_s, u"chromium.desktop"_s, u"firefox.desktop"_s}));

        QVERIFY(catalog.resetToDefaults(u"vivaldi.desktop"_s));
        const auto vivaldi = catalog.browser(u"vivaldi.desktop"_s);
        QCOMPARE(vivaldi->name, u"Vivaldi"_s);
        QCOMPARE(vivaldi->icon, u"vivaldi-icon"_s);
        QCOMPARE(vivaldi->command, (CommandTemplate{u"vivaldi"_s, {u"{url}"_s}}));
        QVERIFY(!vivaldi->customized);
        // Reset only touches the desktop-file fields, not visibility or position.
        QVERIFY(vivaldi->hidden);
        QCOMPARE(ids(catalog.browsers()).first(), u"vivaldi.desktop"_s);
    }

    void reorder()
    {
        BrowserCatalog catalog(system());
        const QString manual = catalog.addManual({u"M"_s, QString(), {u"m"_s, {}}});
        QVERIFY(catalog.remove(u"chromium.desktop"_s));

        const QStringList order{manual, u"vivaldi.desktop"_s, u"firefox.desktop"_s};
        QVERIFY(catalog.reorder(order));
        QCOMPARE(ids(catalog.browsers()), order);

        const QStringList before = ids(catalog.browsers());
        QVERIFY(!catalog.reorder({manual, u"vivaldi.desktop"_s}));
        QVERIFY(!catalog.reorder({manual, u"vivaldi.desktop"_s, u"firefox.desktop"_s, u"chromium.desktop"_s}));
        QVERIFY(!catalog.reorder({manual, manual, u"vivaldi.desktop"_s}));
        QVERIFY(!catalog.reorder({manual, u"vivaldi.desktop"_s, u"unknown.desktop"_s}));
        QCOMPARE(ids(catalog.browsers()), before);

        // The tombstone is kept so the removed browser is not resurrected.
        const BrowserCatalog reloaded(system(), catalog.settings());
        QCOMPARE(ids(reloaded.browsers()), order);
    }

    void uninstalledBrowserKeepsSettingsForReinstall()
    {
        BrowserCatalog catalog(system());
        QVERIFY(catalog.edit(u"firefox.desktop"_s, {u"Fx"_s, u"firefox-icon"_s, {u"firefox"_s, {u"{url}"_s}}}));
        QVERIFY(catalog.reorder({u"firefox.desktop"_s, u"chromium.desktop"_s, u"vivaldi.desktop"_s}));

        auto withoutFirefox = system();
        withoutFirefox.removeAt(1);
        catalog.setDetected(withoutFirefox);
        QCOMPARE(ids(catalog.browsers()), (QStringList{u"chromium.desktop"_s, u"vivaldi.desktop"_s}));
        QVERIFY(!catalog.browserIds().contains(u"firefox.desktop"_s));
        QVERIFY(!catalog.setHidden(u"firefox.desktop"_s, true));

        QVERIFY(catalog.setHidden(u"vivaldi.desktop"_s, true)); // a mutation while uninstalled
        catalog.setDetected(system());
        QCOMPARE(ids(catalog.browsers()).first(), u"firefox.desktop"_s);
        QCOMPARE(catalog.browser(u"firefox.desktop"_s)->name, u"Fx"_s);
    }

    void unknownIdsAreRejected()
    {
        BrowserCatalog catalog(system());
        QVERIFY(!catalog.setHidden(u"nope"_s, true));
        QVERIFY(!catalog.remove(u"nope"_s));
        QVERIFY(!catalog.resetToDefaults(u"nope"_s));
        QVERIFY(!catalog.edit(u"nope"_s, {}));
        QVERIFY(catalog.settings().isEmpty());
    }

    void duplicateSettingsFirstWins()
    {
        BrowserSetting first{.id = u"firefox.desktop"_s};
        first.name = u"First"_s;
        BrowserSetting second{.id = u"firefox.desktop"_s};
        second.name = u"Second"_s;
        const BrowserCatalog catalog(system(), {first, second});
        QCOMPARE(catalog.browsers().size(), 3);
        QCOMPARE(catalog.browser(u"firefox.desktop"_s)->name, u"First"_s);
    }
};

QTEST_GUILESS_MAIN(TestBrowserCatalog)
#include "test_browsercatalog.moc"
