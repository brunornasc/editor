#include "debug/GdbDebugger.hpp"

#include <QProcess>
#include <QStringList>

#include <QDebug>

namespace debug {

GdbDebugger::GdbDebugger(QObject* parent)
    : QObject(parent)
    , m_proc(nullptr)
{
}

GdbDebugger::~GdbDebugger()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
    }
}

bool GdbDebugger::start(const QString& exe, const QString& workDir)
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
        m_proc = nullptr;
    }

    auto* proc = new QProcess(this);
    QStringList args;
    args << QStringLiteral("--interpreter=mi2")
         << QStringLiteral("--nx")
         << QStringLiteral("-iex")
         << QStringLiteral("set debuginfod enabled off")
         << QStringLiteral("-q")
         << exe;
    proc->setProgram(QStringLiteral("gdb"));
    proc->setArguments(args);
    proc->setWorkingDirectory(workDir);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyReadStandardOutput,
            this, &GdbDebugger::onReadyRead);
    connect(proc, &QProcess::finished, this, &GdbDebugger::onFinished);
    connect(proc, &QProcess::errorOccurred, this, &GdbDebugger::onErrorOccurred);

    proc->start();
    if (!proc->waitForStarted(5000))
        return false;

    m_proc = proc;
    m_running = false;
    m_buffer.clear();
    m_pending.clear();
    emit started();
    return true;
}

void GdbDebugger::send(const QString& cmd, PendingCommand::Type type,
                       const QString& file, int line)
{
    if (!m_proc)
        return;
    const int token = m_nextToken++;
    m_pending.append(PendingCommand{token, type, file, line, {}});
    m_proc->write(QStringLiteral("%1%2\n").arg(token).arg(cmd).toUtf8());
}

void GdbDebugger::sendEval(const QString& cmd, const QString& expr)
{
    if (!m_proc)
        return;
    const int token = m_nextToken++;
    m_pending.append(PendingCommand{token, PendingCommand::Type::EvalExpr, {},
                                    -1, expr});
    m_proc->write(QStringLiteral("%1%2\n").arg(token).arg(cmd).toUtf8());
}

void GdbDebugger::runExec()
{
    if (m_proc && !m_running)
        send(QStringLiteral("-exec-run"), PendingCommand::Type::Run);
}

void GdbDebugger::continueExec()
{
    if (m_proc && !m_running)
        send(QStringLiteral("-exec-continue"), PendingCommand::Type::Run);
}

void GdbDebugger::stepOver()
{
    if (m_proc && !m_running)
        send(QStringLiteral("-exec-next"), PendingCommand::Type::Run);
}

void GdbDebugger::stepInto()
{
    if (m_proc && !m_running)
        send(QStringLiteral("-exec-step"), PendingCommand::Type::Run);
}

void GdbDebugger::stepOut()
{
    if (m_proc && !m_running)
        send(QStringLiteral("-exec-finish"), PendingCommand::Type::Run);
}

void GdbDebugger::interrupt()
{
    if (m_proc && m_running)
        send(QStringLiteral("-exec-interrupt"), PendingCommand::Type::Run);
}

void GdbDebugger::shutdown()
{
    if (m_proc)
        send(QStringLiteral("-gdb-exit"), PendingCommand::Type::Run);
}

void GdbDebugger::insertBreakpoint(const QString& file, int line)
{
    if (m_proc) {
        // Nos usamos linhas 0-based; o gdb espera numeracao 1-based.
        QString loc;
        if (file.contains(QLatin1Char(' ')))
            loc = QStringLiteral("\"%1\":%2").arg(file).arg(line + 1);
        else
            loc = QStringLiteral("%1:%2").arg(file).arg(line + 1);
        send(QStringLiteral("-break-insert %1").arg(loc),
             PendingCommand::Type::BreakInsert, file, line);
    }
}

void GdbDebugger::deleteBreakpoint(int number)
{
    if (m_proc)
        send(QStringLiteral("-break-delete %1").arg(number),
             PendingCommand::Type::Run);
}

void GdbDebugger::setBreakpointEnabled(int number, bool enabled)
{
    if (m_proc)
        send(QStringLiteral("-break-%1 %2")
                 .arg(enabled ? QStringLiteral("enable")
                              : QStringLiteral("disable"))
                 .arg(number),
             PendingCommand::Type::Run);
}

void GdbDebugger::queryStack()
{
    send(QStringLiteral("-stack-list-frames"), PendingCommand::Type::StackFrames);
}

void GdbDebugger::queryLocals()
{
    // Tipo vem de --simple-values; valor completo de --all-values
    // (o gdb 12 omite `type` com --all-values sozinho).
    send(QStringLiteral("-stack-list-variables --simple-values"),
         PendingCommand::Type::StackVarTypes);
    send(QStringLiteral("-stack-list-variables --all-values"),
         PendingCommand::Type::StackVarValues);
}

void GdbDebugger::evaluate(const QString& expr)
{
    if (!m_proc)
        return;
    QString quoted = expr;
    quoted.replace(QLatin1Char('"'), QLatin1String("\\\""));
    sendEval(QStringLiteral("-data-evaluate-expression \"%1\"").arg(quoted),
             expr);
}

void GdbDebugger::onReadyRead()
{
    if (m_proc)
        m_buffer += m_proc->readAll();
    pumpBuffer();
}

