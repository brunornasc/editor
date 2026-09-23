#include "debug/DebugController.hpp"

#include "ui/CodeEditorWidget.hpp"
#include "ui/DebugDock.hpp"
#include "ui/EditorTabWidget.hpp"
#include "ui/OutputWindow.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStatusBar>
#include <QToolTip>

#include <algorithm>
#include <QSet>

namespace debug {

namespace {

//! Caminho do executavel gerado pelo make (Parte 4).
QString executableFor(const proj::Project& p)
{
    if (p.rootDir.isEmpty() || p.makeTarget.isEmpty())
        return {};
    return QDir(p.rootDir).filePath(QStringLiteral("build/%1").arg(p.makeTarget));
}

}  // namespace

DebugController::DebugController(ui::EditorTabWidget* tabs,
                                 ui::OutputWindow* output,
                                 ui::DebugDock* dock,
                                 QStatusBar* status,
                                 QObject* parent)
    : QObject(parent)
    , m_tabs(tabs)
    , m_output(output)
    , m_dock(dock)
    , m_status(status)
{
    connect(m_tabs, &ui::EditorTabWidget::editorCreated,
            this, &DebugController::onEditorCreated);
    connect(m_tabs, &ui::EditorTabWidget::currentFileChanged, this,
            [this](const QString& path) { m_currentFile = path; });

    connect(&m_gdb, &GdbDebugger::runningStateChanged,
            this, &DebugController::onRunningChanged);
    connect(&m_gdb, &GdbDebugger::stopped,
            this, &DebugController::onStopped);
    connect(&m_gdb, &GdbDebugger::exited,
            this, &DebugController::onExited);
    connect(&m_gdb, &GdbDebugger::breakpointInserted,
            this, &DebugController::onBreakpointInserted);
    connect(&m_gdb, &GdbDebugger::breakpointInsertFailed,
            this, &DebugController::onBreakpointInsertFailed);
    connect(&m_gdb, &GdbDebugger::consoleOutput,
            this, &DebugController::onConsoleOutput);
    connect(&m_gdb, &GdbDebugger::stackUpdated,
            this, &DebugController::onStackUpdated);
    connect(&m_gdb, &GdbDebugger::localsTypesUpdated,
            this, &DebugController::onLocalsTypesUpdated);
    connect(&m_gdb, &GdbDebugger::localsValuesUpdated,
            this, &DebugController::onLocalsValuesUpdated);
    connect(&m_gdb, &GdbDebugger::evaluated,
            this, &DebugController::onEvaluated);
    connect(&m_gdb, &GdbDebugger::errorMessage,
            this, &DebugController::onError);

    connect(m_dock, &ui::DebugDock::frameActivated,
            this, &DebugController::onCallStackActivated);
    connect(m_dock, &ui::DebugDock::breakpointToggled,
            this, &DebugController::onSetBreakpointEnabled);
    connect(m_dock, &ui::DebugDock::breakpointRemoved,
            this, &DebugController::onRemoveBreakpointAt);
    connect(m_dock, &ui::DebugDock::addToWatchRequested,
            this, &DebugController::onAddToWatchRequested);
    connect(m_dock, &ui::DebugDock::watchRemoved,
            this, &DebugController::removeWatch);
}

DebugController::~DebugController() = default;

void DebugController::setProject(const proj::Project& project)
{
    // Se havia sessao, encerra antes de trocar de projeto.
    if (m_gdb.sessionActive())
        m_gdb.shutdown();
    m_project = project;
    // Estado de debug nao pertence a outro projeto.
    m_breakpoints.clear();
    m_watches.clear();
    updateAllBreakpointDisplay();
    m_dock->setBreakpointsList({});
    m_dock->setWatchExpressions({});
    m_dock->clearWatchValues();
}

void DebugController::onEditorCreated(ui::CodeEditorWidget* editor)
{
    connect(editor, &ui::CodeEditorWidget::breakpointMarginClicked, this,
            [this, editor](int line) {
                // Garante que a aba do arquivo clicado fique em frente.
                m_tabs->setCurrentWidget(editor);
                onToggleBreakpointAt(editor->filePath(), line);
            });
    // Tooltip de valor no hover (mouse parado sobre simbolo).
    connect(editor, &ui::CodeEditorWidget::hoverWordRequested, this,
            &DebugController::onHoverWord);
    updateBreakpointDisplay(editor);
}

void DebugController::updateBreakpointDisplay(ui::CodeEditorWidget* editor)
{
    if (!editor || editor->filePath().isEmpty())
        return;
    const auto& map = m_breakpoints.value(editor->filePath());
    QList<int> lines = map.keys();
    std::sort(lines.begin(), lines.end());
    QSet<int> disabled;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        if (!it.value().enabled)
            disabled.insert(it.key());
    editor->setBreakpoints(lines, disabled);
}

void DebugController::updateAllBreakpointDisplay()
{
    for (int i = 0; i < m_tabs->count(); ++i)
        updateBreakpointDisplay(m_tabs->editorAt(i));
}

void DebugController::onToggleBreakpoint()
{
    const int index = m_tabs->currentIndex();
    auto* editor = m_tabs->editorAt(index);
    if (!editor)
        return;
    int line = 0, col = 0;
    editor->editor()->getCursorPosition(&line, &col);
    onToggleBreakpointAt(editor->filePath(), line);
}

void DebugController::onToggleBreakpointAt(const QString& file, int line)
{
    if (file.isEmpty()) {
        status(tr("Salve o arquivo antes de usar breakpoints."));
        return;
    }
    auto& map = m_breakpoints[file];
    if (map.contains(line)) {
        onRemoveBreakpointAt(file, line);
        return;
    }
    Bp bp;
    map.insert(line, bp);
    if (m_gdb.sessionActive())
        m_gdb.insertBreakpoint(file, line);
    status(tr("Breakpoint: %1:%2")
               .arg(QFileInfo(file).fileName()).arg(line + 1));
    updateAllBreakpointDisplay();
    refreshBreakpointDock();
}

void DebugController::onRemoveBreakpointAt(const QString& file, int line)
{
    if (!file.isEmpty()) {
        auto it = m_breakpoints.find(file);
        if (it != m_breakpoints.end()) {
            auto lit = it.value().find(line);
            if (lit != it.value().end()) {
                const int number = lit.value().number;
                it.value().erase(lit);
                if (m_gdb.sessionActive() && number > 0)
                    m_gdb.deleteBreakpoint(number);
            }
            if (it.value().isEmpty())
                m_breakpoints.erase(it);
        }
    }
    status(tr("Breakpoint removido: %1:%2")
               .arg(QFileInfo(file).fileName()).arg(line + 1));
    updateAllBreakpointDisplay();
    refreshBreakpointDock();
}

void DebugController::onSetBreakpointEnabled(const QString& file, int line,
                                             bool enabled)
{
    auto it = m_breakpoints.find(file);
    if (it == m_breakpoints.end())
        return;
    auto lit = it.value().find(line);
    if (lit == it.value().end())
        return;
    Bp& bp = lit.value();
    if (bp.enabled == enabled)
        return;
    bp.enabled = enabled;
    if (m_gdb.sessionActive() && bp.number > 0)
        m_gdb.setBreakpointEnabled(bp.number, enabled);
    updateAllBreakpointDisplay();
    refreshBreakpointDock();
}

void DebugController::setStoredBreakpoints(
    const QVector<proj::BreakpointRecord>& bps)
{
    m_breakpoints.clear();
    for (const auto& r : bps) {
        if (r.file.isEmpty() || r.line < 0)
            continue;
        Bp bp;
        bp.enabled = r.enabled;
        m_breakpoints[r.file].insert(r.line, bp);
    }
    updateAllBreakpointDisplay();
    refreshBreakpointDock();
}

QVector<proj::BreakpointRecord> DebugController::allBreakpoints() const
{
    QVector<proj::BreakpointRecord> out;
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        for (auto lit = it.value().constBegin(); lit != it.value().constEnd(); ++lit) {
            proj::BreakpointRecord r;
            r.file = it.key();
            r.line = lit.key();
            r.enabled = lit.value().enabled;
            out.append(r);
        }
    }
    return out;
}

