#include "ui/OutputWindow.hpp"

#include <QHeaderView>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStyle>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>

#include <QDebug>

namespace ui {

OutputWindow::OutputWindow(QWidget* parent)
    : QDockWidget(QObject::tr("Output"), parent)
{
    setObjectName("OutputWindow");
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new QTabWidget(container);
    m_tabs->setDocumentMode(false);

    auto makeConsole = [this](const QString& title) {
        auto* console = new QPlainTextEdit(m_tabs);
        console->setReadOnly(true);
        console->setLineWrapMode(QPlainTextEdit::NoWrap);
        console->setUndoRedoEnabled(false);
        m_tabs->addTab(console, title);
        return console;
    };

    m_console[(int)Pane::BuildOutput] = makeConsole(QObject::tr("Build Output"));

    // Error List (VS2005): Arq  Linha  Col  Erro/Aviso  Msg
    m_errors = new QTreeWidget(m_tabs);
    m_errors->setHeaderLabels(
        {QObject::tr("Tipo"), QObject::tr("Arquivo"), QObject::tr("Linha"),
         QObject::tr("Col."), QObject::tr("Mensagem")});
    m_errors->setRootIsDecorated(false);
    m_errors->setAlternatingRowColors(true);
    m_errors->setUniformRowHeights(true);
    m_errors->setColumnWidth(0, 90);
    m_errors->setColumnWidth(1, 280);
    m_errors->setColumnWidth(2, 55);
    m_errors->setColumnWidth(3, 45);
    m_errors->header()->setStretchLastSection(true);
    m_tabs->insertTab((int)Pane::ErrorList, m_errors, QObject::tr("Error List"));

    m_console[(int)Pane::DebugConsole] = makeConsole(QObject::tr("Debug Console"));
    m_console[(int)Pane::ConanLogs] = makeConsole(QObject::tr("Conan Logs"));

    layout->addWidget(m_tabs);
    setWidget(container);

    // Navegacao: duplo-clique ou Enter numa linha de erro abre o arquivo.
    auto jump = [this](QTreeWidgetItem* item) {
        if (!item)
            return;
        emit errorActivated(item->data(1, Qt::UserRole).toString(),
                            item->data(2, Qt::UserRole).toInt());
    };
    connect(m_errors, &QTreeWidget::itemActivated, this, jump);
    connect(m_errors, &QTreeWidget::itemDoubleClicked, this, jump);
}

void OutputWindow::activatePane(Pane pane)
{
    m_tabs->setCurrentIndex((int)pane);
    if (pane == Pane::ErrorList) {
        m_errors->setFocus();
    } else if (auto* out = m_console[(int)pane]) {
        out->setFocus();
    }
}

QPlainTextEdit* OutputWindow::console(Pane pane) const
{
    return m_console[(int)pane];
}

QTreeWidget* OutputWindow::errorList() const
{
    return m_errors;
}

void OutputWindow::append(Pane pane, const QString& text)
{
    auto* out = m_console[(int)pane];
    if (!out || text.isEmpty())
        return;
    out->moveCursor(QTextCursor::End);
    out->insertPlainText(text);
    out->moveCursor(QTextCursor::End);
    QScrollBar* sb = out->verticalScrollBar();
    if (sb)
        sb->setValue(sb->maximum());
}

void OutputWindow::appendHtml(Pane pane, const QString& html)
{
    auto* out = m_console[(int)pane];
    if (!out || html.isEmpty())
        return;
    out->moveCursor(QTextCursor::End);
    out->appendHtml(html);
    out->moveCursor(QTextCursor::End);
    QScrollBar* sb = out->verticalScrollBar();
    if (sb)
        sb->setValue(sb->maximum());
}

void OutputWindow::clear(Pane pane)
{
    auto* out = m_console[(int)pane];
    if (out)
        out->clear();
}

void OutputWindow::setErrors(const QVector<ErrorEntry>& entries)
{
    m_errors->clear();
    QStyle* st = style();
    for (const auto& e : entries) {
        auto* item = new QTreeWidgetItem(m_errors);
        item->setText(0, e.isError ? QObject::tr("Erro") : QObject::tr("Aviso"));
        item->setIcon(0, e.isError
                      ? st->standardIcon(QStyle::SP_MessageBoxCritical)
                      : st->standardIcon(QStyle::SP_MessageBoxWarning));
        item->setText(1, e.file);
        item->setText(2, QString::number(e.line));
        item->setText(3, QString::number(e.column));
        item->setText(4, e.message);
        item->setData(1, Qt::UserRole, e.file);
        item->setData(2, Qt::UserRole, e.line);
        item->setToolTip(4, e.message);
    }
}

}  // namespace ui