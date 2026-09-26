#include "ui/MainWindow.hpp"

#include "build/BuildManager.hpp"
#include "build/ConanManager.hpp"
#include "debug/DebugController.hpp"
#include "project/Project.hpp"
#include "ui/CodeEditorWidget.hpp"
#include "editor/HeaderIndex.hpp"
#include "ui/DebugDock.hpp"
#include "ui/EditorTabWidget.hpp"
#include "ui/OutputWindow.hpp"
#include "ui/ProjectExplorer.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>

#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QDebug>

namespace ui {

namespace {

//! Icone de "depurar": uma baratinha (estilo VS "start debugging").
QIcon debugStartIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor shell(0x36, 0x36, 0x3a);
    // patas
    p.setPen(QPen(shell, 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(5, 7), QPointF(2, 4));
    p.drawLine(QPointF(5, 10), QPointF(2, 10));
    p.drawLine(QPointF(5, 13), QPointF(2, 16));
    p.drawLine(QPointF(17, 7), QPointF(20, 4));
    p.drawLine(QPointF(17, 10), QPointF(20, 10));
    p.drawLine(QPointF(17, 13), QPointF(20, 16));
    // abdomen
    p.setBrush(shell);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(6, 6, 10, 11));
    // cabeca
    p.drawEllipse(QRectF(7, 3.5, 8, 4.5));
    // olhos (risca vertical para parecer foco)
    p.setPen(QPen(Qt::white, 1.3));
    p.drawLine(QPointF(10, 4.5), QPointF(10, 6.5));
    p.drawLine(QPointF(12, 4.5), QPointF(12, 6.5));
    // antenas
    p.setPen(QPen(shell, 1.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(9, 4), QPointF(5, 1));
    p.drawLine(QPointF(13, 4), QPointF(17, 1));
    return QIcon(pm);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setMinimumSize(1024, 640);
    resize(1280, 800);

    createActions();
    createMenus();
    createToolBars();
    createDocks();
    createStatusBar();

    // Parte 4: build/run/Conan precisam dos docks ja existentes.
    m_buildManager = new build::BuildManager(m_outputWindow, m_editorTabs, this);
    connect(m_buildAction, &QAction::triggered, this, &MainWindow::onBuild);
    connect(m_rebuildAction, &QAction::triggered, this, &MainWindow::onRebuild);
    connect(m_cleanAction, &QAction::triggered, this, &MainWindow::onClean);
    connect(m_runAction, &QAction::triggered, this, &MainWindow::onRun);
    connect(m_conanInstallAction, &QAction::triggered,
            this, &MainWindow::onConanInstall);
    connect(m_conanEditAction, &QAction::triggered,
            this, &MainWindow::onConanOpenFile);
    connect(m_buildManager, &build::BuildManager::buildFinished,
            this, &MainWindow::onBuildFinished);

    // Parte 5: depurador GDB/MI (precisa dos docks prontos).
    m_debugController = new debug::DebugController(
        m_editorTabs, m_outputWindow, m_debugDock, statusBar(), this);
    connect(m_debugStartAction, &QAction::triggered,
            this, &MainWindow::onDebugStartContinue);
    connect(m_debugStopAction, &QAction::triggered,
            this, &MainWindow::onDebugStop);
    connect(m_debugStepOverAction, &QAction::triggered,
            this, &MainWindow::onDebugStepOver);
    connect(m_debugStepIntoAction, &QAction::triggered,
            this, &MainWindow::onDebugStepInto);
    connect(m_debugStepOutAction, &QAction::triggered,
            this, &MainWindow::onDebugStepOut);
    connect(m_toggleBreakpointAction, &QAction::triggered,
            this, &MainWindow::onToggleBreakpoint);
    connect(m_addWatchAction, &QAction::triggered,
            this, &MainWindow::onAddWatch);
    connect(m_projectSettingsAction, &QAction::triggered,
            this, &MainWindow::onProjectSettings);
    connect(m_debugController, &debug::DebugController::stateChanged,
            this, &MainWindow::onDebugSessionChanged);
    // F5 sem executavel: compila e, ao concluir, inicia a depuracao.
    connect(m_debugController, &debug::DebugController::buildRequested, this,
            [this] {
                if (!m_project.isValid() || m_buildManager->isBusy())
                    return;
                m_debugAutoStart = true;
                saveAllDocuments();
                m_buildManager->build(m_project, currentConfigName());
            });
    // Traz o console de debug para frente sempre que a sessao fala.
    connect(m_debugController, &debug::DebugController::debugConsoleWanted,
            this, [this] { m_outputWindow->activatePane(
                      ui::OutputWindow::Pane::DebugConsole); });
    onDebugSessionChanged(false, false);

    m_editorTabs->newDocument(QObject::tr("Inicio"));
    updateWindowTitle();
}

ProjectExplorer* MainWindow::projectExplorer() const
{
    return m_projectExplorer;
}

OutputWindow* MainWindow::outputWindow() const
{
    return m_outputWindow;
}

EditorTabWidget* MainWindow::editorTabs() const
{
    return m_editorTabs;
}

void MainWindow::openFilesFromCommandLine(const QStringList& paths)
{
    for (const QString& p : paths)
        m_editorTabs->openFile(QFileInfo(p).absoluteFilePath());
    if (!paths.isEmpty())
        updateWindowTitle();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::onNewFile()
{
    const QString title = QObject::tr("Untitled-%1").arg(m_editorTabs->count() + 1);
    m_editorTabs->newDocument(title);
}

void MainWindow::onOpenFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QObject::tr("Abrir arquivo"), QString(),
        QObject::tr("C/C++ (*.cpp *.hpp *.h *.cc *.cxx *.c);;"
                    "Arquivos Conan (*.txt *.py);;"
                    "Todos (*)"));
    if (path.isEmpty())
        return;
    if (m_editorTabs->openFile(path) >= 0)
        statusBar()->showMessage(QObject::tr("Aberto: %1").arg(path), 5000);
}

void MainWindow::onSave()
{
    saveCurrentDocument();
}

void MainWindow::saveCurrentDocument()
{
    const int idx = m_editorTabs->currentIndex();
    auto* ed = m_editorTabs->editorAt(idx);
    if (!ed)
        return;

    if (ed->filePath().isEmpty()) {
        promptSaveDocumentAt(ed, idx);
    } else {
        if (m_editorTabs->saveDocument(idx))
            statusBar()->showMessage(QObject::tr("Salvo: %1")
                                         .arg(ed->filePath()),
                                     4000);
        else
            QMessageBox::warning(this, QObject::tr("Salvar"),
                                 QObject::tr("Falha ao salvar:\n%1")
                                     .arg(ed->filePath()));
    }
}

void MainWindow::saveAllDocuments()
{
    for (int i = 0; i < m_editorTabs->count(); ++i) {
        auto* ed = m_editorTabs->editorAt(i);
        if (!ed || !ed->isModified())
            continue;
        if (ed->filePath().isEmpty())
            promptSaveDocumentAt(ed, i);
        else
            m_editorTabs->saveDocument(i);
    }
    statusBar()->showMessage(QObject::tr("Todos os documentos salvos"), 4000);
}

//! "Save As": escolhe destino, grava e registra no projeto (se aplicavel).
void MainWindow::promptSaveDocumentAt(CodeEditorWidget* editor, int index)
{
    QString defaultName = QStringLiteral("arquivo.cpp");
    if (m_project.isValid())
        defaultName = m_project.rootDir +
            QLatin1Char('/') + QStringLiteral("src/arquivo.cpp");

    const QString path = QFileDialog::getSaveFileName(
        this, QObject::tr("Salvar Como"), defaultName,
        QObject::tr("C/C++ (*.cpp *.hpp *.h *.cc *.cxx *.c);;Todos (*)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QObject::tr("Salvar Como"),
                             QObject::tr("Falha ao criar:\n%1").arg(path));
        return;
    }
    f.write(editor->editor()->text().toUtf8());
    f.close();

    editor->setFilePath(path);
    m_editorTabs->retitleEditor(editor);
    m_editorTabs->saveDocument(index);  // atualiza dirty/foco

    // Registra automaticamente no projeto quando estiver em rootDir.
    addFileToProjectIfUnderRoot(path);
    statusBar()->showMessage(QObject::tr("Salvo: %1").arg(path), 4000);
}

void MainWindow::onAbout()
{
    QMessageBox::about(
        this, QObject::tr("Sobre a IDE"),
        QObject::tr("<h3>Editor — C++ / Qt6</h3>"
                    "Ambiente de desenvolvimento extensivel: editor QScintilla, "
                    "build por Makefiles + Conan e debug GDB/MI.<br><br>"
                    "Parte 4: build (Makefile + Conan) e navegacao de erros.<br>"
                    "Parte 5: depurador GDB/MI (F5, F9 breakpoints, step, "
                    "pilha de chamadas e locais)."));
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

void MainWindow::onNewProject()
{
    const QString baseDir = QFileDialog::getExistingDirectory(
        this, QObject::tr("Pasta do novo projeto"), QDir::homePath());
    if (baseDir.isEmpty())
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QObject::tr("Novo projeto"), QObject::tr("Nome do projeto:"),
        QLineEdit::Normal, QStringLiteral("MeuProjeto"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString cleanName = name.trimmed();

    QDir dir(baseDir);
    dir.mkpath(QStringLiteral("src"));

    // Template minimo de um main.cpp para o novo projeto.
    const QString mainPath = dir.filePath(QStringLiteral("src/main.cpp"));
    QFile main(mainPath);
    if (!main.exists()) {
        if (main.open(QIODevice::WriteOnly | QIODevice::Text)) {
            main.write(QStringLiteral(
                           "#include <iostream>\n\n"
                           "// Projeto \"%1\" — IDE Editor\n\n"
                           "int main()\n{\n"
                           "    std::cout << \"Hello, %1!\\n\";\n"
                           "    return 0;\n"
                           "}\n")
                           .arg(cleanName)
                           .toUtf8());
        }
    }

    m_project = proj::Project::create(cleanName, baseDir,
                                      {QStringLiteral("src/main.cpp")});
    m_projectJsonPath = dir.filePath(cleanName + QStringLiteral(".json"));
    m_project.save(m_projectJsonPath);
    configureIntellisense();
    m_projectExplorer->setProject(m_project);
    m_debugController->setProject(m_project);
    m_editorTabs->openFile(mainPath);
    statusBar()->showMessage(QObject::tr("Projeto criado: %1").arg(cleanName),
                             5000);
}

void MainWindow::onOpenProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QObject::tr("Abrir projeto"), QString(),
        QObject::tr("Arquivos de projeto (*.json *.vproj);;Todos (*)"));
    if (path.isEmpty())
        return;
    openProjectFile(path);
}

bool MainWindow::openProjectFile(const QString& path)
{
    proj::Project p;
    if (!p.load(path)) {
        QMessageBox::warning(this, QObject::tr("Projeto"),
                             QObject::tr("Falha ao carregar o projeto:\n%1")
                                 .arg(path));
        return false;
    }
    m_project = p;
    m_projectJsonPath = path;
    configureIntellisense();
    m_projectExplorer->setProject(m_project);
    m_debugController->setProject(m_project);
    // Restaura os breakpoints persistidos no arquivo de projeto (Parte 5).
    m_debugController->setStoredBreakpoints(m_project.breakpoints);
    statusBar()->showMessage(QObject::tr("Projeto aberto: %1")
                                 .arg(m_project.displayName()),
                             5000);
    return true;
}

void MainWindow::onCloseProject()
{
    m_debugAutoStart = false;
    m_project = {};
    m_projectJsonPath.clear();
    configureIntellisense();
    m_projectExplorer->setProject(m_project);
    if (m_debugController)
        m_debugController->setProject(m_project);
    statusBar()->showMessage(QObject::tr("Projeto fechado"), 3000);
}

void MainWindow::onSymbolActivated(const QString& filePath, int line)
{
    const int idx = m_editorTabs->openFile(filePath);
    if (idx >= 0 && line >= 0) {
        auto* ed = m_editorTabs->editorAt(idx);
        if (ed)
            ed->gotoLine(line);
    }
}

void MainWindow::onFileActivated(const QString& filePath)
{
    m_editorTabs->openFile(filePath);
}

// ---------------------------------------------------------------------------
// Gerenciamento de arquivos do projeto (menu de contexto na arvore)
// ---------------------------------------------------------------------------

void MainWindow::onNewFileInFolder(const QString& directory)
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QObject::tr("Novo arquivo"),
        QObject::tr("Nome do arquivo (%1):").arg(directory),
        QLineEdit::Normal, QStringLiteral("arquivo.cpp"), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    const QString cleanName = name.trimmed();

