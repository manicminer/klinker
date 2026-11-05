#include "documentportal.h"

#include "core/hostsystem.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QMap>
#include <QStandardPaths>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace klinker
{

QString hostFilePath(const QString &path)
{
    if (!HostSystem::current().sandboxed) {
        return path;
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + u"/doc/"_s;
    const QStringList parts = path.startsWith(documents) ? path.sliced(documents.size()).split(u'/') : QStringList();
    if (parts.size() != 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty()) {
        return hostPath(path);
    }
    QDBusMessage call = QDBusMessage::createMethodCall(u"org.freedesktop.portal.Documents"_s, u"/org/freedesktop/portal/documents"_s,
                                                       u"org.freedesktop.portal.Documents"_s, u"GetHostPaths"_s);
    call << QStringList{parts.at(0)};
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return path;
    }
    QMap<QString, QByteArray> hostPaths;
    reply.arguments().constFirst().value<QDBusArgument>() >> hostPaths;
    QByteArray host = hostPaths.value(parts.at(0));
    if (host.endsWith('\0')) {
        host.chop(1);
    }
    return host.startsWith('/') ? QFile::decodeName(host) : path;
}

QString hostFileUrl(const QString &url)
{
    if (!HostSystem::current().sandboxed || !url.startsWith("file:"_L1, Qt::CaseInsensitive)) {
        return url;
    }
    QUrl parsed(url, QUrl::TolerantMode);
    const QString local = parsed.toLocalFile();
    const QString host = hostFilePath(local);
    if (local.isEmpty() || host == local) {
        return url;
    }
    parsed.setPath(host);
    return parsed.toString(QUrl::FullyEncoded);
}

} // namespace klinker
