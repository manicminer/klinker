#include "placement.h"

#include <QCursor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#ifdef KBS_HAVE_LAYERSHELLQT
#include <LayerShellQt/Window>
#endif

#include <algorithm>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

constexpr int KWinTimeoutMs = 250;

// The usable area minus the margin, or the whole usable area if it is too
// small to keep the margin.
QRect innerArea(const QRect &available, int margin)
{
    if (available.width() <= 2 * margin || available.height() <= 2 * margin) {
        return available;
    }
    return available.adjusted(margin, margin, -margin, -margin);
}

const ScreenInfo *findScreen(const QList<ScreenInfo> &screens, const QString &name)
{
    if (name.isEmpty()) {
        return nullptr;
    }
    const auto it = std::ranges::find(screens, name, &ScreenInfo::name);
    return it == screens.cend() ? nullptr : &*it;
}

bool isWayland(const QString &platformName)
{
    return platformName.startsWith(u"wayland"_s);
}

} // namespace

QSize maximumSize(const QRect &available, int margin)
{
    return innerArea(available, margin).size();
}

Placement planPlacement(const PlacementRequest &request)
{
    if (request.screens.isEmpty()) {
        return {};
    }

    const ScreenInfo *screen = nullptr;
    if (request.cursor) {
        const auto it = std::ranges::find_if(request.screens, [&request](const ScreenInfo &s) {
            return s.geometry.contains(*request.cursor);
        });
        if (it != request.screens.cend()) {
            screen = &*it;
        }
    }
    const bool nearCursor = screen != nullptr;
    if (!screen) {
        screen = findScreen(request.screens, request.activeOutput);
    }
    if (!screen) {
        screen = findScreen(request.screens, request.primaryOutput);
    }
    if (!screen) {
        screen = &request.screens.first();
    }

    const QRect area = innerArea(screen->available, request.margin);
    const QSize size = request.preferredSize.boundedTo(area.size());

    QPoint topLeft;
    if (nearCursor) {
        topLeft = *request.cursor - QPoint(size.width() / 2, CursorOffsetY);
    } else {
        topLeft = area.topLeft() + QPoint((area.width() - size.width()) / 2, (area.height() - size.height()) / 2);
    }
    topLeft.setX(std::clamp(topLeft.x(), area.left(), area.left() + area.width() - size.width()));
    topLeft.setY(std::clamp(topLeft.y(), area.top(), area.top() + area.height() - size.height()));

    return {nearCursor ? PlacementMode::NearCursor : PlacementMode::Centered, screen->name, QRect(topLeft, size)};
}

QRect fitToArea(const QRect &current, QSize preferred, const QRect &available, int margin)
{
    const QRect area = innerArea(available, margin);
    const QSize size = preferred.boundedTo(area.size());
    QPoint topLeft(current.x() + (current.width() - size.width()) / 2, current.y());
    topLeft.setX(std::clamp(topLeft.x(), area.left(), area.left() + area.width() - size.width()));
    topLeft.setY(std::clamp(topLeft.y(), area.top(), area.top() + area.height() - size.height()));
    return {topLeft, size};
}

std::optional<QPoint> reliableCursorPosition(const QString &platformName, QPoint position)
{
    if (platformName == u"xcb"_s) {
        return position;
    }
    return std::nullopt;
}

QString kwinActiveOutputName()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return {};
    }
    const QDBusMessage call = QDBusMessage::createMethodCall(u"org.kde.KWin"_s, u"/KWin"_s, u"org.kde.KWin"_s, u"activeOutputName"_s);
    const QDBusMessage reply = bus.call(call, QDBus::Block, KWinTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    return reply.arguments().constFirst().toString();
}

PopupPlacement placePopup(QWidget *window, QSize preferredSize)
{
    return placePopup(window, [preferredSize](const QSize &) {
        return preferredSize;
    });
}

PopupPlacement placePopup(QWidget *window, const PreferredSizeFunction &preferredSize)
{
    const QString platform = QGuiApplication::platformName();
    PlacementRequest request;
    request.preferredSize = preferredSize(QSize());
    request.cursor = reliableCursorPosition(platform, QCursor::pos());
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        request.screens.append({screen->name(), screen->geometry(), screen->availableGeometry()});
    }
    if (const QScreen *primary = QGuiApplication::primaryScreen()) {
        request.primaryOutput = primary->name();
    }
    if (!request.cursor && isWayland(platform)) {
        request.activeOutput = kwinActiveOutputName();
    }

    PopupPlacement result{planPlacement(request), false};
    if (!result.placement.isValid()) {
        window->resize(request.preferredSize);
        return result;
    }
    if (result.placement.geometry.size() != request.preferredSize) {
        // Too large for the chosen screen: plan again with the size for what
        // fits there. The screen does not depend on the size.
        const auto chosen = std::ranges::find(request.screens, result.placement.screenName, &ScreenInfo::name);
        request.preferredSize = preferredSize(maximumSize(chosen->available, request.margin));
        result.placement = planPlacement(request);
    }

    QScreen *screen = nullptr;
    for (QScreen *candidate : screens) {
        if (candidate->name() == result.placement.screenName) {
            screen = candidate;
        }
    }
    window->resize(result.placement.geometry.size());

#ifdef KBS_HAVE_LAYERSHELLQT
    if (isWayland(platform)) {
        // Creates the platform window without mapping it, so the layer-shell
        // role can still be assigned.
        window->winId();
        if (LayerShellQt::Window *layer = LayerShellQt::Window::get(window->windowHandle())) {
            layer->setScope(u"klinker"_s);
            layer->setLayer(LayerShellQt::Window::LayerTop);
            layer->setAnchors(LayerShellQt::Window::AnchorNone);
            layer->setExclusiveZone(0);
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
            if (screen) {
                layer->setScreen(screen);
            }
            result.layerShell = true;
            return result;
        }
    }
#endif

    if (screen) {
        window->setScreen(screen);
    }
    window->move(result.placement.geometry.topLeft());
    return result;
}

} // namespace kbs