    const QString path = QDir(directory).filePath(cleanName);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, QObject::tr("Novo arquivo"),
                             QObject::tr("O arquivo ja existe:\n%1").arg(path));
        return;
    }

    QString content;
    if (cleanName.endsWith(QLatin1String(".h")) ||
        cleanName.endsWith(QLatin1String(".hpp"))) {
        content = QStringLiteral("#pragma once\n\n");
    } else if (cleanName.endsWith(QLatin1String(".cpp")) ||
               cleanName.endsWith(QLatin1String(".cc")) ||
               cleanName.endsWith(QLatin1String(".cxx"))) {
        content = QStringLiteral(
            "#include <iostream>\n\n"
            "// Criado na IDE Editor\n\n"
            "int main()\n{\n"
            "    std::cout << \"Hello!\\n\";\n"
            "    return 0;\n"
            "}\n");
    }

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(content.toUtf8());
        f.close();
    }

    addFileToProjectIfUnderRoot(path);
    m_editorTabs->openFile(path);
    statusBar()->showMessage(QObject::tr("Criado: %1").arg(path), 4000);
}

void MainWindow::onAddFilesToFolder(const QString& directory)
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QObject::tr("Adicionar arquivos ao projeto"), directory,
        QObject::tr("Codigo C/C++ (*.cpp *.hpp *.h *.cc *.cxx *.c);;"
                    "Todos (*)"));
    if (paths.isEmpty())
        return;

    for (const QString& p : paths) {
        QString target = p;
        const QString rel = QDir(m_project.rootDir).relativeFilePath(p);
        if (rel.startsWith(QLatin1String(".."))) {
            // Fora do projeto: copia para o diretorio selecionado.
            const QString dest = QDir(directory).filePath(QFileInfo(p).fileName());
            if (QFile::exists(dest)) {
                QMessageBox::warning(
                    this, QObject::tr("Adicionar arquivos"),
                    QObject::tr("Destino ja existe, ignorado:\n%1").arg(dest));
                continue;
            }
            if (!QFile::copy(p, dest)) {
                QMessageBox::warning(
                    this, QObject::tr("Adicionar arquivos"),
                    QObject::tr("Falha ao copiar:\n%1").arg(p));
                continue;
            }
            target = dest;
        }
        addFileToProjectIfUnderRoot(target);
    }

    if (m_project.isValid()) {
        m_projectExplorer->setProject(m_project);
    }
    statusBar()->showMessage(QObject::tr("Arquivos adicionados ao projeto"), 4000);
}

