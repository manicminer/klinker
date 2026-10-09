#pragma once

#include "browserlistmodel.h"

#include <QFrame>
#include <QPointer>
#include <QWidget>

class KBusyIndicatorWidget;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QScrollArea;
class QToolButton;

namespace kbs
{

class BrowserFilterModel;
class InlineMessage;

// Single-line plain text, elided in the middle; the full text is in the
// tooltip and accessible name.
class ElidedLabel : public QFrame
{
    Q_OBJECT

public:
    explicit ElidedLabel(QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;
    QString displayedText() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
};

// The borderless "Open link with" card. It only reports choices; the caller
// launches browsers and reports failures back with showError().
class ChooserWindow : public QWidget
{
    Q_OBJECT

public:
    static constexpr int CardWidth = 430;
    static constexpr int MaxShownRows = 9;

    explicit ChooserWindow(QWidget *parent = nullptr);

    // The link as received; shown under "Details".
    void setOriginalUrl(const QString &url);
    // Preprocessing is running: choosing is disabled until setFinalUrl().
    void setBusy(const QString &message);
    bool isBusy() const;
    // The URL that will be launched, and a description of how it was obtained.
    void setFinalUrl(const QString &url, const QString &status, bool warning = false);
    QString finalUrl() const;

    // Visible browsers in user order. hiddenCount only improves the empty state.
    void setBrowsers(const QList<ChooserEntry> &entries, int hiddenCount = 0);
    // Browser IDs as displayed (after filtering), i.e. what keys 1-9 select.
    QStringList displayedBrowserIds() const;

    // Persistent until replaced or cleared; shown as plain text.
    void showError(const QString &message);
    void clearError();
    // The error or warning shown, or an empty string.
    QString messageText() const;

    // Width CardWidth and the height needed for up to MaxShownRows rows. A
    // smaller window still works: the browser list and the link details and
    // messages above it scroll separately.
    QSize preferredSize() const;

    // Closes the window without reporting a cancellation.
    void dismiss();

Q_SIGNALS:
    void browserChosen(const QString &id);
    void settingsRequested();
    void resolutionSkipped();
    void cancelled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void choose(const QModelIndex &index);
    void chooseRow(int row);
    void updateState();
    void setFilterText(const QString &text);
    void setCurrentRow(int row);
    void focusList();
    void restoreListFocus();
    void copyText(QToolButton *button, const QString &text);
    void emitCancelled();
    void updateSize();

    BrowserListModel *m_model;
    BrowserFilterModel *m_filter;
    QLabel *m_heading;
    QScrollArea *m_content; // link, details, progress and messages
    ElidedLabel *m_finalUrl;
    QToolButton *m_copy;
    QToolButton *m_details;
    QWidget *m_detailsPanel;
    ElidedLabel *m_originalUrl;
    QToolButton *m_copyOriginal;
    QLabel *m_redirectStatus;
    QWidget *m_busyRow;
    KBusyIndicatorWidget *m_busyIndicator;
    QLabel *m_busyLabel;
    QPushButton *m_skip;
    InlineMessage *m_message;
    QLineEdit *m_filterEdit;
    QListView *m_list;
    QWidget *m_emptyState;
    QLabel *m_emptyText;
    QPushButton *m_emptySettings;
    QPushButton *m_settings;
    QPushButton *m_cancel;

    QString m_originalText;
    QString m_finalText;
    bool m_busy = false;
    bool m_closed = false; // cancelled or dismissed
    bool m_updatingState = false; // focus moves now are Qt's, not the user's
    QPointer<QWidget> m_userFocus; // a control other than the list the user moved to
    int m_hiddenCount = 0;
};

} // namespace kbs
