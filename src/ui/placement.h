#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>
#include <optional>

class QWidget;

namespace kbs
{

// Distance between the top of the card and the cursor when placed near it, so
// the pointer lands on the heading rather than on a browser row.
inline constexpr int CursorOffsetY = 32;

struct ScreenInfo {
    QString name; // QScreen::name(), the compositor's output name
    QRect geometry;
    QRect available; // without panels
};

enum class PlacementMode {
    NearCursor,
    Centered,
};

struct PlacementRequest {
    QSize preferredSize;
    std::optional<QPoint> cursor; // global position, only if the platform reports it reliably
    QList<ScreenInfo> screens;
    QString activeOutput; // as reported by the compositor; may be empty or stale
    QString primaryOutput;
    int margin = 8; // kept free between the window and the edges of the usable area
};

struct Placement {
    PlacementMode mode = PlacementMode::Centered;
    QString screenName;
    QRect geometry; // global, inside the screen's usable area

    bool isValid() const
    {
        return !screenName.isEmpty() && geometry.isValid();
    }
};

// Near the cursor and clamped to the usable area of the screen under it, or
// centred on the active (else primary, else first) screen when the cursor
// position is unknown or on no screen. The size shrinks to fit.
Placement planPlacement(const PlacementRequest &request);

// Largest window that fits into an available area while keeping the margin,
// unless the area is too small to keep it at all.
QSize maximumSize(const QRect &available, int margin);

// Where a window at `current` goes when its content asks for `preferred`: the
// size bounded by the available area (as maximumSize()), the horizontal centre
// and top kept, then moved fully inside the area.
QRect fitToArea(const QRect &current, QSize preferred, const QRect &available, int margin);

// The global cursor position if the platform reports it reliably. Native
// Wayland clients only learn it once the pointer enters one of their surfaces,
// so it is never trusted there. On X11 every position, including (0, 0), is real.
std::optional<QPoint> reliableCursorPosition(const QString &platformName, QPoint position);

// Output name of the active screen from KWin over D-Bus (org.kde.KWin /KWin
// activeOutputName); empty when KWin is not running or does not answer quickly.
QString kwinActiveOutputName();

// How placePopup() positioned a window, for diagnostics and tests.
struct PopupPlacement {
    Placement placement;
    bool layerShell = false; // a centred wlr-layer-shell surface on Wayland
};

// Sizes and positions a frameless popup before it is first shown. On Wayland
// the window becomes a layer-shell surface with no anchors (the compositor
// centres it on the chosen output); elsewhere it is moved to the planned
// geometry. Must be called before the window is mapped.
PopupPlacement placePopup(QWidget *window, QSize preferredSize);

// The same for content whose height depends on the width it gets: called with
// an invalid size for the natural size, then with the maximum size on the
// chosen screen if the natural size does not fit there.
using PreferredSizeFunction = std::function<QSize(const QSize &maximum)>;
PopupPlacement placePopup(QWidget *window, const PreferredSizeFunction &preferredSize);

} // namespace kbs