void MainWindow::onRemoveFile(const QString& path)
{
    if (!m_project.isValid())
        return;
    const QString rel = QDir(m_project.rootDir).relativeFilePath(path);

    if (QMessageBox::question(
            this, QObject::tr("Remover do projeto"),
            QObject::tr("Remover \"%1\" do projeto?\n\n"
                        "(o arquivo em disco NAO sera apagado)")
                .arg(path),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (!m_project.files.removeOne(rel))
        return;

    m_debugController->onFileClosed(path);
    persistProjectJson();
    m_projectExplorer->setProject(m_project);
    statusBar()->showMessage(QObject::tr("Removido do projeto: %1").arg(rel), 4000);
}

void MainWindow::onRenameFile(const QString& path)
{
    if (!m_project.isValid())
        return;

    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, QObject::tr("Renomear"),
        QObject::tr("Novo nome (na mesma pasta):"),
        QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == QFileInfo(path).fileName())
        return;

    const QString dir = QFileInfo(path).absolutePath();
    const QString newPath = QDir(dir).filePath(newName.trimmed());
    if (QFileInfo::exists(newPath)) {
        QMessageBox::warning(this, QObject::tr("Renomear"),
                             QObject::tr("Ja existe:\n%1").arg(newPath));
        return;
    }
    if (!QFile::rename(path, newPath)) {
        QMessageBox::warning(this, QObject::tr("Renomear"),
                             QObject::tr("Falha ao renomear:\n%1").arg(path));
        return;
    }

    // Atualiza a lista relativa e o caminho no editor aberto.
    const QString oldRel = QDir(m_project.rootDir).relativeFilePath(path);
    const QString newRel = QDir(m_project.rootDir).relativeFilePath(newPath);
    const int i = m_project.files.indexOf(oldRel);
    if (i >= 0)
        m_project.files[i] = newRel;

    for (int t = 0; t < m_editorTabs->count(); ++t) {
        auto* ed = m_editorTabs->editorAt(t);
        if (ed && QFileInfo(ed->filePath()) == QFileInfo(path)) {
            ed->setFilePath(newPath);
            m_editorTabs->retitleEditor(ed);
        }
    }

    persistProjectJson();
    m_projectExplorer->setProject(m_project);
    m_debugController->onFileClosed(path);
    statusBar()->showMessage(QObject::tr("Renomeado para: %1").arg(newPath), 4000);
}

// ---------------------------------------------------------------------------
// Build / Run / Conan (Parte 4)
// ---------------------------------------------------------------------------

QString MainWindow::currentConfigName() const
{
    return (m_configCombo && m_configCombo->currentIndex() >= 0)
        ? m_configCombo->currentText()
        : m_configName;
}

void MainWindow::onBuild()
{
    if (!m_project.isValid()) {
        statusBar()->showMessage(
            QObject::tr("Abra um projeto para compilar (F7)."), 4000);
        return;
    }
    if (m_buildManager->isBusy())
        return;
    saveAllDocuments();
    m_buildManager->build(m_project, currentConfigName());
}

void MainWindow::onRebuild()
{
    if (!m_project.isValid() || m_buildManager->isBusy())
        return;
    saveAllDocuments();
    m_buildManager->rebuild(m_project, currentConfigName());
}

void MainWindow::onClean()
{
    if (!m_project.isValid() || m_buildManager->isBusy())
        return;
    m_buildManager->clean(m_project, currentConfigName());
}

void MainWindow::onRun()
{
    if (!m_project.isValid()) {
        statusBar()->showMessage(
            QObject::tr("Abra um projeto para executar (Ctrl+F5)."), 4000);
        return;
    }
    if (m_buildManager->isBusy())
        return;
    saveAllDocuments();
    m_buildManager->runTarget(m_project, currentConfigName());
}

void MainWindow::onConanInstall()
{
    if (!m_project.isValid()) {
        statusBar()->showMessage(QObject::tr("Abra um projeto para usar Conan."),
                                 4000);
        return;
    }
    m_buildManager->conanInstall(m_project);
}

void MainWindow::onConanOpenFile()
{
    if (!m_project.isValid())
        return;
    QString path = build::ConanManager::writeConanfile(m_project);
    if (path.isEmpty()) {
        // Sem dependencias: ainda assim deixa o arquivo editavel.
        path = QDir(m_project.rootDir).filePath(QStringLiteral("conanfile.txt"));
        QFile f(path);
        if (!f.exists()) {
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write(build::ConanManager::conanfileContent(m_project).toUtf8());
                f.close();
            }
        }
    }
    m_editorTabs->openFile(path);
}