void DebugController::pushBreakpointsToSession()
{
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        const QString& file = it.key();
        for (auto lit = it.value().constBegin(); lit != it.value().constEnd(); ++lit)
            m_gdb.insertBreakpoint(file, lit.key());
    }
}

void DebugController::onStartContinue()
{
    if (m_project.rootDir.isEmpty()) {
        status(tr("Abra um projeto antes de depurar."));
        return;
    }
    if (!m_gdb.sessionActive()) {
        const QString exe = executableFor(m_project);
        if (exe.isEmpty() || !QFileInfo::exists(exe)) {
            status(tr("Compilando antes de depurar..."));
            emit buildRequested();
            return;
        }
        if (!m_gdb.start(exe, m_project.rootDir)) {
            status(tr("Falha ao iniciar o gdb."));
            return;
        }
        m_dock->clearAll();
        clearExecutionPosition();
        emit debugConsoleWanted();
        writeConsole(tr("=== Depuracao iniciada (%1) ===").arg(exe));
        pushBreakpointsToSession();
        m_gdb.runExec();
        emit stateChanged(true, true);
    } else if (m_gdb.inferiorRunning()) {
        status(tr("Ja em execucao (use F10/F11/Shift+F11 ou aguarde)."));
    } else {
        m_gdb.continueExec();
    }
}

