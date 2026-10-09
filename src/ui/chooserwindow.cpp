#include "chooserwindow.h"

#include "placement.h"
#include "theme.h"

#include <KBusyIndicatorWidget>
#include <KSeparator>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace kbs
{

namespace
{

constexpr int IconSize = 32;
constexpr int RowPaddingX = 10;
constexpr int RowPaddingY = 8;
constexpr int RowGap = 12;
constexpr int RowRadius = 7;
constexpr int CardRadius = 12;

QFont detailFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() > 0 ? base.pointSizeF() * 0.88 : 9);
    return font;
}

QFont nameFont(const QFont &base)
{
    QFont font = base;
    font.setWeight(QFont::DemiBold);
    return font;
}

// Browser row: 32 px icon, bold name over a subdued detail line, and the
// number key on the right. Hover and selection use the palette's accent.
class BrowserDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        const int text = QFontMetrics(nameFont(option.font)).height() + 2 + QFontMetrics(detailFont(option.font)).height();
        return {IconSize + 2 * RowPaddingX, std::max(IconSize, text) + 2 * RowPaddingY};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPalette &palette = option.palette;
        const bool enabled = option.state & QStyle::State_Enabled;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool focused = option.state & QStyle::State_HasFocus || (selected && option.widget && option.widget->hasFocus());
        if (!enabled) {
            painter->setOpacity(0.45);
        }

        const QRectF row = QRectF(option.rect).adjusted(1.5, 1.5, -1.5, -1.5);
        QColor accent = palette.color(QPalette::Active, QPalette::Highlight);
        if (enabled && (selected || hovered)) {
            QColor fill = accent;
            fill.setAlphaF(selected ? 0.22f : 0.12f);
            QColor border = accent;
            border.setAlphaF(selected ? (focused ? 0.95f : 0.55f) : 0.35f);
            painter->setPen(QPen(border, 1));
            painter->setBrush(fill);
            painter->drawRoundedRect(row, RowRadius, RowRadius);
        }

        const QRect content = option.rect.adjusted(RowPaddingX, RowPaddingY, -RowPaddingX, -RowPaddingY);
        const QRect iconRect(content.left(), content.center().y() - IconSize / 2 + 1, IconSize, IconSize);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        icon.paint(painter, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);

        // Number key chip.
        const int shortcut = index.data(BrowserListModel::ShortcutRole).toInt();
        int textRight = content.right();
        if (shortcut > 0) {
            const QFont chipFont = detailFont(option.font);
            const QFontMetrics chipMetrics(chipFont);
            const int side = std::max(chipMetrics.height() + 4, chipMetrics.horizontalAdvance(u"9"_s) + 12);
            const QRectF chip(content.right() - side + 1, content.center().y() - side / 2.0 + 1, side, side);
            const QColor text = palette.color(QPalette::WindowText);
            QColor chipBorder = selected ? accent : mixColors(text, palette.color(QPalette::Window), 0.7);
            painter->setPen(QPen(chipBorder, 1));
            painter->setBrush(mixColors(palette.color(QPalette::Window), text, 0.04));
            painter->drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
            painter->setFont(chipFont);
            painter->setPen(selected ? text : subduedTextColor(palette));
            painter->drawText(chip, Qt::AlignCenter, QString::number(shortcut));
            textRight = int(chip.left()) - RowGap;
        }

        const QFont nameF = nameFont(option.font);
        const QFont detailF = detailFont(option.font);
        const QFontMetrics nameMetrics(nameF);
        const QFontMetrics detailMetrics(detailF);
        const int textLeft = iconRect.right() + RowGap;
        const int textWidth = std::max(0, textRight - textLeft);
        const int textHeight = nameMetrics.height() + 2 + detailMetrics.height();
        const int top = content.center().y() - textHeight / 2 + 1;

        painter->setFont(nameF);
        painter->setPen(palette.color(QPalette::WindowText));
        painter->drawText(QRect(textLeft, top, textWidth, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));
        painter->setFont(detailF);
        painter->setPen(subduedTextColor(palette));
        painter->drawText(QRect(textLeft, top + nameMetrics.height() + 2, textWidth, detailMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          detailMetrics.elidedText(index.data(BrowserListModel::DetailRole).toString(), Qt::ElideMiddle, textWidth));
        painter->restore();
    }
};