void MainWindow::onErrorActivated(const QString& filePath, int line)
{
    if (filePath.isEmpty())
        return;
    onSymbolActivated(filePath, qMax(0, line - 1));  // linha 1-based -> 0-based
}

void MainWindow::onBuildFinished(bool ok, int errors, int warnings)
{
    if (ok) {
        statusBar()->showMessage(
            QObject::tr("Build concluido com sucesso (%1 aviso(s)).")
                .arg(warnings),
            6000);
        // F5 com build pendente: agora que compilou, inicia a depuracao.
        if (m_debugAutoStart && m_debugController) {
            m_debugAutoStart = false;
            m_debugController->setProject(m_project);
            m_debugController->onStartContinue();
        }
    } else {
        m_debugAutoStart = false;
        statusBar()->showMessage(
            QObject::tr("Build com %1 erro(s), %2 aviso(s).")
                .arg(errors)
                .arg(warnings),
            8000);
    }
}

// ---------------------------------------------------------------------------
// Debug (Parte 5)
// ---------------------------------------------------------------------------

void MainWindow::onDebugStartContinue()
{
    m_debugController->onStartContinue();
}

void MainWindow::onDebugStop()
{
    m_debugController->onStop();
}

void MainWindow::onDebugStepOver()
{
    m_debugController->onStepOver();
}

