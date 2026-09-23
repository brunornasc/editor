#pragma once

#include "editor/CodeOutlineParser.hpp"
#include "project/Project.hpp"

#include <QAbstractItemModel>
#include <QHash>

namespace ui {

//! Modelo da arvore "Project": Projeto > Pastas > Arquivos > Escopos > Membros.
//!
//! Estrutura de pastas vem do Project; os simbolos (classes/funcoes) sao
//! parserados LAZY via CodeOutlineParser quando o usuario expande o arquivo
//! (cache em `m_outlineCache` evita re-parse).
class ProjectTreeModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    //! Roles customizados usados pelo view / MainWindow no double-click.
    enum Role {
        KindRole = Qt::UserRole + 1,  // int (Node::Kind)
        PathRole,                     // QString (arquivo absoluto)
        LineRole,                     // int (linha 0-based p/ salto)
    };

    struct Node {
        enum class Kind { Project, Folder, File, Scope, Member };
        Kind kind = Kind::Folder;
        QString name;
        QString absPath;   // arquivo dono (File/Scope/Member)
        QString tooltip;   // dica: caminho ou assinatura
        int line = -1;     // Scope/Member
        bool fileFetched = false;
        QVector<Node*> children;
        Node* parent = nullptr;

        ~Node() { qDeleteAll(children); }
    };

    explicit ProjectTreeModel(QObject* parent = nullptr);

    void setProject(const proj::Project& project);
    const proj::Project& project() const { return m_project; }

    Node* nodeFor(const QModelIndex& index) const;

    // ---- QAbstractItemModel ----
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex&) const override { return 1; }
    QVariant data(const QModelIndex& index, int role) const override;
    bool hasChildren(const QModelIndex& index) const override;
    bool canFetchMore(const QModelIndex& index) const override;
    void fetchMore(const QModelIndex& index) override;

private:
    void rebuildTree();
    void populateFile(Node* fileNode);

    proj::Project m_project;
    Node* m_root = nullptr;
    mutable QHash<QString, editor::OutlineData> m_outlineCache;
};

}  // namespace ui