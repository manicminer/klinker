#include "urlcleaner.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

// Deliberately conservative: only parameters that exist purely for click or
// campaign attribution. Generic names such as "ref", "id" or "source" are kept.
constexpr std::array<QStringView, 16> s_trackingNames = {
    u"gclid", u"dclid", u"gbraid", u"wbraid", u"gclsrc", u"fbclid", u"msclkid", u"mc_cid",
    u"mc_eid", u"yclid", u"twclid", u"ttclid", u"igshid", u"_hsenc", u"_hsmi", u"mkt_tok",
};

bool isHttpUrl(const QString &url)
{
    return url.startsWith("http:"_L1, Qt::CaseInsensitive) || url.startsWith("https:"_L1, Qt::CaseInsensitive);
}

QString decodedName(QStringView component)
{
    const qsizetype equals = component.indexOf(u'=');
    const QStringView name = equals < 0 ? component : component.first(equals);
    return QString::fromUtf8(QByteArray::fromPercentEncoding(name.toUtf8()));
}

} // namespace

bool isTrackingParameter(QStringView decodedName)
{
    if (decodedName.startsWith(u"utm_") && decodedName.size() > 4) {
        return true;
    }
    return std::ranges::find(s_trackingNames, decodedName) != s_trackingNames.end();
}

UrlCleanResult stripTrackingParameters(const QString &url)
{
    if (!isHttpUrl(url)) {
        return {url, 0};
    }

    const qsizetype hash = url.indexOf(u'#');
    const qsizetype question = url.indexOf(u'?');
    if (question < 0 || (hash >= 0 && question > hash)) {
        return {url, 0};
    }

    const qsizetype queryEnd = hash < 0 ? url.size() : hash;
    const QStringView query = QStringView(url).sliced(question + 1, queryEnd - question - 1);

    QStringList kept;
    int removed = 0;
    for (const QStringView component : query.tokenize(u'&')) {
        if (!component.isEmpty() && isTrackingParameter(decodedName(component))) {
            ++removed;
        } else {
            kept.append(component.toString());
        }
    }

    if (removed == 0) {
        return {url, 0};
    }

    QString result = url.first(question);
    // Empty components are kept too: "?&utm_source=x&" becomes "?&".
    if (!kept.isEmpty()) {
        result += u'?' + kept.join(u'&');
    }
    if (hash >= 0) {
        result += QStringView(url).sliced(hash);
    }
    return {result, removed};
}

} // namespace kbs
