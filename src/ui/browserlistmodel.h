#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSortFilterProxyModel>
#include <QString>

namespace klinker
{

struct Browser;

// What the chooser shows for one browser.
struct ChooserEntry {
    QString id;
    QString name;
    QString icon; // theme icon name or image file path
    QString detail; // executable and source, shown subdued under the name

    bool operator==(const ChooserEntry &) const = default;
};

ChooserEntry chooserEntry(const Browser &browser);

class BrowserListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        DetailRole,
        ShortcutRole, // 1-9 for the first nine visible rows, otherwise 0
    };

    explicit BrowserListModel(QObject *parent = nullptr);

    void setEntries(const QList<ChooserEntry> &entries);
    QList<ChooserEntry> entries() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    QList<ChooserEntry> m_entries;
};

// Case-insensitive name filter that numbers the rows it lets through, so the
// number keys always match what is on screen.
class BrowserFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit BrowserFilterModel(QObject *parent = nullptr);

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
};

} // namespace klinker
