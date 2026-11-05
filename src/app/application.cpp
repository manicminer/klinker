#include "application.h"

#include "app/launcher.h"
#include "core/browserdiscovery.h"
#include "core/rules.h"
#include "ui/chooserwindow.h"
#include "ui/placement.h"
#include "ui/settingsdialog.h"
#include "ui/theme.h"

#include <QDebug>

using namespace Qt::StringLiterals;

namespace klinker
{

SelfIdentity appSelfIdentity()
{
    SelfIdentity self = SelfIdentity::current();
    const QString desktopId = AppId.toString() + u".desktop"_s;
    self.desktopIds.removeAll(desktopId);
    self.desktopIds.prepend(desktopId);
    return self;
}

QString browserListing(const QString &configPath, const std::optional<QStringList> &dataDirs)
{
    ConfigStore store(configPath.isEmpty() ? ConfigStore::defaultPath() : configPath);
    const ConfigStore::LoadResult loaded = store.load();
    const SelfIdentity self = appSelfIdentity();
    const BrowserCatalog catalog(discoverBrowsers(dataDirs.value_or(defaultDataDirs()), self).browsers, loaded.config.browsers);

    QStringList lines;
    if (loaded.status == ConfigStore::LoadStatus::Invalid) {
        lines.append(u"error: %1: %2"_s.arg(store.path(), loaded.error));
    }
    for (const Browser &browser : catalog.browsers()) {
        const CommandValidation validation = validateCommand(browser.command, self);
        QStringList status = validation.errors + validation.warnings;
        if (status.isEmpty()) {
            status.append(browser.hidden ? u"hidden"_s : u"ok"_s);
        }
        const QString command = (QStringList{browser.command.program} + browser.command.arguments).join(u' ');
        lines.append(QStringList{browser.id, browser.name, command, status.join(u' ')}.join(u'\t'));
    }
    return lines.isEmpty() ? QString() : lines.join(u'\n') + u'\n';
}

Application::Application(const Options &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
    , m_store(std::make_unique<ConfigStore>(options.configPath.isEmpty() ? ConfigStore::defaultPath() : options.configPath))
{
    const ConfigStore::LoadResult loaded = m_store->load();
    m_config = loaded.config;
    if (loaded.status == ConfigStore::LoadStatus::Invalid) {
        m_loadError = loaded.error;
    }
    for (const QString &warning : loaded.warnings) {
        qWarning().noquote() << m_store->path() << warning;
    }
    m_detected = discoverBrowsers(m_options.dataDirs.value_or(defaultDataDirs()), m_options.self).browsers;
    m_catalog = BrowserCatalog(m_detected, m_config.browsers);
    connect(&m_pipeline, &LinkPipeline::finished, this, &Application::onProcessed);
}

Application::~Application()
{
    delete m_chooser.data();
    delete m_settings.data();
}

void Application::openUrls(const QStringList &urls)
{
    m_queue += urls;
    processNext();
}

Config Application::config() const
{
    return m_config;
}

ChooserWindow *Application::chooser() const
{
    return m_chooser;
}

SettingsDialog *Application::settingsDialog() const
{
    return m_settings;
}

bool Application::isIdle() const
{
    return m_queue.isEmpty() && !m_busy && !m_chooser && !m_settings;
}

void Application::processNext()
{
    if (m_busy) {
        return;
    }
    if (m_queue.isEmpty()) {
        checkFinished();
        return;
    }
    m_currentUrl = m_queue.takeFirst();
    m_busy = true;
    startCurrent();
}

// (Re)starts preprocessing of the current link with the current settings.
void Application::startCurrent()
{
    m_pipeline.cancel();
    m_current.reset();
    m_routePending = false;
    if (m_chooser) {
        // Nothing stale can be chosen until the link is ready again.
        m_chooser->setBusy(u"Applying privacy settings…"_s);
    }
    if (LinkPipeline::needsNetwork(m_currentUrl, m_config.privacy)) {
        // Network checks take a moment: show the chooser in its busy state.
        ensureChooser();
        m_chooser->setOriginalUrl(m_currentUrl);
        m_chooser->setBusy(u"Following redirects…"_s);
        showChooser();
    }
    m_pipeline.start(m_currentUrl, m_config.privacy);
}

void Application::onProcessed(const ProcessedLink &link)
{
    m_current = link;
    if (m_settings) {
        m_routePending = true; // decided with the final settings once they close
        return;
    }
    if (m_chooser && m_chooser->isFocusLossPending()) {
        // Dropped if the chooser is cancelled, routed if the user is still there.
        m_routePending = true;
        return;
    }
    route();
}

bool Application::isRoutingDeferred() const
{
    return m_current && m_routePending;
}

void Application::route()
{
    const ProcessedLink &link = *m_current;
    if (!m_options.alwaysChoose && !link.warning) {
        const RouteResult match = RuleEngine(m_config.rules, m_catalog.browserIds()).route(link.finalUrl);
        if (match.matched()) {
            QString error;
            if (launch(match.browserId, &error)) {
                finishCurrent();
                return;
            }
            showChooser(u"Rule %1 could not be used. %2"_s.arg(match.ruleIndex + 1).arg(error));
            return;
        }
    }
    showChooser();
}

void Application::ensureChooser()
{
    if (m_chooser) {
        return;
    }
    m_chooser = new ChooserWindow;
    connect(m_chooser, &ChooserWindow::browserChosen, this, [this](const QString &id) {
        if (!m_current) {
            return;
        }
        QString error;
        if (launch(id, &error)) {
            finishCurrent();
        } else if (m_chooser) {
            m_chooser->showError(error);
        }
    });
    connect(m_chooser, &ChooserWindow::cancelled, this, [this] {
        if (m_pipeline.isActive()) {
            m_pipeline.cancel();
        }
        finishCurrent();
    });
    connect(m_chooser, &ChooserWindow::focusLossResolved, this, [this, chooser = m_chooser.data()] {
        // While the settings are open, routing waits for them instead.
        if (chooser == m_chooser && m_current && m_routePending && !m_settings && !chooser->isFocusLossPending()) {
            m_routePending = false;
            route();
        }
    });
    connect(m_chooser, &ChooserWindow::settingsRequested, this, &Application::openSettings);
    connect(m_chooser, &ChooserWindow::resolutionSkipped, &m_pipeline, &LinkPipeline::skip);
    m_chooser->setPreferences(m_config.chooser);
    refreshChooserBrowsers();
}

void Application::showChooser(const QString &error)
{
    ensureChooser();
    if (m_current) {
        m_chooser->setOriginalUrl(m_current->originalUrl);
        m_chooser->setFinalUrl(m_current->finalUrl, m_current->status, m_current->warning);
    }
    QStringList messages;
    if (!m_loadError.isEmpty()) {
        messages.append(u"Settings could not be read, so defaults are used: %1"_s.arg(m_loadError));
    }
    if (!error.isEmpty()) {
        messages.append(error);
    }
    if (!messages.isEmpty()) {
        m_chooser->showError(messages.join(u'\n'));
    }
    refreshChooserBrowsers();

    if (m_settings || m_chooser->isVisible()) {
        return; // shown again when the settings close
    }
    if (!m_chooser->property("klinkerPlaced").toBool()) {
        placePopup(m_chooser, [chooser = m_chooser.data()](const QSize &maximum) {
            return chooser->preferredSize(maximum);
        });
        m_chooser->setProperty("klinkerPlaced", true);
    } else {
        // Back from the settings, perhaps in another style or with other
        // browsers: sized again on the screen it was on.
        m_chooser->refit();
    }
    m_chooser->show();
    m_chooser->raise();
    m_chooser->activateWindow();
}

void Application::closeChooser()
{
    if (ChooserWindow *chooser = m_chooser) {
        m_chooser = nullptr;
        chooser->dismiss();
        chooser->deleteLater();
    }
}

void Application::refreshChooserBrowsers()
{
    if (!m_chooser) {
        return;
    }
    QList<ChooserEntry> entries;
    for (const Browser &browser : m_catalog.visibleBrowsers()) {
        entries.append(chooserEntry(browser));
    }
    m_chooser->setBrowsers(entries, int(m_catalog.browsers().size() - entries.size()));
}

bool Application::launch(const QString &browserId, QString *error)
{
    const std::optional<Browser> browser = m_catalog.browser(browserId);
    if (!browser) {
        *error = u"The browser “%1” is no longer available."_s.arg(browserId);
        return false;
    }
    // Exactly what was shown and routed: the pipeline has already applied the
    // privacy settings, and is restarted whenever they change.
    const QString url = m_current->finalUrl;
    const LaunchResult result = launchBrowser(*browser, url, m_options.self);
    if (!result.ok) {
        *error = result.error;
        return false;
    }
    Q_EMIT launched(browserId, url);
    return true;
}

void Application::finishCurrent()
{
    m_current.reset();
    m_currentUrl.clear();
    m_routePending = false;
    m_busy = false;
    closeChooser();
    processNext();
}

void Application::openSettings()
{
    if (m_settings) {
        m_settings->raise();
        m_settings->activateWindow();
        return;
    }
    // The dialog saves and reloads through its own copy of the store. Its
    // baseline is taken over only together with a configuration it saved, so
    // "Reload from Disk" followed by Cancel cannot pair m_config with a newer
    // file and silently overwrite another invocation's changes later.
    const auto store = std::make_shared<ConfigStore>(*m_store);
    SettingsDialog::Context context;
    context.store = store.get();
    context.config = m_config;
    context.loadError = m_loadError;
    context.detected = m_detected;
    context.dataDirs = m_options.dataDirs.value_or(defaultDataDirs());
    context.self = m_options.self;
    m_settings = new SettingsDialog(context);
    // Keeps the store alive for as long as the dialog, even past this object.
    connect(m_settings, &QObject::destroyed, m_settings, [store] { });

    connect(m_settings, &SettingsDialog::saved, this, [this, store](const Config &config, const QList<DetectedBrowser> &detected) {
        *m_store = *store;
        const bool privacyChanged = config.privacy != m_config.privacy;
        m_config = config;
        m_detected = detected;
        m_catalog = BrowserCatalog(m_detected, m_config.browsers);
        if (!m_loadError.isEmpty() && m_chooser) {
            m_chooser->clearError();
        }
        m_loadError.clear();
        if (m_chooser) {
            m_chooser->setPreferences(m_config.chooser);
        }
        refreshChooserBrowsers();
        if (privacyChanged && m_busy) {
            // What was routed, shown and launched must all come from one run.
            startCurrent();
        }
    });
    connect(m_settings, &QDialog::finished, this, [this] {
        if (SettingsDialog *dialog = m_settings) {
            m_settings = nullptr;
            dialog->deleteLater();
        }
        if (m_current && m_routePending) {
            m_routePending = false;
            route();
            return;
        }
        if (m_chooser && m_busy) {
            showChooser();
        }
        checkFinished();
    });

    // The chooser stays above normal windows, so it steps aside meanwhile.
    if (m_chooser) {
        m_chooser->hide();
    }
    m_settings->show();
    m_settings->raise();
    m_settings->activateWindow();
}

void Application::checkFinished()
{
    if (isIdle()) {
        Q_EMIT finished();
    }
}

} // namespace klinker