// Asks for exactly as many rows as the chooser wants to show at once.
class BrowserListView : public QListView
{
public:
    using QListView::QListView;

    QSize sizeHint() const override
    {
        const int rows = std::min(model() ? model()->rowCount() : 0, ChooserWindow::MaxShownRows);
        const int rowHeight = std::max(1, sizeHintForRow(0) > 0 ? sizeHintForRow(0) : 52);
        return {ChooserWindow::CardWidth, std::max(1, rows) * rowHeight + 2 * frameWidth()};
    }

    QSize minimumSizeHint() const override
    {
        return {100, std::max(sizeHintForRow(0), 40) + 2 * frameWidth()};
    }
};

// Holds everything above the browser list. It asks for its full content
// height, but can shrink to its first row and scroll, so that long details or
// errors never push the list or the footer out of a small window.
class ContentArea : public QScrollArea
{
public:
    ContentArea(QWidget *content, QWidget *parent)
        : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWidgetResizable(true);
        setWidget(content);
        // setWidget() turns this on; the card paints the background.
        content->setAutoFillBackground(false);
        viewport()->setAutoFillBackground(false);
        content->installEventFilter(this);
        // A tab stop only when there is something to scroll to.
        setFocusPolicy(Qt::NoFocus);
        connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int max) {
            setFocusPolicy(max > 0 ? Qt::TabFocus : Qt::NoFocus);
        });
    }

    QSize sizeHint() const override
    {
        return {ChooserWindow::CardWidth, contentHeight()};
    }

    QSize minimumSizeHint() const override
    {
        const QLayout *column = widget()->layout();
        const int firstRow = column->count() > 0 ? column->itemAt(0)->sizeHint().height() : 0;
        return {100, std::min(contentHeight(), firstRow)};
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == widget() && event->type() == QEvent::LayoutRequest) {
            updateGeometry();
        }
        return QScrollArea::eventFilter(watched, event);
    }

private:
    int contentHeight() const
    {
        // Before the window is laid out, assume the card's usual width.
        const QMargins margins = parentWidget()->layout()->contentsMargins();
        const int width = isVisible() ? this->width() - 2 * frameWidth() : ChooserWindow::CardWidth - margins.left() - margins.right();
        const QWidget *content = widget();
        return content->hasHeightForWidth() ? content->heightForWidth(width) : content->sizeHint().height();
    }
};

QToolButton *flatToolButton(const QString &iconName, const QString &label, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(iconName));
    button->setAutoRaise(true);
    button->setToolTip(label);
    button->setAccessibleName(label);
    return button;
}

} // namespace

// A persistent error or warning: the theme's icon for the severity and plain,
// wrapped text on a slightly raised surface in the palette's colours. Its
// height always follows its width, so long messages are never cut off.
class InlineMessage : public QFrame
{
public:
    enum class Severity {
        Warning,
        Error,
    };

    explicit InlineMessage(QWidget *parent)
        : QFrame(parent)
        , m_icon(new QLabel(this))
        , m_text(new QLabel(this))
    {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(10, 8, 10, 8);
        row->setSpacing(8);
        row->addWidget(m_icon, 0, Qt::AlignTop);
        m_text->setObjectName(u"messageText"_s);
        m_text->setTextFormat(Qt::PlainText);
        m_text->setWordWrap(true);
        m_text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        row->addWidget(m_text, 1);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

    void setMessage(Severity severity, const QString &text)
    {
        m_severity = severity;
        m_text->setText(text);
        const bool error = severity == Severity::Error;
        setAccessibleName(error ? u"Error"_s : u"Warning"_s);
        setAccessibleDescription(text);
        updateIcon();
    }

    QString text() const
    {
        return m_text->text();
    }

protected:
    void changeEvent(QEvent *event) override
    {
        QFrame::changeEvent(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
            updateIcon();
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor window = palette().color(QPalette::Window);
        const QColor text = palette().color(QPalette::WindowText);
        painter.setPen(QPen(mixColors(window, text, 0.3), 1));
        painter.setBrush(mixColors(window, text, 0.06));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), RowRadius, RowRadius);
    }

private:
    void updateIcon()
    {
        const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        const QString name = m_severity == Severity::Error ? u"dialog-error"_s : u"dialog-warning"_s;
        m_icon->setPixmap(QIcon::fromTheme(name).pixmap(QSize(size, size), devicePixelRatioF()));
        m_icon->setFixedSize(size, size);
    }

