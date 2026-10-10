#include "config.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <cmath>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

constexpr int LockTimeoutMs = 1000;

QJsonArray toJsonArray(const QStringList &list)
{
    QJsonArray array;
    for (const QString &item : list) {
        array.append(item);
    }
    return array;
}

QJsonObject browserToJson(const BrowserSetting &setting)
{
    QJsonObject object{{u"id"_s, setting.id}};
    if (setting.manual) {
        object.insert(u"manual"_s, true);
    }
    if (setting.hidden) {
        object.insert(u"hidden"_s, true);
    }
    if (setting.removed) {
        object.insert(u"removed"_s, true);
    }
    if (setting.name) {
        object.insert(u"name"_s, *setting.name);
    }
    if (setting.icon) {
        object.insert(u"icon"_s, *setting.icon);
    }
    if (setting.program) {
        object.insert(u"program"_s, *setting.program);
    }
    if (setting.arguments) {
        object.insert(u"arguments"_s, toJsonArray(*setting.arguments));
    }
    return object;
}

QJsonObject ruleToJson(const Rule &rule)
{
    QJsonObject object{
        {u"enabled"_s, rule.enabled},
        {u"type"_s, ruleTypeToString(rule.type)},
        {u"pattern"_s, rule.pattern},
        {u"browser"_s, rule.browserId},
    };
    if (!rule.name.isEmpty()) {
        object.insert(u"name"_s, rule.name);
    }
    return object;
}

// Reads JSON values with errors that name the offending key, e.g. "rules[2].type".
class Reader
{
public:
    QString error;
    QStringList warnings;

    bool fail(const QString &path, const QString &message)
    {
        if (error.isEmpty()) {
            error = path.isEmpty() ? message : u"%1: %2"_s.arg(path, message);
        }
        return false;
    }

    void warnUnknownKeys(const QJsonObject &object, const QString &path, std::initializer_list<QLatin1StringView> known)
    {
        for (const QString &key : object.keys()) {
            if (std::ranges::find(known, key) == known.end()) {
                warnings.append(u"%1: unknown key ignored."_s.arg(join(path, key)));
            }
        }
    }

    static QString join(const QString &path, const QString &key)
    {
        return path.isEmpty() ? key : path + u'.' + key;
    }

    bool readBool(const QJsonObject &object, const QString &path, const QString &key, bool *out)
    {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) {
            return true;
        }
        if (!value.isBool()) {
            return fail(join(path, key), u"expected true or false."_s);
        }
        *out = value.toBool();
        return true;
    }

    bool readString(const QJsonObject &object, const QString &path, const QString &key, std::optional<QString> *out)
    {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) {
            return true;
        }
        if (!value.isString()) {
            return fail(join(path, key), u"expected a string."_s);
        }
        *out = value.toString();
        return true;
    }

    bool readRequiredString(const QJsonObject &object, const QString &path, const QString &key, QString *out)
    {
        std::optional<QString> value;
        if (!readString(object, path, key, &value)) {
            return false;
        }
        if (!value) {
            return fail(join(path, key), u"missing."_s);
        }
        *out = *value;
        return true;
    }

    bool readStringList(const QJsonObject &object, const QString &path, const QString &key, std::optional<QStringList> *out)
    {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) {
            return true;
        }
        if (!value.isArray()) {
            return fail(join(path, key), u"expected an array of strings."_s);
        }
        QStringList list;
        for (const QJsonValue &item : value.toArray()) {
            if (!item.isString()) {
                return fail(join(path, key), u"expected an array of strings."_s);
            }
            list.append(item.toString());
        }
        *out = list;
        return true;
    }

    bool readArray(const QJsonObject &object, const QString &key, QJsonArray *out)
    {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) {
            return true;
        }
        if (!value.isArray()) {
            return fail(key, u"expected an array."_s);
        }
        *out = value.toArray();
        return true;
    }
};

