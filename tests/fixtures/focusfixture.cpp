// Another application for focus tests in a private, nested KWin (see
// tests/wayland-smoke.sh): shows a plain window, prints "focused" once it has
// the keyboard focus, and quits after the given number of milliseconds.
//
//   kbs_focus_fixture [milliseconds]

#include <QApplication>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const int lifetime = argc > 1 ? QByteArray(argv[1]).toInt() : 10000;

    QWidget window;
    window.setWindowTitle(QStringLiteral("Focus fixture"));
    window.resize(320, 200);
    QObject::connect(&app, &QGuiApplication::focusWindowChanged, &app, [](QWindow *focus) {
        if (focus) {
            std::puts("focused");
            std::fflush(stdout);
        }
    });
    window.show();
    window.activateWindow();
    QTimer::singleShot(lifetime, &app, &QCoreApplication::quit);
    return app.exec();
}