    QLabel *m_icon;
    QLabel *m_text;
    Severity m_severity = Severity::Error;
};

ElidedLabel::ElidedLabel(QWidget *parent)
    : QFrame(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString &text)
{
    m_text = text;
    // Force rich text with escaped content so markup in a URL is shown literally.
    setToolTip(text.isEmpty() ? QString() : u"<qt>%1</qt>"_s.arg(text.toHtmlEscaped()));
    setAccessibleName(text);
    updateGeometry();
    update();
}

QString ElidedLabel::text() const
{
    return m_text;
}

QString ElidedLabel::displayedText() const
{
    return fontMetrics().elidedText(m_text, Qt::ElideMiddle, contentsRect().width());
}

QSize ElidedLabel::sizeHint() const
{
    return {std::min(fontMetrics().horizontalAdvance(m_text), 400), fontMetrics().height()};
}

QSize ElidedLabel::minimumSizeHint() const
{
    return {fontMetrics().averageCharWidth() * 4, fontMetrics().height()};
}

void ElidedLabel::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter, displayedText());
}

ChooserWindow::ChooserWindow(QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_model(new BrowserListModel(this))
    , m_filter(new BrowserFilterModel(this))
{
    setObjectName(u"chooserCard"_s);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(u"Open link with — %1"_s.arg(AppDisplayName));
    setWindowIcon(appIcon());
    setAccessibleName(u"Open link with"_s);
    m_filter->setSourceModel(m_model);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(8);

    // Header: glyph and heading.
    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *glyph = new QLabel(this);
    glyph->setPixmap(appIcon().pixmap(QSize(28, 28), devicePixelRatioF()));
    glyph->setAccessibleName(AppDisplayName.toString());
    header->addWidget(glyph);
    m_heading = new QLabel(u"Open link with"_s, this);
    m_heading->setObjectName(u"heading"_s);
    QFont headingFont = m_heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.2);
    headingFont.setWeight(QFont::DemiBold);
    m_heading->setFont(headingFont);
    header->addWidget(m_heading, 1);
    root->addLayout(header);

    auto *content = new QWidget;
    content->setObjectName(u"chooserContent"_s);
    auto *column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(root->spacing());
    m_content = new ContentArea(content, this);
    m_content->setObjectName(u"chooserContentArea"_s);
    m_content->setAccessibleName(u"Link details and messages"_s);
    root->addWidget(m_content);

    // The URL that will be opened.
    auto *urlRow = new QHBoxLayout;
    urlRow->setSpacing(4);
    m_finalUrl = new ElidedLabel(content);
    m_finalUrl->setObjectName(u"finalUrlLabel"_s);
    m_finalUrl->setAccessibleDescription(u"Destination: the link that will be opened"_s);
    urlRow->addWidget(m_finalUrl, 1);
    m_copy = flatToolButton(u"edit-copy"_s, u"Copy destination link"_s, content);
    m_copy->setObjectName(u"copyUrlButton"_s);
    connect(m_copy, &QToolButton::clicked, this, [this] {
        copyText(m_copy, m_finalUrl->text());
    });
    urlRow->addWidget(m_copy);
    m_details = new QToolButton(content);
    m_details->setObjectName(u"detailsButton"_s);
    m_details->setAutoRaise(true);
    m_details->setCheckable(true);
    m_details->setArrowType(Qt::RightArrow);
    m_details->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_details->setText(u"Details"_s);
    m_details->setAccessibleName(u"Show link details"_s);
    urlRow->addWidget(m_details);
    column->addLayout(urlRow);

    m_detailsPanel = new QWidget(content);
    m_detailsPanel->setObjectName(u"detailsPanel"_s);
    auto *details = new QGridLayout(m_detailsPanel);
    details->setContentsMargins(0, 0, 0, 0);
    details->setHorizontalSpacing(8);
    details->setVerticalSpacing(2);
    const auto caption = [this](const QString &text) {
        auto *label = new QLabel(text, m_detailsPanel);
        label->setForegroundRole(QPalette::PlaceholderText); // follows theme changes
        return label;
    };
    details->addWidget(caption(u"Original"_s), 0, 0, Qt::AlignVCenter);
    m_originalUrl = new ElidedLabel(m_detailsPanel);
    m_originalUrl->setObjectName(u"originalUrlLabel"_s);
    m_originalUrl->setAccessibleDescription(u"Original: the link as it was received"_s);
    details->addWidget(m_originalUrl, 0, 1);
    m_copyOriginal = flatToolButton(u"edit-copy"_s, u"Copy original link"_s, m_detailsPanel);
    m_copyOriginal->setObjectName(u"copyOriginalButton"_s);
    connect(m_copyOriginal, &QToolButton::clicked, this, [this] {
        copyText(m_copyOriginal, m_originalUrl->text());
    });
    details->addWidget(m_copyOriginal, 0, 2);
    details->addWidget(caption(u"Status"_s), 1, 0, Qt::AlignTop);
    m_redirectStatus = new QLabel(m_detailsPanel);
    m_redirectStatus->setObjectName(u"redirectStatusLabel"_s);
    m_redirectStatus->setTextFormat(Qt::PlainText);
    m_redirectStatus->setWordWrap(true);
    details->addWidget(m_redirectStatus, 1, 1, 1, 2);
    details->setColumnStretch(1, 1);
    m_detailsPanel->hide();
    column->addWidget(m_detailsPanel);
    connect(m_details, &QToolButton::toggled, this, [this](bool open) {
        m_details->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        m_details->setAccessibleName(open ? u"Hide link details"_s : u"Show link details"_s);
        m_detailsPanel->setVisible(open);
        updateSize();
    });

    // Busy state while the link is preprocessed.
    m_busyRow = new QWidget(content);
    m_busyRow->setObjectName(u"busyRow"_s);
    auto *busy = new QHBoxLayout(m_busyRow);
    busy->setContentsMargins(0, 0, 0, 0);
    m_busyIndicator = new KBusyIndicatorWidget(m_busyRow);
    m_busyIndicator->setFixedSize(16, 16);
    busy->addWidget(m_busyIndicator);
    m_busyLabel = new QLabel(m_busyRow);
    m_busyLabel->setTextFormat(Qt::PlainText);
    busy->addWidget(m_busyLabel, 1);
    m_skip = new QPushButton(u"Skip"_s, m_busyRow);
    m_skip->setObjectName(u"skipButton"_s);
    m_skip->setFlat(true);
    m_skip->setToolTip(u"Stop following redirects and use the link as received"_s);
    connect(m_skip, &QPushButton::clicked, this, &ChooserWindow::resolutionSkipped);
    busy->addWidget(m_skip);
    m_busyRow->hide();
    column->addWidget(m_busyRow);

    m_message = new InlineMessage(content);
    m_message->setObjectName(u"messageWidget"_s);
    m_message->hide();
    column->addWidget(m_message);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setObjectName(u"filterEdit"_s);
    m_filterEdit->setPlaceholderText(u"Filter browsers"_s);
    m_filterEdit->setAccessibleName(u"Filter browsers"_s);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->hide();
    m_filterEdit->installEventFilter(this);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ChooserWindow::setFilterText);
    root->addWidget(m_filterEdit);

    m_list = new BrowserListView(this);
    m_list->setObjectName(u"browserList"_s);
    m_list->setAccessibleName(u"Browsers"_s);
    m_list->setModel(m_filter);
    m_list->setItemDelegate(new BrowserDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setMouseTracking(true);
    m_list->setAttribute(Qt::WA_Hover);
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setAutoFillBackground(false);
    m_list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_list->installEventFilter(this);
    connect(m_list, &QListView::clicked, this, &ChooserWindow::choose);
    root->addWidget(m_list, 1);

    m_emptyState = new QWidget(content);
    m_emptyState->setObjectName(u"emptyState"_s);
    auto *empty = new QVBoxLayout(m_emptyState);
    empty->setContentsMargins(8, 12, 8, 12);
    empty->setSpacing(10);
    auto *emptyIcon = new QLabel(m_emptyState);
    emptyIcon->setPixmap(QIcon::fromTheme(u"internet-web-browser"_s).pixmap(QSize(48, 48), devicePixelRatioF(), QIcon::Disabled));
    emptyIcon->setAlignment(Qt::AlignCenter);
    empty->addWidget(emptyIcon);
    m_emptyText = new QLabel(m_emptyState);
    m_emptyText->setObjectName(u"emptyStateText"_s);
    m_emptyText->setTextFormat(Qt::PlainText);
    m_emptyText->setWordWrap(true);
    m_emptyText->setAlignment(Qt::AlignCenter);
    empty->addWidget(m_emptyText);
    m_emptySettings = new QPushButton(QIcon::fromTheme(u"configure"_s), u"Open Settings…"_s, m_emptyState);
    m_emptySettings->setObjectName(u"emptyStateSettingsButton"_s);
    connect(m_emptySettings, &QPushButton::clicked, this, &ChooserWindow::settingsRequested);
    empty->addWidget(m_emptySettings, 0, Qt::AlignCenter);
    m_emptyState->hide();
    column->addWidget(m_emptyState);

    root->addWidget(new KSeparator(this));
    auto *footer = new QHBoxLayout;
    footer->setSpacing(6);
    m_settings = new QPushButton(QIcon::fromTheme(u"configure"_s), u"Settings…"_s, this);
    m_settings->setObjectName(u"settingsButton"_s);
    m_settings->setAccessibleName(u"Settings"_s);
    m_settings->setFlat(true);
    m_settings->setAutoDefault(false);
    connect(m_settings, &QPushButton::clicked, this, &ChooserWindow::settingsRequested);
    footer->addWidget(m_settings);
    footer->addStretch(1);
    m_cancel = new QPushButton(QIcon::fromTheme(u"dialog-cancel"_s), u"Cancel"_s, this);
    m_cancel->setObjectName(u"cancelButton"_s);
    m_cancel->setAccessibleName(u"Cancel"_s);
    m_cancel->setFlat(true);
    m_cancel->setAutoDefault(false);
    m_cancel->setToolTip(u"Open nothing (Esc)"_s);
    connect(m_cancel, &QPushButton::clicked, this, &ChooserWindow::emitCancelled);
    footer->addWidget(m_cancel);
    root->addLayout(footer);

    setMinimumSize(200, 160);
    setFocusProxy(m_list);
    // Notices when the user moves the keyboard focus to one of these.
    for (QWidget *control : {static_cast<QWidget *>(m_content), static_cast<QWidget *>(m_copy), static_cast<QWidget *>(m_details),
                             static_cast<QWidget *>(m_copyOriginal), static_cast<QWidget *>(m_skip),
                             static_cast<QWidget *>(m_emptySettings), static_cast<QWidget *>(m_settings),
                             static_cast<QWidget *>(m_cancel)}) {
        control->installEventFilter(this);
    }
    updateState();
}

