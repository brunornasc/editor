#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

class QByteArray;

#include "project/Project.hpp"
#include "ui/EditorTabWidget.hpp"
#include "ui/OutputWindow.hpp"

namespace build {

//! Orquestra build/run/Conan da Parte 4:
//!   * gera (MakefileGenerator) e executa `make` via QProcess;
//!   * reencaminha a saida para o Build Output (com cores VS2005);
//!   * converte linhas do compilador em erros navegaveis (Error List);
//!   * aplica anotacoes de erro no editor aberto;
//!   * `runTarget` compila e, se ok, lanca o executavel no Debug Console;
//!   * `conanInstall` roda `conan install` (PkgConfigDeps).
class BuildManager final : public QObject {
    Q_OBJECT

public:
    BuildManager(ui::OutputWindow* output, ui::EditorTabWidget* tabs,
                 QObject* parent = nullptr);

    bool isBusy() const { return m_state != State::Idle; }

    //! Documentos nao modificados nao sao tocados; MainWindow salva antes.
    void build(const proj::Project& project, const QString& config);
    void rebuild(const proj::Project& project, const QString& config);
    void clean(const proj::Project& project, const QString& config);

    //! Compila (se preciso) e executa o binario com saida no Debug Console.
    void runTarget(const proj::Project& project, const QString& config);

    //! Rodar `conan install . -of=build/conan` (gera conanfile.txt antes).
    void conanInstall(const proj::Project& project);

signals:
    void buildFinished(bool ok, int errors, int warnings);
    void runFinished(int exitCode);

private slots:
    void onMakeFinished(int exitCode, QProcess::ExitStatus status);
    void onRunProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onConanFinished(int exitCode, QProcess::ExitStatus status);

private:
    enum class State { Idle, Building, Running };

    //! Lanca `make -C rootDir <target> CONFIG=<config>`; saida no BuildOutput.
    void runMake(const proj::Project& project, const QString& config,
                 const QString& makeTarget);
    //! Executa build/<makeTarget> com saida no Debug Console.
    void launchTarget(const proj::Project& project);
    //! Consome stdout+stderr do processo (linhas parciais sao acumuladas).
    void drainOutput(QProcess* proc, ui::OutputWindow::Pane pane);
    //! Cor/parsing de uma linha do compilador; preenche `m_entries`.
    void handleBuildLine(const QString& line);
    //! Depois do make: preenche Error List e anotacoes no editor.
    void finalizeBuild(bool ok);
    //! Aplica os erros de um arquivo no editor aberto correspondente.
    void applyErrorMarkers();

    ui::OutputWindow* m_output = nullptr;
    ui::EditorTabWidget* m_tabs = nullptr;

    //! Copia do projeto do build/run ativo (evita dangling de referencia).
    proj::Project m_project;
    QString m_config;
    bool m_runAfterBuild = false;  // runTarget: build => executa binario

    QProcess* m_makeProc = nullptr;
    QProcess* m_runProc = nullptr;
    QProcess* m_conanProc = nullptr;

    QByteArray m_lineBuffer;  // stdout parcial do make
    QVector<ui::ErrorEntry> m_entries;
    int m_errorCount = 0;
    int m_warningCount = 0;
    State m_state = State::Idle;
};

}  // namespace build