bool readBrowser(Reader &reader, const QJsonValue &value, const QString &path, BrowserSetting *out)
{
    if (!value.isObject()) {
        return reader.fail(path, u"expected an object."_s);
    }
    const QJsonObject object = value.toObject();
    reader.warnUnknownKeys(object, path, {"id"_L1, "manual"_L1, "hidden"_L1, "removed"_L1, "name"_L1, "icon"_L1, "program"_L1, "arguments"_L1});

    BrowserSetting setting;
    if (!reader.readRequiredString(object, path, u"id"_s, &setting.id) || !reader.readBool(object, path, u"manual"_s, &setting.manual)
        || !reader.readBool(object, path, u"hidden"_s, &setting.hidden) || !reader.readBool(object, path, u"removed"_s, &setting.removed)
        || !reader.readString(object, path, u"name"_s, &setting.name) || !reader.readString(object, path, u"icon"_s, &setting.icon)
        || !reader.readString(object, path, u"program"_s, &setting.program)
        || !reader.readStringList(object, path, u"arguments"_s, &setting.arguments)) {
        return false;
    }
    if (setting.id.isEmpty()) {
        return reader.fail(path + u".id"_s, u"must not be empty."_s);
    }
    if (setting.manual && setting.removed) {
        return reader.fail(path, u"\"removed\" only applies to detected browsers; delete the entry instead."_s);
    }
    if (setting.manual && setting.program.value_or(QString()).isEmpty()) {
        return reader.fail(path + u".program"_s, u"manual browsers need a program."_s);
    }
    *out = setting;
    return true;
}

bool readRule(Reader &reader, const QJsonValue &value, const QString &path, Rule *out)
{
    if (!value.isObject()) {
        return reader.fail(path, u"expected an object."_s);
    }
    const QJsonObject object = value.toObject();
    reader.warnUnknownKeys(object, path, {"name"_L1, "enabled"_L1, "type"_L1, "pattern"_L1, "browser"_L1});

    Rule rule;
    QString type;
    std::optional<QString> name;
    std::optional<QString> browser;
    if (!reader.readString(object, path, u"name"_s, &name) || !reader.readBool(object, path, u"enabled"_s, &rule.enabled)
        || !reader.readRequiredString(object, path, u"type"_s, &type) || !reader.readRequiredString(object, path, u"pattern"_s, &rule.pattern)
        || !reader.readString(object, path, u"browser"_s, &browser)) {
        return false;
    }
    const auto ruleType = ruleTypeFromString(type);
    if (!ruleType) {
        return reader.fail(path + u".type"_s, u"unknown rule type “%1”; use domain, subdomain, glob or regex."_s.arg(type));
    }
    rule.type = *ruleType;
    rule.name = name.value_or(QString());
    rule.browserId = browser.value_or(QString());
    *out = rule;
    return true;
}

// Values that could make an unusable window are rejected, not adjusted.
bool readChooser(Reader &reader, const QJsonValue &value, ChooserSettings *out)
{
    const QString path = u"chooser"_s;
    if (!value.isObject()) {
        return reader.fail(path, u"expected an object."_s);
    }
    const QJsonObject object = value.toObject();
    reader.warnUnknownKeys(object, path, {"style"_L1, "iconSize"_L1, "showBrowserNames"_L1, "closeOnFocusLoss"_L1});

    ChooserSettings settings;
    std::optional<QString> style;
    if (!reader.readString(object, path, u"style"_s, &style)) {
        return false;
    }
    if (style) {
        const auto parsed = chooserStyleFromString(*style);
        if (!parsed) {
            return reader.fail(path + u".style"_s, u"unknown style “%1”; use vertical or horizontal."_s.arg(*style));
        }
        settings.style = *parsed;
    }

    const QJsonValue size = object.value("iconSize"_L1);
    if (!size.isUndefined()) {
        const double number = size.toDouble(-1);
        if (!size.isDouble() || number != std::floor(number) || number < ChooserSettings::MinIconSize
            || number > ChooserSettings::MaxIconSize) {
            return reader.fail(path + u".iconSize"_s,
                               u"expected a whole number of pixels from %1 to %2."_s.arg(ChooserSettings::MinIconSize).arg(ChooserSettings::MaxIconSize));
        }
        settings.iconSize = int(number);
    }

    if (!reader.readBool(object, path, u"showBrowserNames"_s, &settings.showBrowserNames)
        || !reader.readBool(object, path, u"closeOnFocusLoss"_s, &settings.closeOnFocusLoss)) {
        return false;
    }
    *out = settings;
    return true;
}

QByteArray digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