void ChooserWindow::setOriginalUrl(const QString &url)
{
    m_originalText = url;
    m_originalUrl->setText(url);
    if (m_finalText.isEmpty()) {
        m_finalUrl->setText(url);
    }
}

void ChooserWindow::setBusy(const QString &message)
{
    m_busy = true;
    m_busyLabel->setText(message);
    m_redirectStatus->setText(message);
    updateState();
}

bool ChooserWindow::isBusy() const
{
    return m_busy;
}

void ChooserWindow::setFinalUrl(const QString &url, const QString &status, bool warning)
{
    const bool wasBusy = m_busy;
    m_busy = false;
    m_finalText = url;
    m_finalUrl->setText(url);
    m_redirectStatus->setText(status);
    if (warning && !status.isEmpty()) {
        m_message->setMessage(InlineMessage::Severity::Warning, status);
        m_message->show();
    }
    updateState();
    if (wasBusy) {
        restoreListFocus();
    }
    updateSize();
}

QString ChooserWindow::finalUrl() const
{
    return m_finalText;
}

void ChooserWindow::setBrowsers(const QList<ChooserEntry> &entries, int hiddenCount)
{
    const QString current = m_list->currentIndex().data(BrowserListModel::IdRole).toString();
    m_hiddenCount = hiddenCount;
    m_model->setEntries(entries);
    int row = 0;
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        if (m_filter->index(i, 0).data(BrowserListModel::IdRole).toString() == current) {
            row = i;
        }
    }
    setCurrentRow(row);
    updateState();
    updateSize();
}