void DebugController::onStop()
{
    if (!m_gdb.sessionActive())
        return;
    if (m_gdb.inferiorRunning()) {
        m_stopping = true;
        m_gdb.interrupt();
        status(tr("Interrompendo..."));
    } else {
        m_gdb.shutdown();
    }
}

void DebugController::onStepOver()
{
    if (m_gdb.sessionActive() && !m_gdb.inferiorRunning())
        m_gdb.stepOver();
}

void DebugController::onStepInto()
{
    if (m_gdb.sessionActive() && !m_gdb.inferiorRunning())
        m_gdb.stepInto();
}

void DebugController::onStepOut()
{
    if (m_gdb.sessionActive() && !m_gdb.inferiorRunning())
        m_gdb.stepOut();
}

void DebugController::onFileClosed(const QString& filePath)
{
    if (filePath.isEmpty())
        return;
    if (m_gdb.sessionActive()) {
        const auto map = m_breakpoints.value(filePath);
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (it.value().number > 0)
                m_gdb.deleteBreakpoint(it.value().number);
        }
    }
    m_breakpoints.remove(filePath);
    refreshBreakpointDock();
}

void DebugController::onRunningChanged(bool running)
{
    // Seta de execucao fica ate o proximo *stopped; o MainWindow precisa
    // saber se esta rodando para liberar/bloquear steps e continuar.
    emit stateChanged(true, running);
}

void DebugController::onStopped(const QString& reason, const debug::MiFrame& frame)
{
    // Programa terminou (exited-normally/exited-signalled): nao ha quadros
    // para listar; encerra o gdb para "fechar" a sessao de depuracao.
    if (reason.startsWith(QLatin1String("exited"))) {
        if (reason == QLatin1String("exited-signalled"))
            writeConsole(tr("O programa terminou (sinal)."));
        else
            writeConsole(tr("O programa encerrou normalmente."));
        emit debugConsoleWanted();
        clearExecutionPosition();
        m_gdb.shutdown();
        return;
    }

    writeConsole(tr("Parado em %1 (%2:%3)")
                     .arg(frame.func, frame.file).arg(qMax(0, frame.line)));
    emit debugConsoleWanted();

    const QString file = !frame.fullname.isEmpty() ? frame.fullname : frame.file;
    setExecutionPosition(file, frame.line);

    if (m_stopping) {
        m_stopping = false;
        m_gdb.shutdown();
        return;
    }

    // Ilustra stack, locais e watches do quadro atual.
    m_haveLocalTypes = false;
    m_haveLocalValues = false;
    m_localTypes.clear();
    m_localValues.clear();
    m_gdb.queryStack();
    m_gdb.queryLocals();
    refreshWatches();
}

