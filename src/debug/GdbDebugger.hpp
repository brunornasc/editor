#pragma once

#include "debug/MiParser.hpp"

#include <QObject>
#include <QProcess>

class QByteArray;

namespace debug {

//! Comando pendente (token MI -> tipo) aguardando resposta "^...".
struct PendingCommand {
    enum class Type : int {
        None = 0,
        Run,          // -exec-run / -exec-continue / -exec-next / -exec-step /
                      // -exec-finish / -exec-interrupt / -gdb-exit
        BreakInsert,
        StackFrames,
        StackVarTypes,   // -stack-list-variables --simple-values (tem tipo)
        StackVarValues,  // -stack-list-variables --all-values (tem valor)
        EvalExpr,        // -data-evaluate-expression "expr"
    };

    int token = -1;
    Type type = Type::None;
    QString file;   // p/ BreakInsert
    int line = -1;  // p/ BreakInsert
    QString expr;   // p/ EvalExpr
};

//! Motor de depuracao: aciona `gdb --interpreter=mi2`, fila comandos por
//! token e emiti eventos de parada/continuacao para o DebugController.
class GdbDebugger final : public QObject {
    Q_OBJECT

public:
    explicit GdbDebugger(QObject* parent = nullptr);
    ~GdbDebugger() override;

    //! true se ha um processo gdb vivo.
    bool sessionActive() const { return m_proc != nullptr; }

    //! true se o inferior (programa depurado) esta executando.
    bool inferiorRunning() const { return m_running; }

    //! Abre um processo gdb MI2 para o executavel.
    bool start(const QString& exe, const QString& workDir);

    //! Interrompe o inferior (-exec-interrupt).
    void interrupt();

    //! Encerra a sessao (-gdb-exit).
    void shutdown();

    void runExec();
    void continueExec();
    void stepOver();
    void stepInto();
    void stepOut();
    void insertBreakpoint(const QString& file, int line);
    void deleteBreakpoint(int number);
    //! Ativa/desativa um breakpoint ja registrado no gdb.
    void setBreakpointEnabled(int number, bool enabled);
    void queryStack();
    //! Tipos (--simple-values) e valores (--all-values) dos locais do quadro.
    void queryLocals();
    //! Avalia uma expressao no quadro atual (watch/tooltip).
    void evaluate(const QString& expr);

signals:
    void started();
    void runningStateChanged(bool running);
    //! Inferior parado (breakpoint/fim de step/tela). frame = quadro atual.
    void stopped(const QString& reason, const debug::MiFrame& frame);
    void exited(int exitCode);
    //! Texto de console/saida do proprio gdb.
    void consoleOutput(const QString& text);
    //! Breakpoint registrado no gdb.
    void breakpointInserted(const QString& file, int line, int number);
    void breakpointInsertFailed(const QString& file, int line);
    void stackUpdated(const QVector<debug::MiFrame>& frames);
    //! Locais com TIPO (de -stack-list-variables --simple-values).
    void localsTypesUpdated(const QString& func,
                            const QVector<debug::MiVar>& vars);
    //! Locais com VALOR (de -stack-list-variables --all-values).
    void localsValuesUpdated(const QString& func,
                             const QVector<debug::MiVar>& vars);
    //! Resultado de evaluate() (watch/tooltip). ok=false em erro de expressao.
    void evaluated(const QString& expr, const QString& value, bool ok);
    void errorMessage(const QString& text);

private slots:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError err);

private:
    void send(const QString& cmd, PendingCommand::Type type,
              const QString& file = {}, int line = -1);
    void sendEval(const QString& cmd, const QString& expr);
    void pumpBuffer();
    void handleResult(const debug::MiRecord& rec);
    void handleStopped(const debug::MiRecord& rec);

    QProcess* m_proc = nullptr;
    QList<PendingCommand> m_pending;
    QByteArray m_buffer;
    int m_nextToken = 1;
    bool m_running = false;
    QString m_lastFunc;
};

}  // namespace debug