QStringList ChooserWindow::displayedBrowserIds() const
{
    QStringList ids;
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        ids.append(m_filter->index(i, 0).data(BrowserListModel::IdRole).toString());
    }
    return ids;
}

void ChooserWindow::showError(const QString &message)
{
    m_message->setMessage(InlineMessage::Severity::Error, message);
    m_message->show();
    updateSize();
    // Once laid out, scroll the error into view, starting at its first line.
    QTimer::singleShot(0, m_content, [content = m_content, message = m_message] {
        if (!message->isVisible()) {
            return;
        }
        if (message->height() <= content->viewport()->height()) {
            content->ensureWidgetVisible(message, 0, 0);
        } else {
            content->verticalScrollBar()->setValue(message->y());
        }
    });
}

void ChooserWindow::clearError()
{
    m_message->hide();
    m_message->setMessage(InlineMessage::Severity::Error, QString());
}

QString ChooserWindow::messageText() const
{
    return m_message->isHidden() ? QString() : m_message->text();
}

QSize ChooserWindow::preferredSize() const
{
    QLayout *root = layout();
    // The content's own layout request may still be queued.
    m_content->updateGeometry();
    root->invalidate();
    const int height = root->hasHeightForWidth() ? root->heightForWidth(CardWidth) : root->sizeHint().height();
    return {CardWidth, std::max(height, minimumHeight())};
}

