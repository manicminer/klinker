#include "rules.h"

#include <QByteArray>
#include <QUrl>

#include <algorithm>

#include <regex.h>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

bool isSpecialScheme(const QString &scheme)
{
    return scheme == "http"_L1 || scheme == "https"_L1 || scheme == "ftp"_L1 || scheme == "ws"_L1 || scheme == "wss"_L1;
}

bool isValidScheme(QStringView scheme)
{
    if (scheme.isEmpty() || !(scheme.front().isLetter() && scheme.front().unicode() < 0x80)) {
        return false;
    }
    return std::ranges::all_of(scheme, [](QChar c) {
        return c.unicode() < 0x80 && (c.isLetterOrNumber() || c == u'+' || c == u'-' || c == u'.');
    });
}

bool isForbiddenHostChar(QChar c)
{
    return c.unicode() <= 0x20 || c.unicode() == 0x7f || QStringView(u"#%/:<>?@[\\]^|").contains(c);
}

// The WHATWG URL parser drops ASCII tab and newline anywhere and C0 controls or
// spaces at either end; do the same so routing sees the host the browser sees.
QString browserNormalised(const QString &url)
{
    QString result = url;
    result.removeIf([](QChar c) {
        return c == u'\t' || c == u'\n' || c == u'\r';
    });
    qsizetype begin = 0;
    qsizetype end = result.size();
    while (begin < end && result.at(begin).unicode() <= 0x20) {
        ++begin;
    }
    while (end > begin && result.at(end - 1).unicode() <= 0x20) {
        --end;
    }
    return result.sliced(begin, end - begin);
}

// The WHATWG "IPv4 number parser". Values of 2^32 and above saturate there,
// which is enough to reject them. Nothing when the part is not a number.
std::optional<quint64> parseIpv4Number(QStringView part)
{
    if (part.isEmpty()) {
        return std::nullopt;
    }
    int radix = 10;
    if (part.size() >= 2 && part[0] == u'0' && (part[1] == u'x' || part[1] == u'X')) {
        radix = 16;
        part = part.sliced(2);
    } else if (part.size() >= 2 && part[0] == u'0') {
        radix = 8;
        part = part.sliced(1);
    }
    constexpr quint64 limit = quint64(1) << 32;
    quint64 value = 0;
    for (const QChar c : part) {
        const char16_t u = c.unicode();
        const int digit = u >= u'0' && u <= u'9' ? u - u'0' : u >= u'a' && u <= u'f' ? u - u'a' + 10 : u >= u'A' && u <= u'F' ? u - u'A' + 10 : radix;
        if (digit >= radix) {
            return std::nullopt;
        }
        value = std::min(value * radix + digit, limit);
    }
    return value;
}

// The WHATWG "ends in a number" check: such a host must be an IPv4 address.
bool endsInNumber(QStringView host)
{
    QList<QStringView> parts = host.split(u'.');
    if (parts.last().isEmpty()) {
        if (parts.size() == 1) {
            return false;
        }
        parts.removeLast();
    }
    const QStringView last = parts.last();
    if (!last.isEmpty() && std::ranges::all_of(last, [](QChar c) {
            return c >= u'0' && c <= u'9';
        })) {
        return true;
    }
    return parseIpv4Number(last).has_value();
}

// The WHATWG IPv4 parser, so that 127.1, 0x7f000001 and 2130706433 all become
// 127.0.0.1. Empty when the host is not a valid IPv4 address.
QString canonicalIpv4(QStringView host)
{
    QList<QStringView> parts = host.split(u'.');
    if (parts.size() > 1 && parts.last().isEmpty()) {
        parts.removeLast();
    }
    if (parts.size() > 4) {
        return {};
    }
    QList<quint64> numbers;
    for (const QStringView part : std::as_const(parts)) {
        const auto number = parseIpv4Number(part);
        if (!number) {
            return {};
        }
        numbers.append(*number);
    }
    if (std::any_of(numbers.cbegin(), numbers.cend() - 1, [](quint64 n) {
            return n > 255;
        })) {
        return {};
    }
    if (numbers.last() >= quint64(1) << (8 * (5 - numbers.size()))) {
        return {};
    }
    quint64 address = numbers.last();
    for (qsizetype i = 0; i + 1 < numbers.size(); ++i) {
        address += numbers[i] << (8 * (3 - i));
    }
    return u"%1.%2.%3.%4"_s.arg(address >> 24).arg((address >> 16) & 0xff).arg((address >> 8) & 0xff).arg(address & 0xff);
}

QString aceLower(const QString &host)
{
    return QString::fromLatin1(QUrl::toAce(host)).toLower();
}

QString withoutTrailingDot(QString host)
{
    if (host.endsWith(u'.')) {
        host.chop(1);
    }
    return host;
}

} // namespace

