#include "browsereditdialog.h"

#include "theme.h"

#include <KIconDialog>
#include <KMessageWidget>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace klinker
{

namespace
{

// QFormLayout only sets up mnemonics for rows whose field is a widget.
QLabel *buddyLabel(const QString &text, QWidget *buddy)
{
    auto *label = new QLabel(text, buddy->parentWidget());
    label->setBuddy(buddy);
    return label;
}

} // namespace

BrowserEditDialog::BrowserEditDialog(const BrowserFields &fields, const SelfIdentity &self, QWidget *parent)
    : QDialog(parent)
    , m_self(self)
    , m_originalArguments(fields.command.arguments)
    , m_originalArgumentText(formatArgumentLines(fields.command.arguments))
{
    setWindowTitle(fields.name.isEmpty() && fields.command.program.isEmpty() ? u"Add Browser"_s : u"Edit Browser"_s);
    setMinimumWidth(480);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_name = new QLineEdit(fields.name, this);
    m_name->setObjectName(u"nameEdit"_s);
    m_name->setPlaceholderText(u"Shown in the chooser"_s);
    form->addRow(u"&Name:"_s, m_name);

    auto *programRow = new QHBoxLayout;
    m_program = new QLineEdit(fields.command.program, this);
    m_program->setObjectName(u"programEdit"_s);
    m_program->setPlaceholderText(u"Program name in PATH or full path"_s);
    programRow->addWidget(m_program, 1);
    auto *browseProgram = new QPushButton(QIcon::fromTheme(u"document-open"_s), u"Browse…"_s, this);
    browseProgram->setObjectName(u"browseProgramButton"_s);
    connect(browseProgram, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, u"Choose Program"_s, m_program->text());
        if (!file.isEmpty()) {
            m_program->setText(file);
        }
    });
    programRow->addWidget(browseProgram);
    form->addRow(buddyLabel(u"&Program:"_s, m_program), programRow);

    m_arguments = new QPlainTextEdit(this);
    m_arguments->setObjectName(u"argumentsEdit"_s);
    m_arguments->setPlainText(m_originalArgumentText);
    m_arguments->setTabChangesFocus(true);
    m_arguments->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_arguments->setFixedHeight(m_arguments->fontMetrics().lineSpacing() * 5 + 12);
    m_arguments->setAccessibleName(u"Arguments, one per line"_s);
    form->addRow(u"&Arguments:"_s, m_arguments);
    auto *hint = new QLabel(u"One argument per line, used exactly as written: no quoting is needed and nothing is "
                            u"interpreted by a shell. {url} is replaced by the link; without it the link is added as "
                            u"the last argument."_s,
                            this);
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    hint->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), hint);

    auto *iconRow = new QHBoxLayout;
    m_iconPreview = new QLabel(this);
    m_iconPreview->setFixedSize(32, 32);
    iconRow->addWidget(m_iconPreview);
    m_icon = new QLineEdit(fields.icon, this);
    m_icon->setObjectName(u"iconEdit"_s);
    m_icon->setPlaceholderText(u"Icon name or image file"_s);
    iconRow->addWidget(m_icon, 1);
    auto *chooseIcon = new QPushButton(QIcon::fromTheme(u"preferences-desktop-icons"_s), u"Choose…"_s, this);
    chooseIcon->setObjectName(u"chooseIconButton"_s);
    chooseIcon->setToolTip(u"Pick an icon from the icon theme"_s);
    connect(chooseIcon, &QPushButton::clicked, this, [this] {
        const QString icon = KIconDialog::getIcon(KIconLoader::Desktop, KIconLoader::Application, false, 0, false, this);
        if (!icon.isEmpty()) {
            m_icon->setText(icon);
        }
    });
    iconRow->addWidget(chooseIcon);
    auto *imageFile = new QPushButton(QIcon::fromTheme(u"document-open"_s), u"Image…"_s, this);
    imageFile->setObjectName(u"browseIconButton"_s);
    imageFile->setToolTip(u"Use an image file as the icon"_s);
    connect(imageFile, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, u"Choose Icon Image"_s, QString(), u"Images (*.png *.svg *.svgz *.xpm *.jpg *.jpeg *.webp *.ico)"_s);
        if (!file.isEmpty()) {
            m_icon->setText(file);
        }
    });
    iconRow->addWidget(imageFile);
    form->addRow(buddyLabel(u"&Icon:"_s, m_icon), iconRow);

    root->addLayout(form);

    m_problems = new KMessageWidget(this);
    m_problems->setObjectName(u"problemsMessage"_s);
    m_problems->setTextFormat(Qt::PlainText);
    m_problems->setWordWrap(true);
    m_problems->setCloseButtonVisible(false);
    m_problems->hide();
    root->addWidget(m_problems);
    root->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(u"buttonBox"_s);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &BrowserEditDialog::validate);
    connect(m_program, &QLineEdit::textChanged, this, &BrowserEditDialog::validate);
    connect(m_arguments, &QPlainTextEdit::textChanged, this, &BrowserEditDialog::validate);
    connect(m_icon, &QLineEdit::textChanged, this, &BrowserEditDialog::updateIconPreview);
    validate();
    updateIconPreview();
}

BrowserFields BrowserEditDialog::fields() const
{
    return {m_name->text().trimmed(), m_icon->text().trimmed(), {m_program->text().trimmed(), arguments()}};
}

QStringList BrowserEditDialog::arguments() const
{
    const QString text = m_arguments->toPlainText();
    return text == m_originalArgumentText ? m_originalArguments : parseArgumentLines(text);
}

QStringList BrowserEditDialog::errors() const
{
    return m_errors;
}

QStringList BrowserEditDialog::warnings() const
{
    return m_warnings;
}

void BrowserEditDialog::validate()
{
    const BrowserFields current = fields();
    m_errors.clear();
    m_warnings.clear();
    if (current.name.isEmpty()) {
        m_errors.append(u"Enter a name."_s);
    }
    const CommandValidation command = validateCommand(current.command, m_self);
    m_errors += command.errors;
    m_warnings += command.warnings;
    const auto blank = [](const QString &argument) {
        return argument.trimmed().isEmpty();
    };
    if (std::ranges::any_of(m_originalArguments, blank) && current.command.arguments != m_originalArguments) {
        m_warnings.append(u"The command had empty arguments. Blank lines are ignored, so they are removed when the arguments are edited."_s);
    }

    if (!m_errors.isEmpty()) {
        m_problems->setMessageType(KMessageWidget::Error);
        m_problems->setText((m_errors + m_warnings).join(u'\n'));
        m_problems->show();
    } else if (!m_warnings.isEmpty()) {
        m_problems->setMessageType(KMessageWidget::Warning);
        m_problems->setText(m_warnings.join(u'\n'));
        m_problems->show();
    } else {
        m_problems->hide();
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_errors.isEmpty());
}

void BrowserEditDialog::updateIconPreview()
{
    m_iconPreview->setPixmap(browserIcon(m_icon->text().trimmed()).pixmap(QSize(32, 32), devicePixelRatioF()));
}

} // namespace klinker
