#pragma once

#include "core/browsercatalog.h"
#include "core/execcommand.h"

#include <QDialog>

class KMessageWidget;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace klinker
{

// Adds or edits a browser: display name, program, one argument per line and an
// icon (theme name or image file). OK stays disabled while the entry is unusable.
// Arguments the user leaves untouched are kept exactly, including empty ones,
// which one-per-line text cannot express.
class BrowserEditDialog : public QDialog
{
    Q_OBJECT

public:
    BrowserEditDialog(const BrowserFields &fields, const SelfIdentity &self, QWidget *parent = nullptr);

    BrowserFields fields() const;
    QStringList errors() const;
    QStringList warnings() const;

private:
    void validate();
    void updateIconPreview();
    QStringList arguments() const;

    SelfIdentity m_self;
    QStringList m_originalArguments;
    QString m_originalArgumentText;
    QLineEdit *m_name;
    QLineEdit *m_program;
    QPlainTextEdit *m_arguments;
    QLineEdit *m_icon;
    QLabel *m_iconPreview;
    KMessageWidget *m_problems;
    QDialogButtonBox *m_buttons;
    QStringList m_errors;
    QStringList m_warnings;
};

} // namespace klinker
