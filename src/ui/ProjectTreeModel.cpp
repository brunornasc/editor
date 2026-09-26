#include "ui/ProjectTreeModel.hpp"

#include <QApplication>
#include <QFile>
#include <QStyle>

namespace ui {

namespace {

QString memberLabel(const editor::OutlineMember& m)
{
    return m.isFunction ? m.name + QLatin1String(" ()") : m.name;
}

QString readFileSafely(const QString& path)
{
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f.readAll());
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------

ProjectTreeModel::ProjectTreeModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    m_root = new Node;
    m_root->kind = Node::Kind::Project;
    m_root->name = QStringLiteral("(vazio)");
}

void ProjectTreeModel::setProject(const proj::Project& project)
{
    beginResetModel();
    m_project = project;
    m_outlineCache.clear();
    rebuildTree();
    endResetModel();
}

ProjectTreeModel::Node* ProjectTreeModel::nodeFor(const QModelIndex& index) const
{
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root;
}

// ---------------------------------------------------------------------------
// Indexacao
// ---------------------------------------------------------------------------

QModelIndex ProjectTreeModel::index(int row, int column,
                                    const QModelIndex& parentIndex) const
{
    if (!hasIndex(row, column, parentIndex))
        return {};
    const Node* parentNode = nodeFor(parentIndex);
    if (row < 0 || row >= parentNode->children.size())
        return {};
    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex ProjectTreeModel::parent(const QModelIndex& child) const
{
    const Node* node = nodeFor(child);
    if (!node || !node->parent || node->parent == m_root)
        return {};
    return createIndex(node->parent->children.indexOf(const_cast<Node*>(node)),
                       0, node->parent);
}

int ProjectTreeModel::rowCount(const QModelIndex& parentIndex) const
{
    return nodeFor(parentIndex)->children.size();
}

QVariant ProjectTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};

    const Node* node = nodeFor(index);
    switch (role) {
        case Qt::DisplayRole:
            return node->name;
        case Qt::ToolTipRole:
            return node->tooltip;
        case Qt::DecorationRole: {
            auto* st = qApp->style();
            switch (node->kind) {
                case Node::Kind::Project:
                    return st->standardIcon(QStyle::SP_DirHomeIcon);
                case Node::Kind::Folder:
                    return st->standardIcon(QStyle::SP_DirIcon);
                case Node::Kind::File:
                    return st->standardIcon(QStyle::SP_FileIcon);
                default:
                    return {};
            }
        }
        case KindRole:
            return static_cast<int>(node->kind);
        case PathRole:
            return node->absPath;
        case LineRole:
            return node->line;
        default:
            return {};
    }
}

bool ProjectTreeModel::hasChildren(const QModelIndex& index) const
{
    const Node* node = nodeFor(index);
    if (node->kind == Node::Kind::File)
        return true;  // simbolos preenchidos sob demanda
    return !node->children.isEmpty();
}

// ---------------------------------------------------------------------------
// Fetch lazy (parse so ao expandir o arquivo)
// ---------------------------------------------------------------------------

bool ProjectTreeModel::canFetchMore(const QModelIndex& index) const
{
    const Node* node = nodeFor(index);
    return node->kind == Node::Kind::File && !node->fileFetched;
}