void GdbDebugger::pumpBuffer()
{
    while (true) {
        const int idx = m_buffer.indexOf('\n');
        if (idx < 0)
            break;
        const QByteArray line = m_buffer.left(idx).trimmed();
        m_buffer.remove(0, idx + 1);
        if (line.isEmpty())
            continue;
        // Prompt: linha so com "(gdb) " deve ser ignorada.
        if (line == "(gdb)")
            continue;

        const MiRecord rec = parseRecord(QString::fromUtf8(line));
        if (!rec.ok)
            continue;

        switch (rec.kind) {
        case '^':
            handleResult(rec);
            break;
        case '*':
            if (rec.cls == QLatin1String("running")) {
                if (!m_running) {
                    m_running = true;
                    emit runningStateChanged(true);
                }
            } else if (rec.cls == QLatin1String("stopped")) {
                handleStopped(rec);
            }
            break;
        case '~':
        case '@':
        case '&':
            emit consoleOutput(unquote(rec.body).trimmed());
            break;
        default:
            // Linha que nao e registro MI: stdout/saida direta do inferior
            // (o gdb repassa a saida do programa crua no mesmo pipe).
            if (rec.kind == 0)
                emit consoleOutput(line);
            break;
        }
    }
}

void GdbDebugger::handleResult(const MiRecord& rec)
{
    // Acha o comando pendente por token (FIFO normalmente, mas robusto).
    int pendingIdx = -1;
    for (int i = 0; i < m_pending.size(); ++i) {
        if (rec.token > 0 && m_pending.at(i).token == rec.token) {
            pendingIdx = i;
            break;
        }
    }
    if (pendingIdx < 0 && m_pending.isEmpty())
        return;  // resposta de comando interno do gdb
    const int idx = (pendingIdx >= 0) ? pendingIdx : 0;
    const PendingCommand cmd = m_pending.at(idx);
    m_pending.removeAt(idx);

    if (rec.cls == QLatin1String("error")) {
        const QMap<QString, QString> p = parsePairs(rec.body);
        const QString msg = p.value(QStringLiteral("msg"));
        // Erros de avaliacao sao silenciosos (watch/tooltip nao existem
        // no escopo); demais vao ao console.
        if (cmd.type == PendingCommand::Type::EvalExpr)
            emit evaluated(cmd.expr, QString(), false);
        else
            emit errorMessage(msg);
        return;
    }

    switch (cmd.type) {
    case PendingCommand::Type::Run:
        break;  // continuidade chega via *running/*stopped
    case PendingCommand::Type::BreakInsert: {
        const QMap<QString, QString> top = parsePairs(rec.body);
        const QString bkpt = top.value(QStringLiteral("bkpt"));
        if (!bkpt.isEmpty()) {
            const QMap<QString, QString> p = parsePairs(bkpt);
            const int number = p.value(QStringLiteral("number")).toInt();
            if (number > 0)
                emit breakpointInserted(cmd.file, cmd.line, number);
            else
                emit breakpointInsertFailed(cmd.file, cmd.line);
        } else {
            emit breakpointInsertFailed(cmd.file, cmd.line);
        }
        break;
    }
    case PendingCommand::Type::StackFrames: {
        const QVector<MiFrame> frames = parseFrames(rec.body);
        emit stackUpdated(frames);
        break;
    }
    case PendingCommand::Type::StackVarTypes: {
        const QVector<MiVar> vars = parseVars(rec.body);
        emit localsTypesUpdated(m_lastFunc, vars);
        break;
    }
    case PendingCommand::Type::StackVarValues: {
        const QVector<MiVar> vars = parseVars(rec.body);
        emit localsValuesUpdated(m_lastFunc, vars);
        break;
    }
    case PendingCommand::Type::EvalExpr: {
        const QMap<QString, QString> p = parsePairs(rec.body);
        emit evaluated(cmd.expr, p.value(QStringLiteral("value")), true);
        break;
    }
    default:
        break;
    }
}

void GdbDebugger::handleStopped(const MiRecord& rec)
{
    if (m_running) {
        m_running = false;
        emit runningStateChanged(false);
    }

    const QMap<QString, QString> top = parsePairs(rec.body);
    const QString reason = top.value(QStringLiteral("reason"));

    MiFrame frame;
    const QString frameStr = top.value(QStringLiteral("frame"));
    if (!frameStr.isEmpty()) {
        const QMap<QString, QString> p = parsePairs(frameStr);
        frame.func = p.value(QStringLiteral("func"));
        frame.file = p.value(QStringLiteral("file"));
        frame.fullname = p.value(QStringLiteral("fullname"));
        frame.line = p.value(QStringLiteral("line")).toInt();
    }
    if (!frame.func.isEmpty())
        m_lastFunc = frame.func;
    emit stopped(reason, frame);
}

void GdbDebugger::onFinished(int exitCode, QProcess::ExitStatus)
{
    if (m_proc) {
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_running = false;
    m_pending.clear();
    emit exited(exitCode);
}

void GdbDebugger::onErrorOccurred(QProcess::ProcessError err)
{
    if (err == QProcess::FailedToStart)
        emit errorMessage(QStringLiteral("Falha ao iniciar o gdb."));
}

}  // namespace debug