void DebugController::onStackUpdated(const QVector<debug::MiFrame>& frames)
{
    m_dock->setCallStack(frames);
}

void DebugController::onLocalsTypesUpdated(const QString& func,
                                           const QVector<debug::MiVar>& vars)
{
    m_localsFunc = func;
    m_localTypes.clear();
    for (const auto& v : vars)
        m_localTypes.insert(v.name, v.type);
    m_haveLocalTypes = true;
    tryShowLocals(func);
}

void DebugController::onLocalsValuesUpdated(const QString& func,
                                            const QVector<debug::MiVar>& vars)
{
    m_localsFunc = func;
    m_localValues.clear();
    for (const auto& v : vars)
        m_localValues.insert(v.name, v.value);
    m_haveLocalValues = true;
    tryShowLocals(func);
}

void DebugController::tryShowLocals(const QString& func)
{
    if (!m_haveLocalTypes || !m_haveLocalValues)
        return;
    // Uniao dos nomes (nas duas respostas), mesclando tipo+valor.
    QSet<QString> names;
    const auto tk = m_localTypes.keys();
    for (const QString& k : tk)
        names.insert(k);
    const auto vk = m_localValues.keys();
    for (const QString& k : vk)
        names.insert(k);
    QStringList sorted = names.values();
    std::sort(sorted.begin(), sorted.end());

    QVector<debug::MiVar> merged;
    merged.reserve(sorted.size());
    for (const auto& n : qAsConst(sorted)) {
        debug::MiVar v;
        v.name = n;
        v.type = m_localTypes.value(n);
        v.value = m_localValues.value(n);
        merged.append(v);
    }
    m_dock->setLocals(func, merged);
    // Consumo das respostas; estado reiniciado no proximo *stopped.
    m_haveLocalTypes = false;
    m_haveLocalValues = false;
}

void DebugController::refreshWatchDisplay()
{
    m_dock->setWatchExpressions(m_watches);
}

void DebugController::refreshWatches()
{
    if (!m_watches.isEmpty() && m_gdb.sessionActive() && !m_gdb.inferiorRunning()) {
        for (const QString& expr : qAsConst(m_watches))
            m_gdb.evaluate(expr);
    }
}

void DebugController::addWatch(const QString& expr)
{
    const QString e = expr.trimmed();
    if (e.isEmpty() || m_watches.contains(e))
        return;
    m_watches.append(e);
    refreshWatchDisplay();
    if (m_gdb.sessionActive() && !m_gdb.inferiorRunning())
        m_gdb.evaluate(e);
}

void DebugController::removeWatch(const QString& expr)
{
    m_watches.removeAll(expr);
    refreshWatchDisplay();
}

void DebugController::clearWatches()
{
    m_watches.clear();
    refreshWatchDisplay();
}

void DebugController::onAddToWatchRequested(const QString& expr)
{
    addWatch(expr);
}

void DebugController::onEvaluated(const QString& expr, const QString& value,
                                  bool ok)
{
    if (m_watches.contains(expr))
        m_dock->setWatchValue(expr,
                              ok ? value : QStringLiteral("(indisponivel)"));
    showTooltipIfPending(expr, ok ? value : QString());
}

void DebugController::onHoverWord(const QString& word, const QPoint& globalPos)
{
    // So consulta valores quando a sessao existe e esta parada.
    if (word.isEmpty() || !m_gdb.sessionActive() || m_gdb.inferiorRunning())
        return;
    m_tooltipWord = word;
    m_tooltipPos = globalPos;
    m_gdb.evaluate(word);
}

