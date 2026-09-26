#pragma once

#include <QMainWindow>

#include "project/Project.hpp"

class QMenu;
class QAction;
class QLabel;
class QComboBox;

namespace ui {

class EditorTabWidget;
class ProjectExplorer;
class OutputWindow;
class CodeEditorWidget;
class DebugDock;

}  // namespace ui

namespace build {
class BuildManager;
}  // namespace build

namespace debug {
class DebugController;
}  // namespace debug

namespace ui {

//! Janela principal da IDE. Responsavel apenas por orquestrar o layout
//! (menus, toolbars, docks e central de documentos) -- nenhuma logica de
//! edicao/build/debug vive aqui; as Parts seguintes anexam-se via slots
//! publicos ou by-connection.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    //! Acesso aos docks para as Parts 3/4/5 (wiring sem acoplamento).
    ProjectExplorer* projectExplorer() const;
    OutputWindow* outputWindow() const;
    EditorTabWidget* editorTabs() const;

    //! Abre arquivos passados como argumentos de linha de comando.
    void openFilesFromCommandLine(const QStringList& paths);

    //! Carrega um arquivo de projeto (.json/.vproj). Retorna se teve sucesso.
    bool openProjectFile(const QString& path);

private slots:
    void onNewFile();
    void onOpenFile();
    void onSave();
    void onAbout();
    void onCurrentFileChanged(const QString& filePath);

    void onNewProject();
    void onOpenProject();
    void onCloseProject();
    void onSymbolActivated(const QString& filePath, int line);
    void onFileActivated(const QString& filePath);

    void onNewFileInFolder(const QString& directory);
    void onAddFilesToFolder(const QString& directory);
    void onRemoveFile(const QString& path);
    void onRenameFile(const QString& path);

    // Parte 4: build / run / conan
    void onBuild();
    void onRebuild();
    void onClean();
    void onRun();
    void onConanInstall();
    void onConanOpenFile();
    void onErrorActivated(const QString& filePath, int line);
    void onBuildFinished(bool ok, int errors, int warnings);

    // Parte 5: debug
    void onDebugStartContinue();
    void onDebugStop();
    void onDebugStepOver();
    void onDebugStepInto();
    void onDebugStepOut();
    void onToggleBreakpoint();
    void onDebugSessionChanged(bool active, bool running);
    void onAddWatch();
    void onProjectSettings();

private:
    //! Empurra compilador/flags/includeDirs/cache do projeto para o HeaderIndex
    //! (intellisense de C++). Idempotente: pode chamar a cada abertura/ajuste.
    void configureIntellisense();

    //! Grava o arquivo da aba atual (Save As para documentos novos).
    void saveCurrentDocument();
    void saveAllDocuments();
    //! Se projeto aberto e o arquivo estiver em rootDir, anexa e persiste.
    void addFileToProjectIfUnderRoot(const QString& absPath);
    void persistProjectJson();
    void promptSaveDocumentAt(CodeEditorWidget* editor, int index);
    //! Config ativa (Debug/Release) da combo da toolbar.
    QString currentConfigName() const;

    void createActions();
    void createMenus();
    void createToolBars();
    void createDocks();
    void createStatusBar();
    void updateWindowTitle();

    // Menus
    QMenu* m_fileMenu = nullptr;
    QMenu* m_projectMenu = nullptr;
    QMenu* m_editMenu = nullptr;
    QMenu* m_buildMenu = nullptr;
    QMenu* m_debugMenu = nullptr;
    QMenu* m_viewMenu = nullptr;
    QMenu* m_helpMenu = nullptr;

    // Acoes
    QAction* m_newAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAllAction = nullptr;
    QAction* m_exitAction = nullptr;
    QAction* m_newProjectAction = nullptr;
    QAction* m_openProjectAction = nullptr;
    QAction* m_closeProjectAction = nullptr;
    QAction* m_buildAction = nullptr;
    QAction* m_rebuildAction = nullptr;
    QAction* m_cleanAction = nullptr;
    QAction* m_runAction = nullptr;
    QAction* m_conanInstallAction = nullptr;
    QAction* m_conanEditAction = nullptr;

    // Parte 5: debug
    QAction* m_debugStartAction = nullptr;
    QAction* m_debugStopAction = nullptr;
    QAction* m_debugStepOverAction = nullptr;
    QAction* m_debugStepIntoAction = nullptr;
    QAction* m_debugStepOutAction = nullptr;
    QAction* m_toggleBreakpointAction = nullptr;
    QAction* m_addWatchAction = nullptr;
    QAction* m_projectSettingsAction = nullptr;
    QAction* m_aboutAction = nullptr;

    // Toolbars / status
    QLabel* m_statusMode = nullptr;
    QComboBox* m_configCombo = nullptr;

    // Widgets centrais / docks
    EditorTabWidget* m_editorTabs = nullptr;
    ProjectExplorer* m_projectExplorer = nullptr;
    OutputWindow* m_outputWindow = nullptr;
    DebugDock* m_debugDock = nullptr;
    build::BuildManager* m_buildManager = nullptr;
    debug::DebugController* m_debugController = nullptr;

    QString m_currentFile;
    QString m_projectJsonPath;
    QString m_configName = QStringLiteral("Debug");
    proj::Project m_project;
    bool m_debugAutoStart = false;  // F5 pediu build; dispara apos ok
};

}  // namespace ui