QString ruleTypeToString(RuleType type)
{
    switch (type) {
    case RuleType::Domain:
        return u"domain"_s;
    case RuleType::Subdomain:
        return u"subdomain"_s;
    case RuleType::Glob:
        return u"glob"_s;
    case RuleType::Regex:
        return u"regex"_s;
    }
    return {};
}

std::optional<RuleType> ruleTypeFromString(const QString &text)
{
    for (const auto type : {RuleType::Domain, RuleType::Subdomain, RuleType::Glob, RuleType::Regex}) {
        if (ruleTypeToString(type) == text) {
            return type;
        }
    }
    return std::nullopt;
}

QString canonicalHostOfUrl(const QString &input)
{
    const QString url = browserNormalised(input);

    const qsizetype colon = url.indexOf(u':');
    if (colon <= 0 || !isValidScheme(QStringView(url).first(colon))) {
        return {};
    }
    const QString scheme = url.first(colon).toLower();
    const bool special = isSpecialScheme(scheme);

    QStringView rest = QStringView(url).sliced(colon + 1);
    if (special) {
        while (!rest.isEmpty() && (rest.front() == u'/' || rest.front() == u'\\')) {
            rest = rest.sliced(1);
        }
    } else if (rest.startsWith(u"//")) {
        rest = rest.sliced(2);
    } else {
        return {};
    }

    qsizetype authorityEnd = 0;
    while (authorityEnd < rest.size()) {
        const QChar c = rest.at(authorityEnd);
        if (c == u'/' || c == u'?' || c == u'#' || (special && c == u'\\')) {
            break;
        }
        ++authorityEnd;
    }
    const QStringView authority = rest.first(authorityEnd);
    const QStringView hostAndPort = authority.sliced(authority.lastIndexOf(u'@') + 1);

    if (hostAndPort.startsWith(u'[')) {
        const qsizetype close = hostAndPort.indexOf(u']');
        if (close < 2) {
            return {};
        }
        const QStringView address = hostAndPort.sliced(1, close - 1);
        const bool plausible = std::ranges::all_of(address, [](QChar c) {
            return c.unicode() < 0x80 && (c.isDigit() || c == u':' || c == u'.' || QStringView(u"abcdefABCDEF").contains(c));
        });
        return plausible ? hostAndPort.first(close + 1).toString().toLower() : QString();
    }

    const qsizetype portStart = hostAndPort.indexOf(u':');
    const QStringView rawHost = portStart < 0 ? hostAndPort : hostAndPort.first(portStart);
    const QString host = QString::fromUtf8(QByteArray::fromPercentEncoding(rawHost.toUtf8()));
    if (host.isEmpty() || std::ranges::any_of(host, isForbiddenHostChar)) {
        return {};
    }
    const QString ace = aceLower(host);
    if (special && endsInNumber(ace)) {
        return canonicalIpv4(ace);
    }
    return withoutTrailingDot(ace);
}

QString canonicalDomainPattern(const QString &pattern, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QString();
    };

    QString host = pattern.trimmed().toLower();
    if (host.isEmpty()) {
        return fail(u"Enter a domain name such as example.com."_s);
    }
    if (std::ranges::any_of(host, [](QChar c) {
            return isForbiddenHostChar(c) || c == u'*';
        })) {
        return fail(u"Enter a bare domain name such as example.com, without scheme, port, path or wildcards."_s);
    }
    if (host.endsWith(u'.')) {
        host.chop(1);
    }
    if (host.isEmpty() || host.startsWith(u'.') || host.contains(u".."_s)) {
        return fail(u"“%1” is not a valid domain name."_s.arg(pattern.trimmed()));
    }
    const QString ace = withoutTrailingDot(aceLower(host));
    if (ace.isEmpty()) {
        return fail(u"“%1” is not a valid international domain name."_s.arg(pattern.trimmed()));
    }
    const QString canonical = endsInNumber(ace) ? canonicalIpv4(ace) : ace;
    if (canonical.isEmpty()) {
        return fail(u"“%1” ends in a number but is not a valid IPv4 address."_s.arg(pattern.trimmed()));
    }
    if (error) {
        error->clear();
    }
    return canonical;
}

namespace
{

struct GlobToken {
    enum Kind { Literal, AnyOne, AnySequence } kind;
    QChar ch;
};

// Returns false when the pattern ends in an unpaired escape.
bool tokenizeGlob(QStringView pattern, QList<GlobToken> *tokens)
{
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == u'\\') {
            if (++i == pattern.size()) {
                return false;
            }
            tokens->append(GlobToken{GlobToken::Literal, pattern.at(i)});
        } else if (c == u'*') {
            tokens->append(GlobToken{GlobToken::AnySequence, {}});
        } else if (c == u'?') {
            tokens->append(GlobToken{GlobToken::AnyOne, {}});
        } else {
            tokens->append(GlobToken{GlobToken::Literal, c});
        }
    }
    return true;
}

} // namespace

