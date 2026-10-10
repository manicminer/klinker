#include "core/rules.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace klinker;

namespace
{
Rule makeRule(RuleType type, const QString &pattern, const QString &browser = u"b.desktop"_s, bool enabled = true)
{
    Rule rule;
    rule.type = type;
    rule.pattern = pattern;
    rule.browserId = browser;
    rule.enabled = enabled;
    return rule;
}
} // namespace

class TestRules : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void ruleTypeNames()
    {
        for (const auto type : {RuleType::Domain, RuleType::Subdomain, RuleType::Glob, RuleType::Regex}) {
            QCOMPARE(ruleTypeFromString(ruleTypeToString(type)), type);
        }
        QCOMPARE(ruleTypeToString(RuleType::Subdomain), u"subdomain"_s);
        QCOMPARE(ruleTypeFromString(u"regex"_s), RuleType::Regex);
        QCOMPARE(ruleTypeFromString(u"wildcard"_s), std::nullopt);
    }

    void canonicalHost_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("host");
        QTest::newRow("simple") << u"https://example.com/path"_s << u"example.com"_s;
        QTest::newRow("case") << u"HTTPS://WWW.Example.COM"_s << u"www.example.com"_s;
        QTest::newRow("credentials and port") << u"https://user:p%40ss@example.com:8443/x?y#z"_s << u"example.com"_s;
        QTest::newRow("at in password") << u"https://user:p@ss@example.com/"_s << u"example.com"_s;
        QTest::newRow("trailing dot") << u"https://example.com./"_s << u"example.com"_s;
        QTest::newRow("idn unicode") << u"https://Bücher.de/"_s << u"xn--bcher-kva.de"_s;
        QTest::newRow("idn ace") << u"https://xn--bcher-kva.de/"_s << u"xn--bcher-kva.de"_s;
        QTest::newRow("percent encoded host") << u"https://%65xample.com/"_s << u"example.com"_s;
        QTest::newRow("backslash is path separator") << u"http://evil.example\\@good.example/"_s << u"evil.example"_s;
        QTest::newRow("query before at") << u"https://evil.example?@good.example/"_s << u"evil.example"_s;
        QTest::newRow("fragment before at") << u"https://evil.example#@good.example/"_s << u"evil.example"_s;
        QTest::newRow("ipv6") << u"http://[::1]:8080/"_s << u"[::1]"_s;
        QTest::newRow("ipv4") << u"http://127.0.0.1:8080/"_s << u"127.0.0.1"_s;
        // WHATWG IPv4 parsing: fewer parts, hex, octal and single numbers.
        QTest::newRow("ipv4 two parts") << u"http://127.1/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 hex number") << u"http://0x7f000001/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 decimal number") << u"http://2130706433/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 octal part") << u"http://0177.0.0.1/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 mixed parts") << u"https://0X7F.0.1/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 three parts") << u"http://10.1.258/"_s << u"10.1.1.2"_s;
        QTest::newRow("ipv4 trailing dot") << u"http://127.0.0.1./"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 empty hex") << u"http://0x/"_s << u"0.0.0.0"_s;
        QTest::newRow("ipv4 percent encoded") << u"http://%31%32%37.1/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 with credentials and port") << u"http://u:p@0x7f.1:81/"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 maximum") << u"http://4294967295/"_s << u"255.255.255.255"_s;
        QTest::newRow("ipv4 too large") << u"http://4294967296/"_s << QString();
        QTest::newRow("ipv4 part too large") << u"http://1.2.3.256/"_s << QString();
        QTest::newRow("ipv4 leading part too large") << u"http://256.1/"_s << QString();
        QTest::newRow("ipv4 too many parts") << u"http://1.2.3.4.5/"_s << QString();
        QTest::newRow("ipv4 bad octal") << u"http://09.1.1.1/"_s << QString();
        QTest::newRow("ipv4 huge hex") << u"http://0xffffffffffffffffffff/"_s << QString();
        QTest::newRow("ends in number is not a domain") << u"http://example.123/"_s << QString();
        QTest::newRow("ends in hex is not a domain") << u"http://example.0x1/"_s << QString();
        QTest::newRow("last label not numeric") << u"http://1.2.3.4a/"_s << u"1.2.3.4a"_s;
        QTest::newRow("numeric middle label is a domain") << u"http://1.example/"_s << u"1.example"_s;
        QTest::newRow("non-special host is opaque") << u"foo://127.1/"_s << u"127.1"_s;
        QTest::newRow("no host") << u"mailto:user@example.com"_s << QString();
        // Browsers skip any number of slashes after a special scheme.
        QTest::newRow("extra slashes") << u"https:///example.com/path"_s << u"example.com"_s;
        QTest::newRow("no slashes") << u"https:example.com/path"_s << u"example.com"_s;
        QTest::newRow("tab and newline removed") << u" https://exa\tmple.c\nom/ "_s << u"example.com"_s;
        QTest::newRow("empty host") << u"https://"_s << QString();
        QTest::newRow("empty host with port") << u"https://:443/"_s << QString();
        QTest::newRow("non-special needs slashes") << u"foo:example.com"_s << QString();
        QTest::newRow("non-special with authority") << u"ftp://files.example.com/x"_s << u"files.example.com"_s;
        QTest::newRow("garbage") << u"not a url"_s << QString();
        QTest::newRow("space in host") << u"https://exa mple.com/"_s << QString();
    }

    void canonicalHost()
    {
        QFETCH(QString, url);
        QFETCH(QString, host);
        QCOMPARE(canonicalHostOfUrl(url), host);
    }

    void domainPatternValidation_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("canonical");
        QTest::newRow("plain") << u"Example.COM"_s << u"example.com"_s;
        QTest::newRow("whitespace trimmed") << u"  example.com "_s << u"example.com"_s;
        QTest::newRow("trailing dot") << u"example.com."_s << u"example.com"_s;
        QTest::newRow("idn") << u"BÜCHER.de"_s << u"xn--bcher-kva.de"_s;
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("scheme") << u"https://example.com"_s << QString();
        QTest::newRow("path") << u"example.com/x"_s << QString();
        QTest::newRow("port") << u"example.com:80"_s << QString();
        QTest::newRow("wildcard") << u"*.example.com"_s << QString();
        QTest::newRow("leading dot") << u".example.com"_s << QString();
        QTest::newRow("double dot") << u"example..com"_s << QString();
        QTest::newRow("credentials") << u"u@example.com"_s << QString();
        QTest::newRow("space") << u"exa mple.com"_s << QString();
        QTest::newRow("ipv4") << u"127.0.0.1"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 short") << u"127.1"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 hex") << u"0x7F000001"_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 trailing dot") << u"127.0.0.1."_s << u"127.0.0.1"_s;
        QTest::newRow("ipv4 out of range") << u"1.2.3.256"_s << QString();
        QTest::newRow("ends in number") << u"example.123"_s << QString();
    }

    void domainPatternValidation()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, canonical);
        QString error;
        QCOMPARE(canonicalDomainPattern(pattern, &error), canonical);
        QCOMPARE(error.isEmpty(), !canonical.isEmpty());
    }

    void domainMatching_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("match");

        const int exact = int(RuleType::Domain);
        const int sub = int(RuleType::Subdomain);
        QTest::newRow("exact same") << exact << u"example.com"_s << u"https://example.com/a"_s << true;
        QTest::newRow("exact case insensitive") << exact << u"EXAMPLE.com"_s << u"https://Example.Com/a"_s << true;
        QTest::newRow("exact ignores credentials port path") << exact << u"example.com"_s << u"http://u:p@example.com:81/x?q#f"_s << true;
        QTest::newRow("exact not subdomain") << exact << u"example.com"_s << u"https://www.example.com/"_s << false;
        QTest::newRow("exact not suffix") << exact << u"example.com"_s << u"https://notexample.com/"_s << false;
        QTest::newRow("exact host in path") << exact << u"example.com"_s << u"https://other.org/example.com"_s << false;
        QTest::newRow("exact host in credentials") << exact << u"example.com"_s << u"https://example.com@other.org/"_s << false;
        QTest::newRow("exact idn") << exact << u"bücher.de"_s << u"https://xn--bcher-kva.de/"_s << true;
        QTest::newRow("exact idn reverse") << exact << u"xn--bcher-kva.de"_s << u"https://BÜCHER.de/"_s << true;
        QTest::newRow("exact trailing dot") << exact << u"example.com"_s << u"https://example.com./"_s << true;
        QTest::newRow("sub apex included") << sub << u"example.com"_s << u"https://example.com/"_s << true;
        QTest::newRow("sub one level") << sub << u"example.com"_s << u"https://www.example.com/"_s << true;
        QTest::newRow("sub deep") << sub << u"example.com"_s << u"https://a.b.c.example.com/"_s << true;
        QTest::newRow("sub dot boundary") << sub << u"example.com"_s << u"https://notexample.com/"_s << false;
        QTest::newRow("sub not prefix") << sub << u"example.com"_s << u"https://example.com.evil.net/"_s << false;
        QTest::newRow("sub idn") << sub << u"bücher.de"_s << u"https://shop.bücher.de/"_s << true;
        QTest::newRow("sub backslash trick") << sub << u"good.example"_s << u"http://evil.example\\@good.example/"_s << false;
        QTest::newRow("no host") << sub << u"example.com"_s << u"mailto:me@example.com"_s << false;
        QTest::newRow("exact ipv4 two parts") << exact << u"127.0.0.1"_s << u"http://127.1/"_s << true;
        QTest::newRow("exact ipv4 hex") << exact << u"127.0.0.1"_s << u"http://0x7f000001/"_s << true;
        QTest::newRow("exact ipv4 decimal") << exact << u"127.0.0.1"_s << u"http://2130706433/"_s << true;
        QTest::newRow("exact ipv4 short pattern") << exact << u"127.1"_s << u"http://127.0.0.1:8080/"_s << true;
        QTest::newRow("exact ipv4 other address") << exact << u"127.0.0.1"_s << u"http://127.0.0.2/"_s << false;
        QTest::newRow("sub ipv4 alias") << sub << u"127.0.0.1"_s << u"http://0177.1/"_s << true;
        QTest::newRow("sub ipv4 no partial") << sub << u"0.0.1"_s << u"http://127.0.0.1/"_s << false;
    }

    void domainMatching()
    {
        QFETCH(int, type);
        QFETCH(QString, pattern);
        QFETCH(QString, url);
        QFETCH(bool, match);
        const RuleMatcher matcher(makeRule(RuleType(type), pattern));
        QVERIFY2(matcher.isValid(), qPrintable(matcher.error()));
        QCOMPARE(matcher.matches(url), match);
    }

    void globMatching_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("match");
        QTest::newRow("star crosses slash") << u"https://github.com/*"_s << u"https://github.com/org/repo/issues"_s << true;
        QTest::newRow("whole url anchored") << u"github.com/*"_s << u"https://github.com/org"_s << false;
        QTest::newRow("leading star") << u"*github.com/*"_s << u"https://github.com/org"_s << true;
        QTest::newRow("middle star") << u"https://*.example.com/*"_s << u"https://a.b.example.com/x/y"_s << true;
        QTest::newRow("star empty") << u"https://example.com/*"_s << u"https://example.com/"_s << true;
        QTest::newRow("question one char") << u"https://example.com/?"_s << u"https://example.com/a"_s << true;
        QTest::newRow("question not zero") << u"https://example.com/?"_s << u"https://example.com/"_s << false;
        QTest::newRow("case sensitive") << u"https://example.com/A*"_s << u"https://example.com/a"_s << false;
        QTest::newRow("escaped star literal") << u"https://example.com/\\*"_s << u"https://example.com/*"_s << true;
        QTest::newRow("escaped star not wildcard") << u"https://example.com/\\*"_s << u"https://example.com/x"_s << false;
        QTest::newRow("brackets literal") << u"https://example.com/[a]"_s << u"https://example.com/[a]"_s << true;
        QTest::newRow("multiple stars backtrack") << u"*a*b*c"_s << u"xxaxxbxxbxxc"_s << true;
        QTest::newRow("multiple stars fail") << u"*a*b*c"_s << u"xxaxxcxxb"_s << false;
        QTest::newRow("unicode") << u"https://bücher.de/*"_s << u"https://bücher.de/straße"_s << true;
    }

    void globMatching()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, url);
        QFETCH(bool, match);
        QCOMPARE(globMatches(pattern, url), match);
        const RuleMatcher matcher(makeRule(RuleType::Glob, pattern));
        QVERIFY(matcher.isValid());
        QCOMPARE(matcher.matches(url), match);
    }

    void regexMatching_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("match");
        QTest::newRow("unanchored search") << u"github\\.com/myorg"_s << u"https://github.com/myorg/repo"_s << true;
        QTest::newRow("anchored") << u"^https://(www\\.)?example\\.(com|org)/"_s << u"https://www.example.org/x"_s << true;
        QTest::newRow("anchored no match") << u"^https://(www\\.)?example\\.(com|org)/"_s << u"http://example.org/x"_s << false;
        QTest::newRow("interval") << u"/[0-9]{4}/"_s << u"https://example.com/2024/x"_s << true;
        QTest::newRow("interval short") << u"/[0-9]{4}/"_s << u"https://example.com/202/x"_s << false;
        QTest::newRow("bracket class") << u"[[:upper:]]+"_s << u"https://example.com/ABC"_s << true;
        QTest::newRow("plus and question") << u"^https?://a+b?$"_s << u"http://aaa"_s << true;
        QTest::newRow("end anchor") << u"\\.pdf$"_s << u"https://example.com/doc.pdf?x"_s << false;
        QTest::newRow("case sensitive") << u"EXAMPLE"_s << u"https://example.com/"_s << false;
        QTest::newRow("unicode literal") << u"bücher"_s << u"https://bücher.de/"_s << true;
    }

    void regexMatching()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, url);
        QFETCH(bool, match);
        const RuleMatcher matcher(makeRule(RuleType::Regex, pattern));
        QVERIFY2(matcher.isValid(), qPrintable(matcher.error()));
        QCOMPARE(matcher.matches(url), match);
    }

    void invalidPatterns_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("pattern");
        QTest::newRow("regex unbalanced paren") << int(RuleType::Regex) << u"(abc"_s;
        QTest::newRow("regex unbalanced bracket") << int(RuleType::Regex) << u"[abc"_s;
        QTest::newRow("regex bad interval") << int(RuleType::Regex) << u"a{2,1}"_s;
        QTest::newRow("regex empty") << int(RuleType::Regex) << QString();
        QTest::newRow("regex nul") << int(RuleType::Regex) << QString(u"a\0b"_s);
        QTest::newRow("glob empty") << int(RuleType::Glob) << QString();
        QTest::newRow("glob trailing backslash") << int(RuleType::Glob) << u"https://x/\\"_s;
        QTest::newRow("domain empty") << int(RuleType::Domain) << QString();
        QTest::newRow("domain url") << int(RuleType::Domain) << u"https://example.com/"_s;
        QTest::newRow("subdomain wildcard") << int(RuleType::Subdomain) << u"*.example.com"_s;
    }

    void invalidPatterns()
    {
        QFETCH(int, type);
        QFETCH(QString, pattern);
        const RuleMatcher matcher(makeRule(RuleType(type), pattern));
        QVERIFY(!matcher.isValid());
        QVERIFY(!matcher.error().isEmpty());
        QVERIFY(!matcher.matches(u"https://example.com/"_s));
        QVERIFY(!matcher.matches(QString()));
    }

    void firstMatchingRuleWins()
    {
        const QList<Rule> rules = {
            makeRule(RuleType::Domain, u"example.com"_s, u"first.desktop"_s),
            makeRule(RuleType::Subdomain, u"example.com"_s, u"second.desktop"_s),
            makeRule(RuleType::Glob, u"*"_s, u"third.desktop"_s),
        };
        const RuleEngine engine(rules, {u"first.desktop"_s, u"second.desktop"_s, u"third.desktop"_s});

        auto result = engine.route(u"https://example.com/"_s);
        QCOMPARE(result.browserId, u"first.desktop"_s);
        QCOMPARE(result.ruleIndex, 0);

        result = engine.route(u"https://www.example.com/"_s);
        QCOMPARE(result.browserId, u"second.desktop"_s);
        QCOMPARE(result.ruleIndex, 1);

        result = engine.route(u"https://other.org/"_s);
        QCOMPARE(result.browserId, u"third.desktop"_s);
        QCOMPARE(result.ruleIndex, 2);
    }

    void numericHostAliasesRouteLikeTheAddress()
    {
        const QList<Rule> rules = {
            makeRule(RuleType::Domain, u"127.0.0.1"_s, u"local.desktop"_s),
            makeRule(RuleType::Glob, u"*"_s, u"other.desktop"_s),
        };
        const RuleEngine engine(rules, {u"local.desktop"_s, u"other.desktop"_s});
        for (const QString &url : {u"http://127.0.0.1/"_s, u"http://127.1/"_s, u"http://0x7f000001/"_s, u"http://2130706433/x"_s}) {
            QCOMPARE(engine.route(url).browserId, u"local.desktop"_s);
        }
        QCOMPARE(engine.route(u"http://127.0.0.2/"_s).browserId, u"other.desktop"_s);
    }

    void noMatchMeansChooser()
    {
        const RuleEngine engine({makeRule(RuleType::Domain, u"example.com"_s)}, {u"b.desktop"_s});
        const auto result = engine.route(u"https://other.org/"_s);
        QVERIFY(!result.matched());
        QVERIFY(result.browserId.isEmpty());
        QCOMPARE(result.ruleIndex, -1);
    }

    void unusableRulesAreSkippedAndReported()
    {
        const QList<Rule> rules = {
            makeRule(RuleType::Domain, u"example.com"_s, u"one.desktop"_s, false), // disabled
            makeRule(RuleType::Regex, u"(broken"_s, u"one.desktop"_s), // invalid pattern
            makeRule(RuleType::Domain, u"example.com"_s, u"removed.desktop"_s), // missing target
            makeRule(RuleType::Domain, u"example.com"_s, QString()), // no target
            makeRule(RuleType::Domain, u"example.com"_s, u"hidden.desktop"_s), // hidden browsers are valid targets
        };
        const RuleEngine engine(rules, {u"one.desktop"_s, u"hidden.desktop"_s});

        QVERIFY(engine.ruleError(0).isEmpty());
        QVERIFY(!engine.ruleError(1).isEmpty());
        QVERIFY(engine.ruleError(2).contains(u"removed.desktop"_s));
        QVERIFY(!engine.ruleError(3).isEmpty());
        QVERIFY(engine.ruleError(4).isEmpty());
        QVERIFY(!engine.ruleError(99).isEmpty());

        const auto result = engine.route(u"https://example.com/"_s);
        QCOMPARE(result.browserId, u"hidden.desktop"_s);
        QCOMPARE(result.ruleIndex, 4);
    }

    void disabledRuleWithBrokenPatternStillReported()
    {
        const RuleEngine engine({makeRule(RuleType::Regex, u"[x"_s, u"b.desktop"_s, false)}, {u"b.desktop"_s});
        QVERIFY(!engine.ruleError(0).isEmpty());
        QVERIFY(!engine.route(u"https://x/"_s).matched());
    }

    void compiledMatcherCopiesShareRegex()
    {
        const RuleMatcher original(makeRule(RuleType::Regex, u"^https://a"_s));
        const RuleMatcher copy = original;
        QVERIFY(copy.matches(u"https://a/"_s));
        QVERIFY(original.matches(u"https://a/"_s));
    }
};

QTEST_GUILESS_MAIN(TestRules)
#include "test_rules.moc"