void DebugController::showTooltipIfPending(const QString& expr,
                                           const QString& value)
{
    if (expr != m_tooltipWord)
        return;
    m_tooltipWord.clear();
    if (!m_gdb.sessionActive() || m_gdb.inferiorRunning())
        return;
    QToolTip::showText(m_tooltipPos,
                       value.isEmpty() ? tr("(indisponivel no escopo)")
                                       : value,
                       m_tabs->currentWidget());
}

void DebugController::onBreakpointInserted(const QString& file, int line,
                                           int number)
{
    auto it = m_breakpoints.find(file);
    if (it == m_breakpoints.end())
        return;
    auto lit = it.value().find(line);
    if (lit == it.value().end())
        return;
    Bp& bp = lit.value();
    bp.number = number;
    // Breakpoint desativado: mantem registrado mas inativo no gdb.
    if (!bp.enabled)
        m_gdb.setBreakpointEnabled(number, false);
    refreshBreakpointDock();
}

void DebugController::refreshBreakpointDock()
{
    QVector<ui::BreakpointEntry> entries;
    for (auto it = m_breakpoints.constBegin(); it != m_breakpoints.constEnd(); ++it) {
        const QString fileName = QFileInfo(it.key()).fileName();
        for (auto lit = it.value().constBegin(); lit != it.value().constEnd(); ++lit) {
            ui::BreakpointEntry e;
            e.file = it.key();
            e.label = QStringLiteral("%1:%2").arg(fileName).arg(lit.key() + 1);
            e.line = lit.key();
            e.enabled = lit.value().enabled;
            entries.append(e);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const ui::BreakpointEntry& a, const ui::BreakpointEntry& b) {
                  return a.label < b.label;
              });
    m_dock->setBreakpointsList(entries);
}

void DebugController::onBreakpointInsertFailed(const QString& file, int line)
{
    Q_UNUSED(file);
    Q_UNUSED(line);
    writeConsole(tr("Falha ao inserir breakpoint (o arquivo foi compilado?)."));
}

void DebugController::onConsoleOutput(const QString& text)
{
    writeConsole(text);
}

void DebugController::onError(const QString& text)
{
    writeConsole(tr("[gdb] %1").arg(text));
}

void DebugController::onExited(int exitCode)
{
    writeConsole(tr("=== Depuracao encerrada (gdb saiu, codigo %1) ===")
                     .arg(exitCode));
    clearExecutionPosition();
    m_dock->clearAll();
    m_dock->clearWatchValues();
    m_stopping = false;
    m_tooltipWord.clear();
    emit stateChanged(false, false);
    emit debugConsoleWanted();
}

void DebugController::setExecutionPosition(const QString& file, int line)
{
    if (file.isEmpty() || line < 0) {
        clearExecutionPosition();
        return;
    }
    const int idx = m_tabs->openFile(file);
    if (idx >= 0) {
        m_tabs->setCurrentIndex(idx);
        auto* editor = m_tabs->editorAt(idx);
        if (editor) {
            if (line > 0)
                editor->gotoLine(line - 1);
            editor->setExecutionLine(line - 1);
        }
    }
    m_currentFile = file;
    m_currentLine = line;
}

void DebugController::clearExecutionPosition()
{
    for (int i = 0; i < m_tabs->count(); ++i)
        m_tabs->editorAt(i)->setExecutionLine(-1);
    m_currentFile.clear();
    m_currentLine = -1;
}

void DebugController::onCallStackActivated(const QString& file, int line)
{
    setExecutionPosition(file, line);
}

void DebugController::writeConsole(const QString& text)
{
    if (m_output)
        m_output->append(ui::OutputWindow::Pane::DebugConsole,
                         text + QLatin1Char('\n'));
}

void DebugController::status(const QString& message)
{
    if (m_status)
        m_status->showMessage(message, 4000);
}

}  // namespace debug