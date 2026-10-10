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

enum class ChooserStyle {
    Vertical, // a list with names and details (the default)
    Horizontal, // a strip of browser icons
};

// "vertical" or "horizontal", as stored in the file.
QString chooserStyleToString(ChooserStyle style);
std::optional<ChooserStyle> chooserStyleFromString(const QString &text);

struct ChooserSettings {
    static constexpr int MinIconSize = 24;
    static constexpr int MaxIconSize = 128;
    static constexpr int DefaultIconSize = 48;

    ChooserStyle style = ChooserStyle::Vertical;
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
//
// With a legacy path, a missing file is read from there instead (the
// configuration of an earlier version). The legacy file is only ever read: the
// first save writes to path(), and only if neither file changed since loading.
class ConfigStore
{
public:
    explicit ConfigStore(const QString &path, const QString &legacyPath = QString());

    // $XDG_CONFIG_HOME/klinker/config.json
    static QString defaultPath();
    // $XDG_CONFIG_HOME/kde-browser-switcher/config.json, from before the rename.
    static QString legacyDefaultPath();
    QString path() const;
    QString legacyPath() const;
    // The file the last load() read from: path() or legacyPath().
    QString sourcePath() const;

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
    LoadResult read(const QString &path, std::optional<QByteArray> *digest);

    QString m_path;
    QString m_legacyPath;
    QString m_sourcePath;
    bool m_loaded = false;
    std::optional<QByteArray> m_loadedDigest; // empty: the file did not exist
    // Set while path() did not exist at loading: then the legacy file is part
    // of what was loaded, even if it did not exist either.
    bool m_legacyLoaded = false;
    std::optional<QByteArray> m_legacyDigest;
};

} // namespace kbs
