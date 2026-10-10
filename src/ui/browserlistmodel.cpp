#include "browserlistmodel.h"

#include "core/browsercatalog.h"
#include "theme.h"

#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

// "firefox", or "org.mozilla.firefox (Flatpak)" for `flatpak run ... <app>`.
QString programLabel(const CommandTemplate &command)
{
    QString program = effectiveProgram(command);
    if (program.isEmpty()) {
        program = command.program;
    }
    const QString name = QFileInfo(program).fileName();
    if (name == u"flatpak"_s) {
        const qsizetype run = command.arguments.indexOf(u"run"_s);
        for (qsizetype i = run + 1; run >= 0 && i < command.arguments.size(); ++i) {
            if (!command.arguments[i].startsWith(u'-')) {
                return u"%1 (Flatpak)"_s.arg(command.arguments[i]);
            }
        }
    }
    return name;
}

} // namespace

ChooserEntry chooserEntry(const Browser &browser)
{
    QString source;
    if (browser.source == BrowserSource::Manual) {
        source = u"Added manually"_s;
    } else if (browser.customized) {
        source = u"Customised"_s;
    } else {
        source = u"Detected"_s;
    }
    const QString program = programLabel(browser.command);
    return {browser.id, browser.name, browser.icon, program.isEmpty() ? source : u"%1 · %2"_s.arg(program, source)};
}

BrowserListModel::BrowserListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void BrowserListModel::setEntries(const QList<ChooserEntry> &entries)
{
    beginResetModel();
    m_entries = entries;
    endResetModel();
}

QList<ChooserEntry> BrowserListModel::entries() const
{
    return m_entries;
}

int BrowserListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant BrowserListModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const ChooserEntry &entry = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return entry.name;
    case Qt::DecorationRole:
        return browserIcon(entry.icon);
    case Qt::ToolTipRole:
    case DetailRole:
    case Qt::AccessibleDescriptionRole:
        return entry.detail;
    case IdRole:
        return entry.id;
    default:
        return {};
    }
}

BrowserFilterModel::BrowserFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterRole(Qt::DisplayRole);
}

QVariant BrowserFilterModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const int shortcut = index.row() < 9 ? index.row() + 1 : 0;
    if (role == BrowserListModel::ShortcutRole) {
        return shortcut;
    }
    if (role == Qt::AccessibleTextRole) {
        const QString name = QSortFilterProxyModel::data(index, Qt::DisplayRole).toString();
        return shortcut ? u"%1, press %2"_s.arg(name).arg(shortcut) : name;
    }
    // Names may be hidden (horizontal chooser), so the tooltip always has one.
    if (role == Qt::ToolTipRole) {
        const QString name = QSortFilterProxyModel::data(index, Qt::DisplayRole).toString().toHtmlEscaped();
        const QString detail = QSortFilterProxyModel::data(index, BrowserListModel::DetailRole).toString().toHtmlEscaped();
        const QString title = shortcut ? u"<b>%1</b> — press %2"_s.arg(name).arg(shortcut) : u"<b>%1</b>"_s.arg(name);
        return detail.isEmpty() ? u"<qt>%1</qt>"_s.arg(title) : u"<qt>%1<br>%2</qt>"_s.arg(title, detail);
    }
    return QSortFilterProxyModel::data(index, role);
}

} // namespace klinker
