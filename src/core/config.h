#pragma once

#include "browsercatalog.h"
#include "rules.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace kbs
{

inline constexpr int ConfigVersion = 1;

struct PrivacySettings {
    bool stripTrackingParameters = false;
    bool unwrapRedirects = false;

    bool operator==(const PrivacySettings &) const = default;
};

// Everything persisted. Deliberately no URL history.
struct Config {
    QList<BrowserSetting> browsers;
    QList<Rule> rules;
    PrivacySettings privacy;

    bool operator==(const Config &) const = default;
};

QByteArray configToJson(const Config &config);

struct ConfigParseResult {
    std::optional<Config> config; // empty when the document is invalid
    QString error;
    QStringList warnings; // e.g. unknown keys, which are ignored
};

ConfigParseResult configFromJson(const QByteArray &json);

// Loads and saves the config file. Saving refuses to write a configuration that
// loading would reject and to replace a file that is malformed or was changed by
// someone else since it was loaded, serialises writers with a lock file and
// replaces the file atomically.
class ConfigStore
{
public:
    explicit ConfigStore(const QString &path = defaultPath());

    // $XDG_CONFIG_HOME/kde-browser-switcher/config.json
    static QString defaultPath();
    QString path() const;

    enum class LoadStatus {
        Loaded,
        Missing, // no file yet: defaults
        Invalid, // unreadable or malformed: defaults, and saving is refused until fixed
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        Config config;
        QString error;
        QStringList warnings;
    };

    LoadResult load();

    struct SaveResult {
        bool ok = false;
        QString error;
    };

    SaveResult save(const Config &config);

private:
    QString m_path;
    bool m_loaded = false;
    std::optional<QByteArray> m_loadedDigest; // empty: the file did not exist
};

} // namespace kbs