bool globMatches(QStringView pattern, QStringView text)
{
    QList<GlobToken> tokens;
    if (!tokenizeGlob(pattern, &tokens)) {
        return false;
    }

    // Greedy matching that backtracks only to the most recent '*': linear in practice.
    qsizetype p = 0;
    qsizetype t = 0;
    qsizetype starToken = -1;
    qsizetype starText = 0;
    while (t < text.size()) {
        if (p < tokens.size() && tokens[p].kind == GlobToken::AnySequence) {
            starToken = p++;
            starText = t;
        } else if (p < tokens.size()
                   && (tokens[p].kind == GlobToken::AnyOne || (tokens[p].kind == GlobToken::Literal && tokens[p].ch == text[t]))) {
            ++p;
            ++t;
        } else if (starToken >= 0) {
            p = starToken + 1;
            t = ++starText;
        } else {
            return false;
        }
    }
    while (p < tokens.size() && tokens[p].kind == GlobToken::AnySequence) {
        ++p;
    }
    return p == tokens.size();
}

RuleMatcher::RuleMatcher(const Rule &rule)
    : m_rule(rule)
{
    if (rule.pattern.isEmpty()) {
        m_error = u"The pattern is empty."_s;
        return;
    }

    switch (rule.type) {
    case RuleType::Domain:
    case RuleType::Subdomain:
        m_domain = canonicalDomainPattern(rule.pattern, &m_error);
        break;
    case RuleType::Glob: {
        QList<GlobToken> tokens;
        if (!tokenizeGlob(rule.pattern, &tokens)) {
            m_error = u"The pattern ends with an unpaired backslash."_s;
        }
        break;
    }
    case RuleType::Regex: {
        if (rule.pattern.contains(QChar(0))) {
            m_error = u"The regular expression contains a NUL character."_s;
            break;
        }
        auto regex = std::make_unique<regex_t>();
        const int status = regcomp(regex.get(), rule.pattern.toUtf8().constData(), REG_EXTENDED | REG_NOSUB);
        if (status != 0) {
            // A failed regcomp leaves nothing to regfree.
            char message[256];
            regerror(status, regex.get(), message, sizeof message);
            m_error = u"Invalid regular expression: %1."_s.arg(QString::fromLocal8Bit(message));
            break;
        }
        m_regex = std::shared_ptr<regex_t>(regex.release(), [](regex_t *r) {
            regfree(r);
            delete r;
        });
        break;
    }
    }
}

bool RuleMatcher::isValid() const
{
    return m_error.isEmpty();
}

QString RuleMatcher::error() const
{
    return m_error;
}

bool RuleMatcher::matches(const QString &url) const
{
    if (!isValid()) {
        return false;
    }

    switch (m_rule.type) {
    case RuleType::Domain:
        return !m_domain.isEmpty() && canonicalHostOfUrl(url) == m_domain;
    case RuleType::Subdomain: {
        const QString host = canonicalHostOfUrl(url);
        return !host.isEmpty() && (host == m_domain || host.endsWith(u'.' + m_domain));
    }
    case RuleType::Glob:
        return globMatches(m_rule.pattern, url);
    case RuleType::Regex:
        if (!m_regex || url.contains(QChar(0))) {
            return false;
        }
        return regexec(m_regex.get(), url.toUtf8().constData(), 0, nullptr, 0) == 0;
    }
    return false;
}

RuleEngine::RuleEngine(const QList<Rule> &rules, const QSet<QString> &availableBrowserIds)
    : m_rules(rules)
{
    m_matchers.reserve(rules.size());
    m_errors.reserve(rules.size());
    for (const Rule &rule : rules) {
        const RuleMatcher &matcher = m_matchers.emplace_back(rule);
        if (!matcher.isValid()) {
            m_errors.append(matcher.error());
        } else if (rule.browserId.isEmpty()) {
            m_errors.append(u"No target browser is selected."_s);
        } else if (!availableBrowserIds.contains(rule.browserId)) {
            m_errors.append(u"The target browser “%1” is not available."_s.arg(rule.browserId));
        } else {
            m_errors.append(QString());
        }
    }
}

QString RuleEngine::ruleError(qsizetype index) const
{
    if (index < 0 || index >= m_errors.size()) {
        return u"No such rule."_s;
    }
    return m_errors.at(index);
}

RouteResult RuleEngine::route(const QString &url) const
{
    for (qsizetype i = 0; i < m_rules.size(); ++i) {
        if (m_rules[i].enabled && m_errors[i].isEmpty() && m_matchers[i].matches(url)) {
            return {m_rules[i].browserId, int(i)};
        }
    }
    return {};
}

} // namespace kbs
