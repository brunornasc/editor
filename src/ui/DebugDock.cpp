#include "ui/DebugDock.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QDebug>
#include <QFileInfo>

namespace ui {

namespace {

int frameLine(const QListWidgetItem* item)
{
    return item->data(Qt::UserRole + 1).toInt();
}

QString frameFile(const QListWidgetItem* item)
{
    return item->data(Qt::UserRole).toString();
}

}  // namespace

DebugDock::DebugDock(QWidget* parent)
    : QDockWidget(tr("Depuracao"), parent)
{
    setObjectName(QStringLiteral("debugDock"));
    setFeatures(DockWidgetMovable | DockWidgetFloatable | DockWidgetClosable);

    m_stack = new QListWidget(this);
    m_stack->setUniformItemSizes(true);

    m_locals = new QTreeWidget(this);
    m_locals->setColumnCount(3);
    m_locals->setHeaderLabels(QStringList()
                              << tr("Nome") << tr("Valor") << tr("Tipo"));
    m_locals->header()->setStretchLastSection(false);
    m_locals->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_locals->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_locals->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_locals->setContextMenuPolicy(Qt::CustomContextMenu);

    // Watch (Inspecao): nome + valor (tipo vem do tooltip/avaliacao).
    m_watchTable = new QTableWidget(this);
    m_watchTable->setColumnCount(2);
    m_watchTable->setHorizontalHeaderLabels(
        QStringList() << tr("Expressao") << tr("Valor"));
    m_watchTable->horizontalHeader()->setStretchLastSection(false);
    m_watchTable->horizontalHeader()->setSectionResizeMode(0,
                                                           QHeaderView::ResizeToContents);
    m_watchTable->horizontalHeader()->setSectionResizeMode(1,
                                                           QHeaderView::Stretch);
    m_watchTable->verticalHeader()->setVisible(false);
    m_watchTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Breakpoints: [ ] | Arquivo:Linha
    m_bpTable = new QTableWidget(this);
    m_bpTable->setColumnCount(1);
    m_bpTable->setHorizontalHeaderLabels(QStringList() << tr("Breakpoint"));
    m_bpTable->horizontalHeader()->setStretchLastSection(true);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_stack, tr("Pilha de Chamadas"));
    m_tabs->addTab(m_locals, tr("Locais"));
    m_tabs->addTab(m_watchTable, tr("Inspecao"));
    m_tabs->addTab(m_bpTable, tr("Breakpoints"));
    setWidget(m_tabs);

    // Duplo-clique/Enter num quadro: navega ate o arquivo/linha.
    connect(m_stack, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const QString file = frameFile(item);
                const int line = frameLine(item);
                if (!file.isEmpty() && line >= 0)
                    emit frameActivated(file, line);
            });

    // Locais -> menu de contexto "Adicionar a Inspecao".
    connect(m_locals, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QTreeWidgetItem* item = m_locals->itemAt(pos);
                if (!item)
                    return;
                QMenu menu(this);
                QAction* act = menu.addAction(tr("Adicionar a Inspecao"));
                if (menu.exec(m_locals->viewport()->mapToGlobal(pos)) == act) {
                    if (!item->text(0).isEmpty())
                        emit addToWatchRequested(item->text(0));
                }
            });

    // Breakpoints: checkbox ativa/desativa; duplo-clique navega.
    connect(m_bpTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem* item) {
                if (item->column() != 0)
                    return;
                const QString file =
                    m_bpTable->item(item->row(), 0)->data(Qt::UserRole).toString();
                const int line =
                    m_bpTable->item(item->row(), 0)->data(Qt::UserRole + 1).toInt();
                emit breakpointToggled(file, line,
                                       item->checkState() == Qt::Checked);
            });
    connect(m_bpTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem* item) {
                const QString file = item->data(Qt::UserRole).toString();
                const int line = item->data(Qt::UserRole + 1).toInt();
                if (!file.isEmpty() && line >= 0)
                    emit frameActivated(file, line);
            });
    connect(m_bpTable, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                QAction* act = menu.addAction(tr("Remover Breakpoint"));
                if (menu.exec(m_bpTable->viewport()->mapToGlobal(pos)) != act)
                    return;
                const int row = m_bpTable->currentRow();
                if (row < 0)
                    return;
                QTableWidgetItem* it = m_bpTable->item(row, 0);
                emit breakpointRemoved(it->data(Qt::UserRole).toString(),
                                       it->data(Qt::UserRole + 1).toInt());
            });

    // Inspecao: menu de contexto remove a expressao selecionada.
    connect(m_watchTable, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                QAction* act = menu.addAction(tr("Remover Inspecao"));
                if (menu.exec(m_watchTable->viewport()->mapToGlobal(pos)) != act)
                    return;
                const int row = m_watchTable->currentRow();
                if (row < 0)
                    return;
                QTableWidgetItem* it = m_watchTable->item(row, 0);
                if (it)
                    emit watchRemoved(it->text());
            });
}