void MainWindow::onDebugStepInto()
{
    m_debugController->onStepInto();
}

void MainWindow::onDebugStepOut()
{
    m_debugController->onStepOut();
}

void MainWindow::onToggleBreakpoint()
{
    m_debugController->onToggleBreakpoint();
}

void MainWindow::onDebugSessionChanged(bool active, bool running)
{
    const bool stopped = active && !running;
    m_debugStopAction->setEnabled(active);
    m_debugStepOverAction->setEnabled(stopped);
    m_debugStepIntoAction->setEnabled(stopped);
    m_debugStepOutAction->setEnabled(stopped);
    m_debugStartAction->setEnabled(!running);
    m_debugStartAction->setText(active ? QObject::tr("&Continuar")
                                       : QObject::tr("&Depurar"));
}

void MainWindow::onAddWatch()
{
    bool ok = false;
    const QString expr = QInputDialog::getText(
        this, QObject::tr("Adicionar a Inspecao"),
        QObject::tr("Expressao para observar (ex.: soma):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || expr.trimmed().isEmpty())
        return;
    m_debugController->addWatch(expr);
    m_debugController->refreshWatchDisplay();
}

void MainWindow::onProjectSettings()
{
    if (!m_project.isValid()) {
        QMessageBox::information(this, QObject::tr("Projeto"),
                                 QObject::tr("Abra ou crie um projeto primeiro."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QObject::tr("Configuracoes do Projeto - %1")
                           .arg(m_project.displayName()));
    auto* form = new QFormLayout(&dlg);

    auto* nameLabel = new QLabel(m_project.displayName(), &dlg);
    form->addRow(QObject::tr("Nome:"), nameLabel);

    // Compilador: o que existe no PATH, com o do projeto marcado.
    auto* compiler = new QComboBox(&dlg);
    compiler->setEditable(true);
    for (const QString& c : proj::Project::detectCompilers())
        compiler->addItem(c);
    if (!m_project.compiler.isEmpty() && compiler->findText(m_project.compiler) < 0)
        compiler->addItem(m_project.compiler);
    compiler->setCurrentText(m_project.compiler.isEmpty() ? QStringLiteral("g++")
                                                         : m_project.compiler);
    form->addRow(QObject::tr("Compilador:"), compiler);

    // Flags que mudam o resultado do preprocessamento (`-std=c++20`, ...).
    auto* flags = new QLineEdit(m_project.compilerFlags.join(QLatin1Char(' ')), &dlg);
    flags->setPlaceholderText(QStringLiteral("-std=c++20 -fno-exceptions"));
    form->addRow(QObject::tr("Flags:"), flags);

    // Include dirs extras, relativos a raiz do projeto (separados por ;).
    auto* incDirs = new QLineEdit(m_project.includeDirs.join(QLatin1Char(';')), &dlg);
    incDirs->setPlaceholderText(QStringLiteral("include;third_party/foo"));
    form->addRow(QObject::tr("Include dirs:"), incDirs);

    auto* target = new QLineEdit(m_project.makeTarget, &dlg);
    form->addRow(QObject::tr("Executavel (make):"), target);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                             | QDialogButtonBox::Cancel,
                                         &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    m_project.compiler = compiler->currentText().trimmed();
    if (m_project.compiler.isEmpty())
        m_project.compiler = QStringLiteral("g++");

    m_project.compilerFlags.clear();
    for (const QString& f : flags->text().split(QLatin1Char(' '), Qt::SkipEmptyParts))
        m_project.compilerFlags << f;

    m_project.includeDirs.clear();
    for (const QString& d : incDirs->text().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString t = d.trimmed();
        if (!t.isEmpty())
            m_project.includeDirs << t;
    }
    m_project.makeTarget = target->text().trimmed();
    if (m_project.makeTarget.isEmpty())
        m_project.makeTarget = m_project.name;
    persistProjectJson();
    configureIntellisense();
    m_projectExplorer->setProject(m_project);
    statusBar()->showMessage(QObject::tr("Configuracoes do projeto salvas"),
                             3000);
}

void MainWindow::configureIntellisense()
{
    editor::HeaderIndex* idx = editor::HeaderIndex::instance();
    if (!m_project.isValid()) {
        // Sem projeto: ainda assim usa o compilador do PATH, so sem cache.
        idx->configure(QStringLiteral("g++"), {}, {});
        idx->setCacheDir(QString());
        return;
    }
    QString compiler = m_project.compiler.trimmed();
    if (compiler.isEmpty())
        compiler = QStringLiteral("g++");
    // `-std=` e flags do projeto vao para o `g++ -E`; os `-I` dos includeDirs
    // tambem, para que `#include "utils/log.h"` preprocesse igual no build.
    idx->configure(compiler, m_project.preprocessFlags(),
                   m_project.resolvedIncludeDirs());
    idx->setCacheDir(QDir(m_project.rootDir).filePath(QStringLiteral(".editor-cache")));
}

// ---------------------------------------------------------------------------
// Persistencia do projeto
// ---------------------------------------------------------------------------

void MainWindow::persistProjectJson()
{
    if (!m_project.isValid() || m_projectJsonPath.isEmpty())
        return;
    if (m_debugController)
        m_project.breakpoints = m_debugController->allBreakpoints();
    if (!m_project.save(m_projectJsonPath))
        QMessageBox::warning(this, QObject::tr("Projeto"),
                             QObject::tr("Falha ao salvar o projeto:\n%1")
                                 .arg(m_projectJsonPath));
}

void MainWindow::addFileToProjectIfUnderRoot(const QString& absPath)
{
    if (!m_project.isValid())
        return;
    const QString rel = QDir(m_project.rootDir).relativeFilePath(absPath);
    if (rel.startsWith(QLatin1String("..")))
        return;
    if (m_project.files.contains(rel))
        return;
    m_project.files << rel;
    persistProjectJson();
    m_projectExplorer->setProject(m_project);
}

void MainWindow::onCurrentFileChanged(const QString& filePath)
{
    m_currentFile = filePath;
    updateWindowTitle();
    m_statusMode->setText(QFileInfo(filePath).fileName());
}

// ---------------------------------------------------------------------------
// Construcao da UI
// ---------------------------------------------------------------------------

void MainWindow::createActions()
{
    m_newAction = new QAction(QObject::tr("&Novo Arquivo..."), this);
    m_newAction->setShortcut(QKeySequence::New);
    connect(m_newAction, &QAction::triggered, this, &MainWindow::onNewFile);

    m_openAction = new QAction(QObject::tr("&Abrir..."), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpenFile);

    m_saveAction = new QAction(QObject::tr("&Salvar"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::onSave);

    m_saveAllAction = new QAction(QObject::tr("Sal&var Tudo"), this);
    m_saveAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(m_saveAllAction, &QAction::triggered,
            this, &MainWindow::saveAllDocuments);

    m_exitAction = new QAction(QObject::tr("E&xit"), this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, &QWidget::close);

    m_buildAction = new QAction(QObject::tr("&Compilar"), this);
    m_buildAction->setShortcut(QKeySequence(Qt::Key_F7));
    m_buildAction->setStatusTip(QObject::tr("Gera o Makefile e compila (F7)"));
    m_buildAction->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));

    m_rebuildAction = new QAction(QObject::tr("&Recompilar"), this);
    m_rebuildAction->setStatusTip(QObject::tr("Limpa e recompila tudo"));
    m_rebuildAction->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));

    m_cleanAction = new QAction(QObject::tr("&Limpar"), this);
    m_cleanAction->setStatusTip(QObject::tr("Remove a pasta build/"));
    m_cleanAction->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));

    m_runAction = new QAction(QObject::tr("E&xecutar sem depurar"), this);
    m_runAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    m_runAction->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));

    m_conanInstallAction =
        new QAction(QObject::tr("Conan: &Instalar dependencias"), this);
    m_conanInstallAction->setStatusTip(
        QObject::tr("Gera conanfile.txt e roda `conan install` (PkgConfigDeps)"));
    m_conanInstallAction
        ->setIcon(style()->standardIcon(QStyle::SP_DialogYesButton));

    m_conanEditAction = new QAction(QObject::tr("Conan: &Abrir conanfile.txt"), this);
    m_conanEditAction->setStatusTip(QObject::tr("Edita as dependencias Conan"));

    m_aboutAction = new QAction(QObject::tr("&Sobre a IDE..."), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::onAbout);

    m_newProjectAction = new QAction(QObject::tr("&Novo Projeto..."), this);
    connect(m_newProjectAction, &QAction::triggered,
            this, &MainWindow::onNewProject);

    m_openProjectAction = new QAction(QObject::tr("&Abrir Projeto..."), this);
    connect(m_openProjectAction, &QAction::triggered,
            this, &MainWindow::onOpenProject);

    m_closeProjectAction = new QAction(QObject::tr("&Fechar Projeto"), this);
    connect(m_closeProjectAction, &QAction::triggered,
            this, &MainWindow::onCloseProject);

    // --- Parte 5: debug ---
