#include "browsercatalog.h"

#include <QUuid>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{
BrowserSetting plainSetting(const QString &id)
{
    BrowserSetting setting;
    setting.id = id;
    return setting;
}
} // namespace

BrowserCatalog::BrowserCatalog(const QList<DetectedBrowser> &detected, const QList<BrowserSetting> &settings)
    : m_detected(detected)
    , m_settings(settings)
{
}

void BrowserCatalog::setDetected(const QList<DetectedBrowser> &detected)
{
    m_detected = detected;
}

QList<Browser> BrowserCatalog::browsers() const
{
    QList<Browser> result;
    QSet<QString> listed;

    for (const BrowserSetting &s : m_settings) {
        if (listed.contains(s.id)) {
            continue;
        }
        listed.insert(s.id);

        if (s.manual) {
            result.append({s.id,
                           BrowserSource::Manual,
                           s.name.value_or(QString()),
                           s.icon.value_or(QString()),
                           {s.program.value_or(QString()), s.arguments.value_or(QStringList())},
                           s.hidden,
                           false});
        } else if (const DetectedBrowser *d = detected(s.id); d && !s.removed) {
            result.append({s.id,
                           BrowserSource::Detected,
                           s.name.value_or(d->name),
                           s.icon.value_or(d->icon),
                           {s.program.value_or(d->command.program), s.arguments.value_or(d->command.arguments)},
                           s.hidden,
                           s.name || s.icon || s.program || s.arguments});
        }
    }

    for (const DetectedBrowser &d : m_detected) {
        if (!listed.contains(d.desktopId)) {
            result.append({d.desktopId, BrowserSource::Detected, d.name, d.icon, d.command, false, false});
        }
    }
    return result;
}

QList<Browser> BrowserCatalog::visibleBrowsers() const
{
    QList<Browser> result = browsers();
    result.removeIf([](const Browser &b) {
        return b.hidden;
    });
    return result;
}

std::optional<Browser> BrowserCatalog::browser(const QString &id) const
{
    for (const Browser &b : browsers()) {
        if (b.id == id) {
            return b;
        }
    }
    return std::nullopt;
}

QSet<QString> BrowserCatalog::browserIds() const
{
    QSet<QString> result;
    for (const Browser &b : browsers()) {
        result.insert(b.id);
    }
    return result;
}

QString BrowserCatalog::addManual(const BrowserFields &fields)
{
    const QSet<QString> taken = [this] {
        QSet<QString> ids = browserIds();
        for (const BrowserSetting &s : std::as_const(m_settings)) {
            ids.insert(s.id);
        }
        return ids;
    }();
    QString id;
    do {
        id = u"manual-"_s + QUuid::createUuid().toString(QUuid::Id128).left(8);
    } while (taken.contains(id));

    materializeOrder();
    BrowserSetting s = plainSetting(id);
    s.manual = true;
    s.name = fields.name;
    s.icon = fields.icon;
    s.program = fields.command.program;
    s.arguments = fields.command.arguments;
    m_settings.append(s);
    return id;
}

bool BrowserCatalog::edit(const QString &id, const BrowserFields &fields)
{
    const auto current = browser(id);
    if (!current) {
        return false;
    }
    materializeOrder();
    BrowserSetting *s = setting(id);

    if (current->source == BrowserSource::Manual) {
        s->name = fields.name;
        s->icon = fields.icon;
        s->program = fields.command.program;
        s->arguments = fields.command.arguments;
        return true;
    }

    // Store only what differs so untouched fields keep following the desktop file.
    const DetectedBrowser *d = detected(id);
    const auto overrideIfChanged = [](const auto &value, const auto &original) {
        using T = std::decay_t<decltype(value)>;
        return value == original ? std::optional<T>() : std::optional<T>(value);
    };
    s->name = overrideIfChanged(fields.name, d->name);
    s->icon = overrideIfChanged(fields.icon, d->icon);
    s->program = overrideIfChanged(fields.command.program, d->command.program);
    s->arguments = overrideIfChanged(fields.command.arguments, d->command.arguments);
    return true;
}

bool BrowserCatalog::setHidden(const QString &id, bool hidden)
{
    if (!browser(id)) {
        return false;
    }
    materializeOrder();
    setting(id)->hidden = hidden;
    return true;
}

bool BrowserCatalog::remove(const QString &id)
{
    const auto current = browser(id);
    if (!current) {
        return false;
    }
    if (current->source == BrowserSource::Manual) {
        m_settings.removeIf([&id](const BrowserSetting &s) {
            return s.id == id;
        });
        return true;
    }
    materializeOrder();
    BrowserSetting *s = setting(id);
    *s = plainSetting(id);
    s->removed = true;
    return true;
}

bool BrowserCatalog::resetToDefaults(const QString &id)
{
    const auto current = browser(id);
    if (!current || current->source != BrowserSource::Detected) {
        return false;
    }
    if (BrowserSetting *s = setting(id)) {
        s->name.reset();
        s->icon.reset();
        s->program.reset();
        s->arguments.reset();
    }
    return true;
}

bool BrowserCatalog::reorder(const QStringList &orderedIds)
{
    QStringList current;
    for (const Browser &b : browsers()) {
        current.append(b.id);
    }
    const QSet<QString> requested(orderedIds.cbegin(), orderedIds.cend());
    if (orderedIds.size() != current.size() || requested.size() != orderedIds.size()
        || requested != QSet<QString>(current.cbegin(), current.cend())) {
        return false;
    }

    QList<BrowserSetting> reordered;
    for (const QString &id : orderedIds) {
        const BrowserSetting *s = setting(id);
        reordered.append(s ? *s : plainSetting(id));
    }
    // Keep tombstones and settings of uninstalled browsers.
    for (const BrowserSetting &s : std::as_const(m_settings)) {
        if (!requested.contains(s.id)) {
            reordered.append(s);
        }
    }
    m_settings = reordered;
    return true;
}

QList<BrowserSetting> BrowserCatalog::settings() const
{
    return m_settings;
}

const DetectedBrowser *BrowserCatalog::detected(const QString &id) const
{
    const auto it = std::ranges::find(m_detected, id, &DetectedBrowser::desktopId);
    return it == m_detected.cend() ? nullptr : &*it;
}

BrowserSetting *BrowserCatalog::setting(const QString &id)
{
    const auto it = std::ranges::find(m_settings, id, &BrowserSetting::id);
    return it == m_settings.end() ? nullptr : &*it;
}

// Gives every listed browser a settings entry so that later edits cannot change
// the relative order of browsers that were only ordered by detection.
void BrowserCatalog::materializeOrder()
{
    for (const Browser &b : browsers()) {
        if (!setting(b.id)) {
            m_settings.append(plainSetting(b.id));
        }
    }
}

} // namespace kbs