void ChooserWindow::dismiss()
{
    m_closed = true;
    close();
}

void ChooserWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(mixColors(palette().color(QPalette::Window), palette().color(QPalette::WindowText), 0.22), 1));
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(card, CardRadius, CardRadius);
}

// The keyboard belongs to the browser list whenever the card is (re)shown.
void ChooserWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!event->spontaneous()) {
        m_userFocus = nullptr;
        restoreListFocus();
    }
}

void ChooserWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        emitCancelled();
        return;
    }
    if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9 && !(event->modifiers() & ~Qt::KeypadModifier)) {
        chooseRow(event->key() - Qt::Key_1);
        return;
    }
    QWidget::keyPressEvent(event);
}

bool ChooserWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::FocusIn) {
        if (watched == m_list || watched == m_filterEdit) {
            m_userFocus = nullptr;
        } else if (!m_updatingState) {
            switch (static_cast<QFocusEvent *>(event)->reason()) {
            case Qt::MouseFocusReason:
            case Qt::TabFocusReason:
            case Qt::BacktabFocusReason:
            case Qt::ShortcutFocusReason:
                m_userFocus = static_cast<QWidget *>(watched);
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    auto *key = static_cast<QKeyEvent *>(event);
    const bool plain = !(key->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier));

    if (watched == m_list) {
        switch (key->key()) {
        case Qt::Key_Escape:
            if (m_filterEdit->isVisible()) {
                m_filterEdit->clear();
                m_filterEdit->hide();
            } else {
                emitCancelled();
            }
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose(m_list->currentIndex());
            return true;
        default:
            break;
        }
        if (key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9 && plain) {
            chooseRow(key->key() - Qt::Key_1);
            return true;
        }
        // Any other printable text starts filtering.
        const QString text = key->text();
        if (plain && !text.isEmpty() && text.at(0).isPrint() && !text.at(0).isSpace() && !m_busy) {
            m_filterEdit->show();
            m_filterEdit->setFocus();
            m_filterEdit->setText(m_filterEdit->text() + text);
            updateState();
            return true;
        }
        return false;
    }

    if (watched == m_filterEdit) {
        switch (key->key()) {
        case Qt::Key_Escape:
            m_filterEdit->clear();
            m_filterEdit->hide();
            focusList();
            return true;
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            focusList();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose(m_list->currentIndex().isValid() ? m_list->currentIndex() : m_filter->index(0, 0));
            return true;
        default:
            return false;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChooserWindow::closeEvent(QCloseEvent *event)
{
    emitCancelled();
    QWidget::closeEvent(event);
}

void ChooserWindow::choose(const QModelIndex &index)
{
    if (m_busy || !index.isValid() || !m_list->isEnabled()) {
        return;
    }
    Q_EMIT browserChosen(index.data(BrowserListModel::IdRole).toString());
}

void ChooserWindow::chooseRow(int row)
{
    if (row < 0 || row >= m_filter->rowCount()) {
        return;
    }
    choose(m_filter->index(row, 0));
}

void ChooserWindow::updateState()
{
    // Hiding or disabling the focused widget moves the focus on (with
    // Qt::TabFocusReason); that is not the user's choice.
    const QScopedValueRollback updating(m_updatingState, true);
    const bool empty = m_model->rowCount() == 0;
    m_list->setVisible(!empty);
    m_emptyState->setVisible(empty);
    if (empty) {
        m_emptyText->setText(m_hiddenCount > 0 ? u"All %1 browsers are hidden. Show them again in Settings."_s.arg(m_hiddenCount)
                                               : u"No web browsers were found. Add one in Settings."_s);
    }
    m_busyRow->setVisible(m_busy);
    if (m_busy) {
        m_busyIndicator->start();
    } else {
        m_busyIndicator->stop();
    }
    m_list->setEnabled(!m_busy);
    m_filterEdit->setEnabled(!m_busy);
    m_copy->setEnabled(!m_finalUrl->text().isEmpty());
}

void ChooserWindow::setFilterText(const QString &text)
{
    m_filter->setFilterFixedString(text);
    setCurrentRow(0);
}

void ChooserWindow::setCurrentRow(int row)
{
    const QModelIndex index = m_filter->index(row, 0);
    m_list->setCurrentIndex(index);
    if (index.isValid()) {
        m_list->scrollTo(index);
    }
}

void ChooserWindow::focusList()
{
    m_list->setFocus();
    if (!m_list->currentIndex().isValid()) {
        setCurrentRow(0);
    }
}

// After the link is ready, unless the user moved to another control that is
// still there. Only sets the window's focus widget; never activates it.
void ChooserWindow::restoreListFocus()
{
    if (m_busy || m_model->rowCount() == 0) {
        return;
    }
    QWidget *focused = focusWidget();
    if (m_userFocus && focused == m_userFocus && m_userFocus->isVisibleTo(this) && m_userFocus->isEnabled()) {
        return;
    }
    focusList();
}

void ChooserWindow::copyText(QToolButton *button, const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    const QString label = button->accessibleName();
    button->setIcon(QIcon::fromTheme(u"checkmark"_s, QIcon::fromTheme(u"dialog-ok-apply"_s)));
    button->setToolTip(u"Copied"_s);
    QTimer::singleShot(1500, button, [button, label] {
        button->setIcon(QIcon::fromTheme(u"edit-copy"_s));
        button->setToolTip(label);
    });
}

void ChooserWindow::emitCancelled()
{
    if (m_closed) {
        return;
    }
    m_closed = true;
    Q_EMIT cancelled();
}

// Grows or shrinks a visible window to its contents, within its screen.
void ChooserWindow::updateSize()
{
    if (!isVisible()) {
        return;
    }
    QScreen *current = screen();
    const QRect available = current ? current->availableGeometry() : QRect();
    QSize size = preferredSize();
    if (available.isValid()) {
        size = size.boundedTo(kbs::maximumSize(available, 8));
    }
    if (size == this->size()) {
        return;
    }
    resize(size);
    // Layer-shell surfaces are positioned by the compositor; others are kept on screen.
    if (available.isValid() && !QGuiApplication::platformName().startsWith(u"wayland"_s)) {
        const QRect area = available.adjusted(8, 8, -8, -8);
        move(std::clamp(x(), area.left(), std::max(area.left(), area.right() - width() + 1)),
             std::clamp(y(), area.top(), std::max(area.top(), area.bottom() - height() + 1)));
    }
}

} // namespace kbs