m_debugStartAction = new QAction(QObject::tr("&Depurar"), this);
    m_debugStartAction->setShortcut(QKeySequence(Qt::Key_F5));
    m_debugStartAction->setStatusTip(
        QObject::tr("Inicia/Continua a depuracao (F5)"));
    m_debugStartAction->setIcon(debugStartIcon());

    m_debugStopAction = new QAction(QObject::tr("&Parar Depuracao"), this);
    m_debugStopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    m_debugStopAction->setStatusTip(
        QObject::tr("Encerra a sessao de depuracao (Shift+F5)"));
    m_debugStopAction->setIcon(
        style()->standardIcon(QStyle::SP_MediaStop));

    m_debugStepIntoAction = new QAction(QObject::tr("Entrar (Step Into)"), this);
    m_debugStepIntoAction->setShortcut(QKeySequence(Qt::Key_F11));
    m_debugStepIntoAction->setIcon(
        style()->standardIcon(QStyle::SP_MediaSeekForward));

    m_debugStepOverAction = new QAction(QObject::tr("Ignorar (Step Over)"), this);
    m_debugStepOverAction->setShortcut(QKeySequence(Qt::Key_F10));
    m_debugStepOverAction->setIcon(
        style()->standardIcon(QStyle::SP_ArrowDown));

    m_debugStepOutAction = new QAction(QObject::tr("Sair (Step Out)"), this);
    m_debugStepOutAction->setShortcut(
        QKeySequence(Qt::SHIFT | Qt::Key_F11));
    m_debugStepOutAction->setIcon(
        style()->standardIcon(QStyle::SP_ArrowUp));

    m_toggleBreakpointAction = new QAction(QObject::tr("&Alternar Breakpoint"), this);
    m_toggleBreakpointAction->setShortcut(QKeySequence(Qt::Key_F9));
    m_toggleBreakpointAction->setStatusTip(
        QObject::tr("Insere/remove breakpoint na linha atual (F9)"));
    m_toggleBreakpointAction->setIcon(
        style()->standardIcon(QStyle::SP_DialogDiscardButton));

    m_addWatchAction = new QAction(QObject::tr("Adicionar a &Inspecao..."),
                                   this);
    m_addWatchAction->setStatusTip(
        QObject::tr("Observa uma expressao durante a depuracao"));

    m_projectSettingsAction =
        new QAction(QObject::tr("&Configuracoes do Projeto..."), this);
    m_projectSettingsAction->setStatusTip(
        QObject::tr("Compilador, executavel de destino etc."));
}

