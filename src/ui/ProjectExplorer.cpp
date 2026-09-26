#include "ui/ProjectExplorer.hpp"

#include "project/Project.hpp"
#include "ui/ProjectTreeModel.hpp"

#include <QHeaderView>
#include <QMenu>
#include <QTreeView>

namespace ui {

ProjectExplorer::ProjectExplorer(QWidget* parent)
    : QDockWidget(QObject::tr("Project"), parent)
{
    setObjectName("ProjectExplorer");
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_tree = new QTreeView(this);
    m_tree->setHeaderHidden(false);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);

    m_model = new ProjectTreeModel(this);
    m_tree->setModel(m_model);

    // Menu de contexto com coordenadas CORRETAS do viewport.
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeView::customContextMenuRequested,
            this, &ProjectExplorer::showContextMenu);

    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        const auto* node = m_model->nodeFor(idx);
        if (!node)
            return;
        switch (node->kind) {
            case ProjectTreeModel::Node::Kind::Member:
            case ProjectTreeModel::Node::Kind::Scope:
                emit symbolActivated(node->absPath, node->line);
                break;
            case ProjectTreeModel::Node::Kind::File:
                emit fileActivated(node->absPath);
                break;
            default:
                break;
        }
    });

    setWidget(m_tree);
}

void ProjectExplorer::setProject(const proj::Project& project)
{
    m_model->setProject(project);
    expandFolders(QModelIndex());
}

void ProjectExplorer::expandFolders(const QModelIndex& parent)
{
    const int rows = m_model->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex idx = m_model->index(row, 0, parent);
        const auto* node = m_model->nodeFor(idx);
        if (!node)
            continue;
        // Só projeto e pastas: expandir um arquivo dispararia o parse lazy
        // dos simbolos de todo o projeto.
        if (node->kind != ProjectTreeModel::Node::Kind::Project
            && node->kind != ProjectTreeModel::Node::Kind::Folder) {
            continue;
        }
        m_tree->expand(idx);
        expandFolders(idx);
    }
}

QTreeView* ProjectExplorer::tree() const
{
    return m_tree;
}

// ---------------------------------------------------------------------------
// Menu de contexto (gerenciamento de arquivos do projeto)
// ---------------------------------------------------------------------------

void ProjectExplorer::showContextMenu(const QPoint& viewportPos)
{
    const QModelIndex idx = m_tree->indexAt(viewportPos);
    const auto* node = m_model->nodeFor(idx);
    if (!node)
        return;

    m_tree->setCurrentIndex(idx);

    QMenu menu(this);

    switch (node->kind) {
        case ProjectTreeModel::Node::Kind::Project:
        case ProjectTreeModel::Node::Kind::Folder: {
            const QString dir = node->absPath;
            menu.addAction(QObject::tr("Novo arquivo aqui..."),
                           this, [this, dir] { emit newFileInFolderRequested(dir); });
            menu.addAction(QObject::tr("Adicionar arquivos existentes..."),
                           this, [this, dir] { emit addFilesToFolderRequested(dir); });
            break;
        }
        case ProjectTreeModel::Node::Kind::File: {
            const QString path = node->absPath;
            menu.addAction(QObject::tr("Abrir"),
                           this, [this, path] { emit fileActivated(path); });
            menu.addSeparator();
            menu.addAction(QObject::tr("Renomear..."),
                           this, [this, path] { emit renameFileRequested(path); });
            menu.addAction(QObject::tr("Remover do projeto"),
                           this, [this, path] { emit removeFileRequested(path); });
            break;
        }
        case ProjectTreeModel::Node::Kind::Scope:
        case ProjectTreeModel::Node::Kind::Member: {
            const QString path = node->absPath;
            const int line = node->line;
            menu.addAction(QObject::tr("Ir para a definicao"),
                           this, [this, path, line] { emit symbolActivated(path, line); });
            break;
        }
    }

    menu.exec(m_tree->viewport()->mapToGlobal(viewportPos));
}

}  // namespace ui