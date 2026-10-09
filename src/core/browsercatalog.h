#pragma once

#include "browserdiscovery.h"
#include "execcommand.h"

#include <QList>
#include <QSet>
#include <QString>

#include <optional>

namespace kbs
{

// One entry of the ordered "browsers" config array. For detected browsers only
// the fields the user changed are set; manual browsers store everything.
struct BrowserSetting {
    QString id;
    bool manual = false;
    bool hidden = false;
    bool removed = false; // detected browsers only: keeps a rescan from bringing it back
    std::optional<QString> name;
    std::optional<QString> icon;
    std::optional<QString> program;
    std::optional<QStringList> arguments;

    bool operator==(const BrowserSetting &) const = default;
};

enum class BrowserSource {
    Detected,
    Manual,
};

struct Browser {
    QString id;
    BrowserSource source = BrowserSource::Detected;
    QString name;
    QString icon;
    CommandTemplate command;
    bool hidden = false; // hidden from the chooser; rules may still target it
    bool customized = false; // a detected browser with user overrides
};

struct BrowserFields {
    QString name;
    QString icon;
    CommandTemplate command;
};

class BrowserCatalog
{
public:
    explicit BrowserCatalog(const QList<DetectedBrowser> &detected = {}, const QList<BrowserSetting> &settings = {});

    // Replaces the detected browsers after a rescan; user settings are kept.
    void setDetected(const QList<DetectedBrowser> &detected);

    // All usable browsers in user order, including hidden ones.
    QList<Browser> browsers() const;
    QList<Browser> visibleBrowsers() const;
    std::optional<Browser> browser(const QString &id) const;
    QSet<QString> browserIds() const;

    QString addManual(const BrowserFields &fields);
    bool edit(const QString &id, const BrowserFields &fields);
    bool setHidden(const QString &id, bool hidden);
    bool remove(const QString &id);
    bool resetToDefaults(const QString &id);
    // orderedIds must contain exactly the IDs of browsers().
    bool reorder(const QStringList &orderedIds);

    QList<BrowserSetting> settings() const;

private:
    const DetectedBrowser *detected(const QString &id) const;
    BrowserSetting *setting(const QString &id);
    void materializeOrder();

    QList<DetectedBrowser> m_detected;
    QList<BrowserSetting> m_settings;
};

} // namespace kbs
