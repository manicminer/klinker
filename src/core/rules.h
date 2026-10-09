#pragma once

#include <QList>
#include <QSet>
#include <QString>

#include <memory>
#include <optional>

#include <regex.h>

namespace kbs
{

enum class RuleType {
    Domain, // canonical host equals the pattern
    Subdomain, // canonical host equals the pattern or ends with "." + pattern
    Glob, // whole URL, '*' also crosses '/'
    Regex, // POSIX extended regular expression, unanchored search
};

QString ruleTypeToString(RuleType type);
std::optional<RuleType> ruleTypeFromString(const QString &text);

struct Rule {
    QString name;
    bool enabled = true;
    RuleType type = RuleType::Domain;
    QString pattern;
    QString browserId;

    bool operator==(const Rule &) const = default;
};

// Host as the browser will see it: no credentials or port, percent-decoded,
// IDN converted to ASCII, lower case, without a trailing dot. For http(s),
// ftp and ws(s) a host whose last label is a number is parsed as IPv4 the way
// the WHATWG URL parser does, so 127.1 and 0x7f000001 become 127.0.0.1. Empty
// if the URL has no valid host.
QString canonicalHostOfUrl(const QString &url);

// Canonical form of a domain rule pattern, or an empty string with *error set.
// Numeric patterns are canonicalised like hosts, so "127.1" means 127.0.0.1.
QString canonicalDomainPattern(const QString &pattern, QString *error = nullptr);

// Matches the glob syntax described in docs/implementation-plan.md against the whole text.
bool globMatches(QStringView pattern, QStringView text);

// A compiled rule. Invalid rules never match.
class RuleMatcher
{
public:
    explicit RuleMatcher(const Rule &rule);

    bool isValid() const;
    QString error() const;
    bool matches(const QString &url) const;

private:
    Rule m_rule;
    QString m_domain;
    QString m_error;
    std::shared_ptr<regex_t> m_regex; // compiled POSIX ERE, shared between copies
};

struct RouteResult {
    QString browserId; // empty: no rule matched, show the chooser
    int ruleIndex = -1;

    bool matched() const
    {
        return ruleIndex >= 0;
    }
};

// Compiles a rule list once for routing, testing and diagnostics.
class RuleEngine
{
public:
    RuleEngine(const QList<Rule> &rules, const QSet<QString> &availableBrowserIds);

    // Empty when the rule can route. Disabled rules still report problems.
    QString ruleError(qsizetype index) const;

    // First enabled, valid, matching rule wins.
    RouteResult route(const QString &url) const;

private:
    QList<Rule> m_rules;
    QList<RuleMatcher> m_matchers;
    QList<QString> m_errors;
};

} // namespace kbs
