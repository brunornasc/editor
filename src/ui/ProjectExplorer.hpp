#pragma once

#include <QDockWidget>

class QTreeView;

namespace proj {
struct Project;
}

namespace ui {

class ProjectTreeModel;

//! Dock "Project": arvore de arquivos + hierarquia de simbolos.
//!
//! Emite `symbolActivated(file, line)` no duplo-clique de classe/funcao,
//! e `fileActivated(file)` no duplo-clique de arquivo. MainWindow consome
//! e abre/salta o cursor no editor.
class ProjectExplorer final : public QDockWidget {
    Q_OBJECT

public:
    explicit ProjectExplorer(QWidget* parent = nullptr);

    //! Carrega/limpa a arvore a partir de um projeto.
    void setProject(const proj::Project& project);

    QTreeView* tree() const;

signals:
    void fileActivated(const QString& filePath);
    void symbolActivated(const QString& filePath, int line);
    //! Menu de contexto: criar/adicionar/modificar arquivos do projeto.
    void newFileInFolderRequested(const QString& directory);
    void addFilesToFolderRequested(const QString& directory);
    void removeFileRequested(const QString& filePath);
    void renameFileRequested(const QString& filePath);

protected:
    //! Monta o menu de contexto na posicao (coords do viewport) clicada.
    void showContextMenu(const QPoint& viewportPos);

private:
    QTreeView* m_tree = nullptr;
    ProjectTreeModel* m_model = nullptr;
};

}  // namespace ui