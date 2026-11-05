#pragma once

#include "browsercatalog.h"
#include "rules.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace klinker
{

inline constexpr int ConfigVersion = 1;

struct PrivacySettings {
    bool stripTrackingParameters = false;
    bool unwrapRedirects = false;

    bool operator==(const PrivacySettings &) const = default;
};

enum class ChooserStyle {
    Vertical, // a list with names and details
    Horizontal, // a strip of browser icons (the default)
};

// "vertical" or "horizontal", as stored in the file.
QString chooserStyleToString(ChooserStyle style);
std::optional<ChooserStyle> chooserStyleFromString(const QString &text);

struct ChooserSettings {
    static constexpr int MinIconSize = 24;
    static constexpr int MaxIconSize = 128;
    static constexpr int DefaultIconSize = 48;

    ChooserStyle style = ChooserStyle::Horizontal;
    // Horizontal only, in logical pixels; kept while the vertical style is used.
    int iconSize = DefaultIconSize;
    bool showBrowserNames = true; // horizontal only
    // Cancel the link when another application takes the focus.
    bool closeOnFocusLoss = false;

    bool operator==(const ChooserSettings &) const = default;
};

// Everything persisted. Deliberately no URL history.
struct Config {
    QList<BrowserSetting> browsers;
    QList<Rule> rules;
    PrivacySettings privacy;
    ChooserSettings chooser;

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
    explicit ConfigStore(const QString &path);

    // $XDG_CONFIG_HOME/klinker/config.json
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

} // namespace klinker