// An error when the file is not as it was loaded, or is malformed.
QString checkUnchanged(const QString &path, const std::optional<QByteArray> &loadedDigest)
{
    std::optional<QByteArray> currentDigest;
    QFile current(path);
    if (current.exists()) {
        if (!current.open(QIODevice::ReadOnly)) {
            return u"Cannot read %1: %2"_s.arg(path, current.errorString());
        }
        const QByteArray bytes = current.readAll();
        if (!configFromJson(bytes).config) {
            return u"%1 is not a valid configuration file; fix or move it before saving."_s.arg(path);
        }
        currentDigest = digest(bytes);
    }
    if (currentDigest != loadedDigest) {
        return u"%1 was changed by another program since it was loaded; reload it and try again."_s.arg(path);
    }
    return {};
}

} // namespace

QString chooserStyleToString(ChooserStyle style)
{
    return style == ChooserStyle::Horizontal ? u"horizontal"_s : u"vertical"_s;
}

std::optional<ChooserStyle> chooserStyleFromString(const QString &text)
{
    if (text == u"vertical"_s) {
        return ChooserStyle::Vertical;
    }
    if (text == u"horizontal"_s) {
        return ChooserStyle::Horizontal;
    }
    return std::nullopt;
}

QByteArray configToJson(const Config &config)
{
    QJsonArray browsers;
    for (const BrowserSetting &setting : config.browsers) {
        browsers.append(browserToJson(setting));
    }
    QJsonArray rules;
    for (const Rule &rule : config.rules) {
        rules.append(ruleToJson(rule));
    }
    const QJsonObject root{
        {u"version"_s, ConfigVersion},
        {u"browsers"_s, browsers},
        {u"rules"_s, rules},
        {u"privacy"_s,
         QJsonObject{
             {u"stripTrackingParameters"_s, config.privacy.stripTrackingParameters},
             {u"unwrapRedirects"_s, config.privacy.unwrapRedirects},
         }},
        {u"chooser"_s,
         QJsonObject{
             {u"style"_s, chooserStyleToString(config.chooser.style)},
             {u"iconSize"_s, config.chooser.iconSize},
             {u"showBrowserNames"_s, config.chooser.showBrowserNames},
             {u"closeOnFocusLoss"_s, config.chooser.closeOnFocusLoss},
         }},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

ConfigParseResult configFromJson(const QByteArray &json)
{
    if (json.trimmed().isEmpty()) {
        return {std::nullopt, u"The file is empty."_s, {}};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        const qsizetype offset = std::min<qsizetype>(parseError.offset, json.size());
        const qsizetype lineStart = json.lastIndexOf('\n', offset - 1) + 1;
        return {std::nullopt,
                u"Invalid JSON at line %1, column %2: %3."_s.arg(json.left(offset).count('\n') + 1).arg(offset - lineStart + 1).arg(parseError.errorString()),
                {}};
    }
    if (!document.isObject()) {
        return {std::nullopt, u"The top level must be a JSON object."_s, {}};
    }

    const QJsonObject root = document.object();
    Reader reader;
    const auto failed = [&reader] {
        return ConfigParseResult{std::nullopt, reader.error, reader.warnings};
    };

    const QJsonValue version = root.value("version"_L1);
    if (version.isUndefined()) {
        reader.fail(u"version"_s, u"missing; expected %1."_s.arg(ConfigVersion));
        return failed();
    }
    const double number = version.toDouble(-1);
    if (!version.isDouble() || number != std::floor(number)) {
        reader.fail(u"version"_s, u"expected the integer %1."_s.arg(ConfigVersion));
        return failed();
    }
    if (number > ConfigVersion) {
        reader.fail(u"version"_s, u"the file was written by a newer version (%1) of the application."_s.arg(number));
        return failed();
    }
    if (number != ConfigVersion) {
        reader.fail(u"version"_s, u"unsupported version %1."_s.arg(number));
        return failed();
    }

    reader.warnUnknownKeys(root, QString(), {"version"_L1, "browsers"_L1, "rules"_L1, "privacy"_L1, "chooser"_L1});
    Config config;

    QJsonArray browsers;
    if (!reader.readArray(root, u"browsers"_s, &browsers)) {
        return failed();
    }
    QSet<QString> ids;
    for (qsizetype i = 0; i < browsers.size(); ++i) {
        const QString path = u"browsers[%1]"_s.arg(i);
        BrowserSetting setting;
        if (!readBrowser(reader, browsers.at(i), path, &setting)) {
            return failed();
        }
        if (ids.contains(setting.id)) {
            reader.fail(path + u".id"_s, u"“%1” is listed more than once."_s.arg(setting.id));
            return failed();
        }
        ids.insert(setting.id);
        config.browsers.append(setting);
    }

    QJsonArray rules;
    if (!reader.readArray(root, u"rules"_s, &rules)) {
        return failed();
    }
    for (qsizetype i = 0; i < rules.size(); ++i) {
        Rule rule;
        if (!readRule(reader, rules.at(i), u"rules[%1]"_s.arg(i), &rule)) {
            return failed();
        }
        config.rules.append(rule);
    }

    const QJsonValue privacy = root.value("privacy"_L1);
    if (!privacy.isUndefined()) {
        if (!privacy.isObject()) {
            reader.fail(u"privacy"_s, u"expected an object."_s);
            return failed();
        }
        const QJsonObject object = privacy.toObject();
        reader.warnUnknownKeys(object, u"privacy"_s, {"stripTrackingParameters"_L1, "unwrapRedirects"_L1});
        if (!reader.readBool(object, u"privacy"_s, u"stripTrackingParameters"_s, &config.privacy.stripTrackingParameters)
            || !reader.readBool(object, u"privacy"_s, u"unwrapRedirects"_s, &config.privacy.unwrapRedirects)) {
            return failed();
        }
    }

    const QJsonValue chooser = root.value("chooser"_L1);
    if (!chooser.isUndefined() && !readChooser(reader, chooser, &config.chooser)) {
        return failed();
    }

    return {config, QString(), reader.warnings};
}

ConfigStore::ConfigStore(const QString &path)
    : m_path(path)
{
}

QString ConfigStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u"/klinker/config.json"_s;
}

QString ConfigStore::path() const
{
    return m_path;
}

ConfigStore::LoadResult ConfigStore::load()
{
    m_loaded = true;
    m_loadedDigest.reset();

    QFile file(m_path);
    if (!file.exists()) {
        return {LoadStatus::Missing, Config(), QString(), {}};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        // Remember that something is there so that save() re-checks it.
        m_loadedDigest = QByteArray();
        return {LoadStatus::Invalid, Config(), u"Cannot read %1: %2"_s.arg(m_path, file.errorString()), {}};
    }
    const QByteArray bytes = file.readAll();
    m_loadedDigest = digest(bytes);

    const ConfigParseResult parsed = configFromJson(bytes);
    if (!parsed.config) {
        return {LoadStatus::Invalid, Config(), u"%1: %2"_s.arg(m_path, parsed.error), parsed.warnings};
    }
    return {LoadStatus::Loaded, *parsed.config, QString(), parsed.warnings};
}

ConfigStore::SaveResult ConfigStore::save(const Config &config)
{
    if (!m_loaded) {
        return {false, u"The configuration must be loaded before it is saved."_s};
    }

    // Never write what load() would reject: the existing file stays as it is.
    const QByteArray json = configToJson(config);
    const ConfigParseResult outgoing = configFromJson(json);
    if (!outgoing.config) {
        return {false, u"The settings were not saved because they are invalid: %1"_s.arg(outgoing.error)};
    }

    const QString directory = QFileInfo(m_path).absolutePath();
    if (!QDir().mkpath(directory)) {
        return {false, u"Cannot create the directory %1."_s.arg(directory)};
    }

    QLockFile lock(m_path + u".lock"_s);
    if (!lock.tryLock(LockTimeoutMs)) {
        return {false,
                lock.error() == QLockFile::LockFailedError ? u"%1 is being saved by another process; try again."_s.arg(m_path)
                                                           : u"Cannot lock %1 for saving."_s.arg(m_path)};
    }

    if (const QString error = checkUnchanged(m_path, m_loadedDigest); !error.isEmpty()) {
        return {false, error};
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
        return {false, u"Cannot save %1: %2"_s.arg(m_path, file.errorString())};
    }
    m_loadedDigest = digest(json);
    return {true, QString()};
}

} // namespace klinker
