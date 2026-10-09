#pragma once

#include <QString>
#include <QStringView>

namespace kbs
{

struct UrlCleanResult {
    QString url;
    int removedCount = 0;
};

// True when a (percent-decoded) query parameter name is a known tracking parameter.
bool isTrackingParameter(QStringView decodedName);

// Removes known tracking parameters from the query of an http(s) URL. Every other
// character of the input, including the order, encoding and duplicates of the
// remaining parameters, empty components and the fragment, is preserved verbatim.
// The '?' is dropped only when the query held nothing but tracking parameters.
UrlCleanResult stripTrackingParameters(const QString &url);

} // namespace kbs