void DebugDock::setCallStack(const QVector<debug::MiFrame>& frames)
{
    m_stack->clear();
    for (const auto& f : frames) {
        QString loc = f.file;
        if (f.fullname.isEmpty() && f.file.isEmpty())
            loc = QStringLiteral("?");
        else if (!f.fullname.isEmpty())
            loc = f.fullname;
        if (f.line >= 0)
            loc += QStringLiteral(":%1").arg(f.line);

        const QString text = QStringLiteral("%1 (%2)")
            .arg(f.func.isEmpty() ? QStringLiteral("?") : f.func)
            .arg(loc);

        auto* item = new QListWidgetItem(text, m_stack);
        item->setData(Qt::UserRole, f.fullname.isEmpty() ? f.file : f.fullname);
        item->setData(Qt::UserRole + 1, f.line);
        item->setToolTip(QStringLiteral("%1\n%2:%3")
                             .arg(f.func)
                             .arg(f.fullname.isEmpty() ? f.file : f.fullname)
                             .arg(f.line));
    }
    if (m_stack->count() > 0)
        m_stack->setCurrentRow(0);
}

void DebugDock::setLocals(const QString& func,
                          const QVector<debug::MiVar>& vars)
{
    m_locals->clear();
    auto* group = new QTreeWidgetItem(m_locals);
    group->setText(0, func.isEmpty() ? tr("(quadro desconhecido)") : func);
    group->setExpanded(true);

    for (const auto& v : vars) {
        auto* item = new QTreeWidgetItem(group);
        item->setText(0, v.name);
        item->setText(1, v.value);
        item->setText(2, v.type);
        if (v.value.contains(QLatin1Char('"')) || v.value.contains(QLatin1Char('\n')))
            item->setToolTip(1, v.value);
    }
}

void DebugDock::clearAll()
{
    m_stack->clear();
    m_locals->clear();
}

void DebugDock::setBreakpointsList(const QVector<BreakpointEntry>& entries)
{
    const QSignalBlocker blocker(m_bpTable);
    m_bpTable->setRowCount(entries.size());
    for (int r = 0; r < entries.size(); ++r) {
        const auto& e = entries[r];
        auto* item = new QTableWidgetItem(e.enabled ? QStringLiteral("[x] %1")
                                                          .arg(e.label)
                                                    : QStringLiteral("[ ] %1")
                                                          .arg(e.label));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
                       | Qt::ItemIsUserCheckable);
        item->setCheckState(e.enabled ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, e.file);
        item->setData(Qt::UserRole + 1, e.line);
        item->setToolTip(QStringLiteral("%1\n%2")
                             .arg(e.label, e.enabled ? QString()
                                                     : tr("desativado")));
        m_bpTable->setItem(r, 0, item);
    }
}

void DebugDock::setWatchExpressions(const QStringList& exprs)
{
    const QSignalBlocker blocker(m_watchTable);
    m_watchTable->setRowCount(exprs.size());
    for (int r = 0; r < exprs.size(); ++r) {
        auto* name = new QTableWidgetItem(exprs.at(r));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        auto* value = new QTableWidgetItem(m_watchValues.value(exprs.at(r)));
        value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_watchTable->setItem(r, 0, name);
        m_watchTable->setItem(r, 1, value);
    }
}

void DebugDock::setWatchValue(const QString& expr, const QString& value)
{
    m_watchValues.insert(expr, value);
    for (int r = 0; r < m_watchTable->rowCount(); ++r) {
        QTableWidgetItem* name = m_watchTable->item(r, 0);
        if (name && name->text() == expr) {
            m_watchTable->setItem(r, 1, new QTableWidgetItem(value));
            return;
        }
    }
    // Entrada nova (watch adicionada antes do rebuild): cria uma linha.
    const int r = m_watchTable->rowCount();
    m_watchTable->setRowCount(r + 1);
    auto* nameItem = new QTableWidgetItem(expr);
    nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    auto* valueItem = new QTableWidgetItem(value);
    valueItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_watchTable->setItem(r, 0, nameItem);
    m_watchTable->setItem(r, 1, valueItem);
}

void DebugDock::clearWatchValues()
{
    const QSignalBlocker blocker(m_watchTable);
    m_watchValues.clear();
    for (int r = 0; r < m_watchTable->rowCount(); ++r)
        m_watchTable->setItem(r, 1, new QTableWidgetItem);
}

}  // namespace ui