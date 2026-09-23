#pragma once

#include "debug/GdbDebugger.hpp"
#include "project/Project.hpp"

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <QVector>

class QStatusBar;

namespace ui {
class CodeEditorWidget;
class DebugDock;
class EditorTabWidget;
class OutputWindow;
}  // namespace ui

namespace debug {

//! Estado de um breakpoint no modelo do depurador.
struct Bp {
    int number = -1;   // numero no gdb (-1 = ainda nao registrado)
    bool enabled = true;
};

//! Orquestra a depuracao: breakpoints (modelo + margem), sessao gdb e
//! atualizacao de UI (seta de execucao, Call Stack, Locais, consoles).
//! Nao conhece menus/toolbars; o MainWindow mapeia acoes for slots.
class DebugController final : public QObject {
    Q_OBJECT

public:
    DebugController(ui::EditorTabWidget* tabs, ui::OutputWindow* output,
                    ui::DebugDock* dock, QStatusBar* status,
                    QObject* parent = nullptr);
    ~DebugController() override;

    //! (Re)define o projeto de destino do depurador (MainWindow no open/close).
    void setProject(const proj::Project& project);

    bool isSessionActive() const { return m_gdb.sessionActive(); }

    //! Restaura breakpoints persistidos ao abrir um projeto (MainWindow).
    void setStoredBreakpoints(const QVector<proj::BreakpointRecord>& bps);
    //! Snapshop do modelo para persistir no JSON (MainWindow).
    QVector<proj::BreakpointRecord> allBreakpoints() const;

public slots:
    //! F9: alterna breakpoint na linha do cursor do editor ativo.
    void onToggleBreakpoint();
    //! Alterna breakpoint num arquivo/linha (margem clicada).
    void onToggleBreakpointAt(const QString& file, int line);
    //! Remove um breakpoint (janela de Breakpoints/margem de novo clique).
    void onRemoveBreakpointAt(const QString& file, int line);
    //! Ativa/desativa um breakpoint (janela de Breakpoints).
    void onSetBreakpointEnabled(const QString& file, int line, bool enabled);
    //! Inicia a sessao ou continua a execucao parada.
    void onStartContinue();
    //! Shift+F5: interrompe e encerra a sessao.
    void onStop();
    void onStepOver();
    void onStepInto();
    void onStepOut();

    //! Watch: adiciona/remove expressao observada (janela "Inspecao").
    void addWatch(const QString& expr);
    void removeWatch(const QString& expr);
    void clearWatches();
    //! Atualiza a aba "Inspecao" com a lista atual (MainWindow/menu).
    void refreshWatchDisplay();

    //! Tooltip: mouse parado sobre um simbolo enquanto depura parado.
    void onHoverWord(const QString& word, const QPoint& globalPos);

    //! Limpa breakpoints ao remover/renomear um arquivo do projeto.
    void onFileClosed(const QString& filePath);

signals:
    //! Estado da sessao para o MainWindow habilitar/desabilitar acoes.
    //! running=false com session=true => inferior parado (steps liberados).
    void stateChanged(bool sessionActive, bool running);

    //! O MainWindow deve compilar antes de depurar (executavel ausente).
    void buildRequested();

    //! O MainWindow deve exibir o console de debug para o usuario ver a sessao.
    void debugConsoleWanted();

private slots:
    void onEditorCreated(ui::CodeEditorWidget* editor);
    void onRunningChanged(bool running);
    void onStopped(const QString& reason, const debug::MiFrame& frame);
    void onExited(int exitCode);
    void onBreakpointInserted(const QString& file, int line, int number);
    void onBreakpointInsertFailed(const QString& file, int line);
    void onConsoleOutput(const QString& text);
    void onStackUpdated(const QVector<debug::MiFrame>& frames);
    void onLocalsTypesUpdated(const QString& func,
                              const QVector<debug::MiVar>& vars);
    void onLocalsValuesUpdated(const QString& func,
                               const QVector<debug::MiVar>& vars);
    void onEvaluated(const QString& expr, const QString& value, bool ok);
    void onError(const QString& text);
    void onCallStackActivated(const QString& file, int line);
    void onAddToWatchRequested(const QString& expr);

private:
    void updateBreakpointDisplay(ui::CodeEditorWidget* editor);
    void updateAllBreakpointDisplay();
    void refreshBreakpointDock();
    //! Envia os breakpoints atuais ao gdb (novos e os pendentes).
    void pushBreakpointsToSession();
    void applyBreakpointsFromEditors();  // margens ja visiveis
    void setExecutionPosition(const QString& file, int line);
    void clearExecutionPosition();
    void writeConsole(const QString& text);
    void status(const QString& message);
    //! Merge tipos+valores dos locais quando ambos os reponses chegaram.
    void tryShowLocals(const QString& func);
    //! Reavalia todas as watches (chamado quando para num breakpoint).
    void refreshWatches();
    //! Mostra tooltip apos -data-evaluate-expression da palavra suspensa.
    void showTooltipIfPending(const QString& expr, const QString& value);

    ui::EditorTabWidget* m_tabs = nullptr;
    ui::OutputWindow* m_output = nullptr;
    ui::DebugDock* m_dock = nullptr;
    QStatusBar* m_status = nullptr;

    // file abs -> (linha -> estado do breakpoint).
    QHash<QString, QHash<int, Bp>> m_breakpoints;

    // Locais: acumula meio-resultado das duas consultas por parada.
    QHash<QString, QString> m_localTypes;
    QHash<QString, QString> m_localValues;
    bool m_haveLocalTypes = false;
    bool m_haveLocalValues = false;
    QString m_localsFunc;

    // Watch (janela "Inspecao").
    QStringList m_watches;

    // Tooltip pendente (palavra em espera da avaliacao).
    QString m_tooltipWord;
    QPoint m_tooltipPos;

    GdbDebugger m_gdb;
    proj::Project m_project;
    QString m_currentFile;
    int m_currentLine = -1;
    bool m_stopping = false;  // aguardando parada para encerrar (Shift+F5)
};

}  // namespace debug