void MainWindow::createMenus()
{
    m_fileMenu = menuBar()->addMenu(QObject::tr("&Arquivo"));
    m_fileMenu->addAction(m_newAction);
    m_fileMenu->addAction(m_openAction);
    m_fileMenu->addSeparator();
    m_fileMenu->addAction(m_saveAction);
    m_fileMenu->addAction(m_saveAllAction);
    m_fileMenu->addSeparator();
    m_fileMenu->addAction(m_exitAction);

    m_projectMenu = menuBar()->addMenu(QObject::tr("&Projeto"));
    m_projectMenu->addAction(m_newProjectAction);
    m_projectMenu->addAction(m_openProjectAction);
    m_projectMenu->addAction(m_closeProjectAction);
    m_projectMenu->addSeparator();
    m_projectMenu->addAction(m_projectSettingsAction);

    m_editMenu = menuBar()->addMenu(QObject::tr("&Editar"));

    m_buildMenu = menuBar()->addMenu(QObject::tr("&Build"));
    m_buildMenu->addAction(m_buildAction);
    m_buildMenu->addAction(m_rebuildAction);
    m_buildMenu->addAction(m_cleanAction);
    m_buildMenu->addSeparator();
    m_buildMenu->addAction(m_conanInstallAction);
    m_buildMenu->addAction(m_conanEditAction);
    m_buildMenu->addSeparator();
    m_buildMenu->addAction(m_runAction);

    m_debugMenu = menuBar()->addMenu(QObject::tr("&Depurar"));
    m_debugMenu->addAction(m_debugStartAction);
    m_debugMenu->addAction(m_debugStopAction);
    m_debugMenu->addSeparator();
    m_debugMenu->addAction(m_debugStepIntoAction);
    m_debugMenu->addAction(m_debugStepOverAction);
    m_debugMenu->addAction(m_debugStepOutAction);
    m_debugMenu->addSeparator();
    m_debugMenu->addAction(m_runAction);
    m_debugMenu->addSeparator();
    m_debugMenu->addAction(m_toggleBreakpointAction);
    m_debugMenu->addAction(m_addWatchAction);

    m_viewMenu = menuBar()->addMenu(QObject::tr("&Exibir"));

    m_helpMenu = menuBar()->addMenu(QObject::tr("&Ajuda"));
    m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::createToolBars()
{
    auto* main = addToolBar(QObject::tr("Principal"));
    main->setObjectName("MainToolBar");
    main->setMovable(false);
    main->addAction(m_newAction);
    main->addAction(m_openAction);
    main->addAction(m_saveAction);
    main->addSeparator();
    main->addAction(m_buildAction);
    main->addAction(m_rebuildAction);
    main->addAction(m_cleanAction);
    main->addSeparator();
    main->addAction(m_runAction);

    QAction* dbgSep = main->addSeparator();
    main->addAction(m_debugStartAction);
    main->addAction(m_debugStopAction);
    main->addSeparator();
    main->addAction(m_debugStepIntoAction);
    main->addAction(m_debugStepOverAction);
    main->addAction(m_debugStepOutAction);
    main->addSeparator();
    main->addAction(m_toggleBreakpointAction);
    Q_UNUSED(dbgSep);

    m_configCombo = new QComboBox(main);
    m_configCombo->setFixedWidth(220);
    m_configCombo->addItem(QObject::tr("Debug"));
    m_configCombo->addItem(QObject::tr("Release"));
    main->addWidget(m_configCombo);
    connect(m_configCombo, &QComboBox::currentIndexChanged, this,
            [this] { m_configName = currentConfigName(); });
}

void MainWindow::createDocks()
{
    m_projectExplorer = new ProjectExplorer(this);
    addDockWidget(Qt::RightDockWidgetArea, m_projectExplorer);
    m_viewMenu->addAction(m_projectExplorer->toggleViewAction());

    m_outputWindow = new OutputWindow(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_outputWindow);
    m_viewMenu->addAction(m_outputWindow->toggleViewAction());

    m_debugDock = new DebugDock(this);
    // Mesma area do Project: os dois ficam no mesmo espaco, separados em abas
    // (equivale a arrastar um painel sobre o outro).
    addDockWidget(Qt::RightDockWidgetArea, m_debugDock);
    tabifyDockWidget(m_debugDock, m_projectExplorer);
    m_projectExplorer->raise();
    m_viewMenu->addAction(m_debugDock->toggleViewAction());

    m_editorTabs = new EditorTabWidget(this);
    setCentralWidget(m_editorTabs);

    connect(m_editorTabs, &EditorTabWidget::currentFileChanged,
            this, &MainWindow::onCurrentFileChanged);

    // Cliques na arvore "Project" abrem/saltam no editor.
    connect(m_projectExplorer, &ProjectExplorer::symbolActivated,
            this, &MainWindow::onSymbolActivated);
    connect(m_projectExplorer, &ProjectExplorer::fileActivated,
            this, &MainWindow::onFileActivated);

    // Menu de contexto (novo/adicionar/renomear/remover arquivos).
    connect(m_projectExplorer, &ProjectExplorer::newFileInFolderRequested,
            this, &MainWindow::onNewFileInFolder);
    connect(m_projectExplorer, &ProjectExplorer::addFilesToFolderRequested,
            this, &MainWindow::onAddFilesToFolder);
    connect(m_projectExplorer, &ProjectExplorer::removeFileRequested,
            this, &MainWindow::onRemoveFile);
    connect(m_projectExplorer, &ProjectExplorer::renameFileRequested,
            this, &MainWindow::onRenameFile);

    // Error List (OutoPut): duplo-clique abre o arquivo na linha do erro.
    connect(m_outputWindow, &OutputWindow::errorActivated,
            this, &MainWindow::onErrorActivated);
}

void MainWindow::createStatusBar()
{
    if (m_statusMode)
        return;

    statusBar()->setSizeGripEnabled(true);

    m_statusMode = new QLabel(QObject::tr("Pronto"), this);
    m_statusMode->setMinimumWidth(140);
    statusBar()->addWidget(m_statusMode);

    auto* modeCombo = new QComboBox(this);
    modeCombo->addItems({QObject::tr("Codigo"), QObject::tr("Debug")});
    modeCombo->setFixedWidth(110);
    statusBar()->addPermanentWidget(modeCombo);

    statusBar()->showMessage(QObject::tr("Pronto"), 3000);
}

void MainWindow::updateWindowTitle()
{
    if (m_currentFile.isEmpty())
        setWindowTitle(QObject::tr("Editor — [*]"));
    else
        setWindowTitle(QObject::tr("Editor — %1 [*]")
                           .arg(QFileInfo(m_currentFile).fileName()));
    setWindowModified(m_editorTabs->currentWidget()
                          ? m_editorTabs->editorAt(m_editorTabs->currentIndex())
                                ->isModified()
                          : false);
}

}  // namespace ui