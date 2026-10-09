// Stand-in browser for launch tests: `kbs_capture_fixture --out DIR ARGS...`
// writes ARGS as a JSON array to a new file in DIR and exits.

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>

#include <cstdio>

#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3 || QByteArray(argv[1]) != "--out") {
        std::fprintf(stderr, "usage: %s --out DIR [ARGS...]\n", argv[0]);
        return 2;
    }
    QJsonArray arguments;
    for (int i = 3; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    const QString dir = QString::fromLocal8Bit(argv[2]);
    const QString name = QStringLiteral("%1/capture-%2-%3")
                             .arg(dir)
                             .arg(QDateTime::currentMSecsSinceEpoch())
                             .arg(qint64(::getpid()));
    // Write then rename so readers never see a partial file.
    QFile file(name + QStringLiteral(".tmp"));
    if (!file.open(QIODevice::WriteOnly)) {
        return 1;
    }
    file.write(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    file.close();
    return file.rename(name + QStringLiteral(".json")) ? 0 : 1;
}
