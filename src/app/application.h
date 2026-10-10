#pragma once

#include "app/linkpipeline.h"
#include "core/browsercatalog.h"
#include "core/config.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <memory>
#include <optional>

namespace klinker
{

class ChooserWindow;
class SettingsDialog;

// SelfIdentity::current() with this application's desktop file ID.
SelfIdentity appSelfIdentity();

// Runs one invocation: links are preprocessed and routed one after another,
// showing the chooser when no rule applies; settings can be opened at any time.
// Nothing is saved unless the user saves the settings. While the settings are
// open nothing is routed or launched; saving different privacy settings starts
// the current link again from the original URL. Nor is anything routed while
// the chooser decides whether losing the focus cancels the link
// (ChooserWindow::isFocusLossPending()): a cancellation drops the link, and
// otherwise it is routed once that is decided.
class Application : public QObject
{
    Q_OBJECT

public:
    struct Options {
        QString configPath; // empty: ConfigStore::defaultPath()
        std::optional<QStringList> dataDirs; // empty: defaultDataDirs()
        bool alwaysChoose = false; // --choose
        SelfIdentity self = appSelfIdentity();
    };

    explicit Application(const Options &options, QObject *parent = nullptr);
    ~Application() override;

    void openUrls(const QStringList &urls);
    void openSettings();

    Config config() const;
    ChooserWindow *chooser() const;
    SettingsDialog *settingsDialog() const;
    bool isIdle() const;
    // The current link is ready but its routing waits: for the settings to
    // close, or for the chooser's focus-loss decision.
    bool isRoutingDeferred() const;

Q_SIGNALS:
    void launched(const QString &browserId, const QString &url);
    // Nothing is queued and no window is open.
    void finished();

private:
    void processNext();
    void startCurrent();
    void onProcessed(const ProcessedLink &link);
    void route();
    void showChooser(const QString &error = QString());
    void ensureChooser();
    void closeChooser();
    void refreshChooserBrowsers();
    bool launch(const QString &browserId, QString *error);
    void finishCurrent();
    void checkFinished();

    Options m_options;
    std::unique_ptr<ConfigStore> m_store;
    Config m_config;
    QString m_loadError;
    QList<DetectedBrowser> m_detected;
    BrowserCatalog m_catalog;

    LinkPipeline m_pipeline;
    QStringList m_queue;
    QString m_currentUrl; // as received, while m_busy
    std::optional<ProcessedLink> m_current;
    bool m_busy = false; // a link is being processed or decided
    bool m_routePending = false; // m_current became ready while the settings were open or a focus loss was pending
    QPointer<ChooserWindow> m_chooser;
    QPointer<SettingsDialog> m_settings;
};

} // namespace klinker