void ProjectTreeModel::fetchMore(const QModelIndex& index)
{
    Node* node = nodeFor(index);
    if (!node || node->kind != Node::Kind::File || node->fileFetched)
        return;

    editor::OutlineData outline;
    if (m_outlineCache.contains(node->absPath)) {
        outline = m_outlineCache.value(node->absPath);
    } else {
        outline = editor::CodeOutlineParser::parse(readFileSafely(node->absPath));
        m_outlineCache.insert(node->absPath, outline);  // cacheia ate os vazios
    }

    // Monta todos os nos-filhos em paralelo (usa begin/endInsertRows).
    QVector<Node*> insertList;

    // Nodos de escopos (classes/namespaces), indexados por posicao do outline.
    QVector<Node*> scopeNodes;
    scopeNodes.reserve(outline.scopes.size());
    for (const auto& s : outline.scopes) {
        auto* sn = new Node;
        sn->kind = Node::Kind::Scope;
        sn->name = s.name;
        sn->absPath = node->absPath;
        sn->line = s.line;
        sn->tooltip = s.fullPath;
        scopeNodes << sn;
    }

    // Membros globais (funcoes fora de classe/namespace).
    for (const auto& m : outline.globalMembers) {
        auto* mn = new Node;
        mn->kind = Node::Kind::Member;
        mn->name = memberLabel(m);
        mn->absPath = node->absPath;
        mn->line = m.line;
        mn->tooltip = m.signature;
        insertList << mn;
    }

    // Escopos na ordem do arquivo; aninhados vao sob o escopo pai.
    for (int i = 0; i < outline.scopes.size(); ++i) {
        Node* sn = scopeNodes.at(i);
        const int parentIdx = outline.scopes[i].parent;
        if (parentIdx >= 0 && parentIdx < scopeNodes.size())
            sn->parent = scopeNodes.at(parentIdx);
        else
            sn->parent = node;
    }

    // Membros de cada escopo anexam-se ao proprio no de escopo.
    for (int i = 0; i < outline.scopes.size(); ++i) {
        Node* sn = scopeNodes.at(i);
        for (const auto& m : outline.scopes[i].members) {
            auto* mn = new Node;
            mn->kind = Node::Kind::Member;
            mn->name = memberLabel(m);
            mn->absPath = node->absPath;
            mn->line = m.line;
            mn->tooltip = m.signature;
            mn->parent = sn;
            sn->children << mn;
        }
    }

    // Insere filhos de FILE node (escopos top-level + membros globais).
    for (Node* sn : scopeNodes)
        if (sn->parent == node)
            insertList << sn;

    node->fileFetched = true;

    const int first = node->children.size();
    const QModelIndex self = node->parent
        ? createIndex(node->parent->children.indexOf(node), 0, node)
        : QModelIndex();
    beginInsertRows(self, first, first + insertList.size() - 1);
    node->children << insertList;
    endInsertRows();
}

// ---------------------------------------------------------------------------
// Construcao da arvore de pastas/arquivos
// ---------------------------------------------------------------------------

void ProjectTreeModel::rebuildTree()
{
    // `m_root` e a raiz INVISIVEL do model (nunca aparece na view); o no de
    // projeto e seu primeiro filho, para que o nome do projeto seja a linha de
    // topo exibida. Sem projeto -> arvore vazia.
    m_root = new Node;
    m_root->kind = Node::Kind::Project;
    m_root->name = QStringLiteral("(vazio)");

    if (m_project.displayName().isEmpty())
        return;

    auto* project = new Node;
    project->kind = Node::Kind::Project;
    project->name = m_project.displayName();
    project->absPath = m_project.rootDir;
    project->tooltip = m_project.rootDir;
    project->parent = m_root;
    m_root->children << project;

    for (const QString& rel : m_project.files) {
        Node* cursor = project;
        const QStringList parts = rel.split(QLatin1Char('/'));

        // Cria/segue pastas intermediarias.
        QString folderAbs = m_project.rootDir;
        for (int i = 0; i < parts.size() - 1; ++i) {
            folderAbs += QLatin1Char('/') + parts[i];
            Node* next = nullptr;
            for (Node* c : cursor->children) {
                if (c->kind == Node::Kind::Folder && c->name == parts[i]) {
                    next = c;
                    break;
                }
            }
            if (!next) {
                next = new Node;
                next->kind = Node::Kind::Folder;
                next->name = parts[i];
                next->absPath = folderAbs;
                next->parent = cursor;
                cursor->children << next;
            }
            cursor = next;
        }

        auto* file = new Node;
        file->kind = Node::Kind::File;
        file->name = parts.last();
        file->absPath = m_project.rootDir + QLatin1Char('/') + rel;
        file->tooltip = file->absPath;
        file->parent = cursor;
        cursor->children << file;
    }
}

}  // namespace ui