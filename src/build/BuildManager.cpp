#include "build/BuildManager.hpp"

#include "build/ConanManager.hpp"
#include "build/MakefileGenerator.hpp"

#include "project/Project.hpp"
#include "ui/CodeEditorWidget.hpp"
#include "ui/EditorTabWidget.hpp"
#include "ui/OutputWindow.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QtGlobal>

#include <QDebug>

namespace build {

namespace {

//! Escapa texto para exibicao segura em appendHtml().
QString htmlEscape(const QString& s)
{
    QString out = s;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return out;
}

//! GCC/Clang:  file(line:col): error: msg  |  file(line): warning: msg
const QRegularExpression& gccRegex()
{
    static const QRegularExpression re(
        QStringLiteral("^(.+?):(\\d+):(\\d+):\\s*(fatal error|error|warning):\\s?(.*)$"));
    return re;
}
const QRegularExpression& gccRegexNoCol()
{
    static const QRegularExpression re(
        QStringLiteral("^(.+?):(\\d+):\\s*(fatal error|error|warning):\\s?(.*)$"));
    return re;
}

}  // namespace

BuildManager::BuildManager(ui::OutputWindow* output, ui::EditorTabWidget* tabs,
                           QObject* parent)
    : QObject(parent)
    , m_output(output)
    , m_tabs(tabs)
{
}

void BuildManager::build(const proj::Project& project, const QString& config)
{
    if (isBusy())
        return;
    m_project = project;
    m_config = config;
    m_runAfterBuild = false;
    runMake(m_project, config, QStringLiteral("all"));
}

void BuildManager::rebuild(const proj::Project& project, const QString& config)
{
    if (isBusy())
        return;
    m_project = project;
    m_config = config;
    m_runAfterBuild = false;
    runMake(m_project, config, QStringLiteral("rebuild"));
}

void BuildManager::clean(const proj::Project& project, const QString& config)
{
    if (isBusy())
        return;
    m_project = project;
    m_config = config;
    m_runAfterBuild = false;
    runMake(m_project, config, QStringLiteral("clean"));
}

void BuildManager::runTarget(const proj::Project& project, const QString& config)
{
    if (isBusy())
        return;
    m_project = project;
    m_config = config;
    m_runAfterBuild = true;
    runMake(m_project, config, QStringLiteral("all"));
}

void BuildManager::conanInstall(const proj::Project& project)
{
    if (isBusy())
        return;
    m_project = project;
    m_runAfterBuild = false;

    const QString conanfile = ConanManager::writeConanfile(m_project);
    m_output->activatePane(ui::OutputWindow::Pane::ConanLogs);
    m_output->clear(ui::OutputWindow::Pane::ConanLogs);
    m_output->append(ui::OutputWindow::Pane::ConanLogs,
                     QObject::tr("== conan install ==") + QLatin1String("\n"));

    if (conanfile.isEmpty()) {
        m_output->append(ui::OutputWindow::Pane::ConanLogs,
                         QObject::tr("Projeto sem dependencias declaradas.\n"));
        return;
    }

    m_conanProc = new QProcess(this);
    m_conanProc->setWorkingDirectory(project.rootDir);
    m_conanProc->setProcessChannelMode(QProcess::MergedChannels);
    m_conanProc->setProgram(QStringLiteral("conan"));
    m_conanProc->setArguments(
        {QStringLiteral("install"), QStringLiteral("."),
         QStringLiteral("--output-folder=build/conan")});
    connect(m_conanProc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_conanProc)
            return;
        const QByteArray data = m_conanProc->readAllStandardOutput();
        m_output->append(ui::OutputWindow::Pane::ConanLogs,
                         QString::fromUtf8(data));
    });
    connect(m_conanProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BuildManager::onConanFinished);
    m_state = State::Building;
    m_conanProc->start();
}

void BuildManager::runMake(const proj::Project& project, const QString& config,
                           const QString& makeTarget)
{
    if (!MakefileGenerator::writeToDisk(project)) {
        m_output->activatePane(ui::OutputWindow::Pane::BuildOutput);
        m_output->appendHtml(ui::OutputWindow::Pane::BuildOutput,
                             QStringLiteral("<font color=\"#C00000\">%1</font>")
                                 .arg(QObject::tr("Falha ao gravar o Makefile.")));
        return;
    }

    m_state = State::Building;
    m_output->clear(ui::OutputWindow::Pane::BuildOutput);
    m_output->clear(ui::OutputWindow::Pane::ErrorList);
    m_output->setErrors({});
    m_errorCount = 0;
    m_warningCount = 0;
    m_entries.clear();
    m_lineBuffer.clear();
    m_output->activatePane(ui::OutputWindow::Pane::BuildOutput);

    QStringList args{QStringLiteral("-C"), project.rootDir, makeTarget,
                     QStringLiteral("CONFIG=%1").arg(config)};
    m_makeProc = new QProcess(this);
    m_makeProc->setWorkingDirectory(project.rootDir);
    m_makeProc->setProcessChannelMode(QProcess::MergedChannels);
    m_makeProc->setProgram(QStringLiteral("make"));
    m_makeProc->setArguments(args);
    connect(m_makeProc, &QProcess::readyReadStandardOutput, this, [this]() {
        drainOutput(m_makeProc, ui::OutputWindow::Pane::BuildOutput);
    });
    connect(m_makeProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BuildManager::onMakeFinished);
    m_makeProc->start();
}

void BuildManager::drainOutput(QProcess* proc, ui::OutputWindow::Pane pane)
{
    Q_UNUSED(pane);
    if (!proc)
        return;
    const QByteArray data = proc->readAllStandardOutput();
    m_lineBuffer += data;

    int nl = 0;
    while ((nl = m_lineBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_lineBuffer.left(nl));
        m_lineBuffer.remove(0, nl + 1);
        handleBuildLine(line);
    }
}

void BuildManager::handleBuildLine(const QString& line)
{
    if (line.isEmpty())
        return;

    QRegularExpressionMatch m = gccRegex().match(line);
    const bool hasCol = m.hasMatch();
    if (!hasCol)
        m = gccRegexNoCol().match(line);

    if (m.hasMatch()) {
        const QString file = m.captured(1);  // ex.: src/main.cpp
        const int srcLine = m.captured(2).toInt();
        const int col = hasCol ? m.captured(3).toInt() : 0;
        const QString kind = m.captured(hasCol ? 4 : 3);
        QString msg = m.captured(hasCol ? 5 : 4);
        const bool isError = kind.contains(QLatin1String("error"));

        if (msg.trimmed().isEmpty())
            msg = kind;  // gcc as vezes reporta so "error:" p/ linha seguinte

        QString absFile = file;
        if (m_project.isValid() && QFileInfo(file).isRelative())
            absFile = QDir(m_project.rootDir).filePath(file);

        m_entries.push_back({absFile, srcLine, col, isError, msg});
        if (isError)
            ++m_errorCount;
        else
            ++m_warningCount;

        const QString color = isError ? QStringLiteral("#C00000")
                                      : QStringLiteral("#C06200");
        m_output->appendHtml(ui::OutputWindow::Pane::BuildOutput,
                             QStringLiteral("<font color=\"%1\">%2</font>")
                                 .arg(color, htmlEscape(line)));
        return;
    }

    // Linhas de comando do make ficam em cinza, o resto em preto.
    const bool isMakeCommand = line.startsWith('\t') ||
                               line.startsWith(QLatin1String("make[")) ||
                               line.startsWith(QLatin1String("g++")) ||
                               line.startsWith(QLatin1String("mkdir"));
    const QString color = isMakeCommand ? QStringLiteral("#808080")
                                        : QStringLiteral("#000000");
    m_output->appendHtml(ui::OutputWindow::Pane::BuildOutput,
                         QStringLiteral("<font color=\"%1\">%2</font>")
                             .arg(color, htmlEscape(line)));
}

void BuildManager::applyErrorMarkers()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* ed = m_tabs->editorAt(i);
        if (!ed || ed->filePath().isEmpty())
            continue;
        const QFileInfo ei(ed->filePath());
        QVector<std::pair<int, QString>> lines;
        for (const auto& e : m_entries) {
            if (QFileInfo(e.file) == ei)
                lines.push_back({qMax(0, e.line - 1), e.message});
        }
        if (lines.isEmpty())
            ed->clearCompilerErrors();
        else
            ed->setCompilerErrors(lines);
    }
}

void BuildManager::finalizeBuild(bool ok)
{
    m_output->setErrors(m_entries);
    if (m_errorCount > 0)
        m_output->activatePane(ui::OutputWindow::Pane::ErrorList);
    else
        m_output->activatePane(ui::OutputWindow::Pane::BuildOutput);

    m_output->appendHtml(ui::OutputWindow::Pane::BuildOutput,
                         QStringLiteral("<br><font color=\"#000080\">%1</font>")
                             .arg(htmlEscape(ok
                                  ? QObject::tr("Build concluido")
                                  : QObject::tr("Build contem erros"))));

    emit buildFinished(ok, m_errorCount, m_warningCount);
    m_state = State::Idle;

    if (m_runAfterBuild) {
        m_runAfterBuild = false;
        if (ok && m_project.isValid())
            launchTarget(m_project);
    }
}

void BuildManager::onMakeFinished(int exitCode, QProcess::ExitStatus status)
{
    // Linhas finais sem newline ainda na fila.
    if (!m_lineBuffer.isEmpty()) {
        handleBuildLine(QString::fromUtf8(m_lineBuffer).trimmed());
        m_lineBuffer.clear();
    }

    const bool ok = (status == QProcess::NormalExit && exitCode == 0);
    finalizeBuild(ok);
}

void BuildManager::onRunProcessFinished(int exitCode, QProcess::ExitStatus)
{
    m_output->append(ui::OutputWindow::Pane::DebugConsole,
                     QObject::tr("\n[processo encerrado, codigo %1]\n")
                         .arg(exitCode));
    emit runFinished(exitCode);
    m_state = State::Idle;
}

void BuildManager::onConanFinished(int exitCode, QProcess::ExitStatus)
{
    m_output->append(ui::OutputWindow::Pane::ConanLogs,
                     exitCode == 0
                         ? QObject::tr("conan install concluido.\n")
                         : QObject::tr("conan install falhou (exit %1).\n")
                               .arg(exitCode));
    m_output->activatePane(ui::OutputWindow::Pane::ConanLogs);
    if (m_project.isValid())
        MakefileGenerator::writeToDisk(m_project);
    m_state = State::Idle;
}

void BuildManager::launchTarget(const proj::Project& project)
{
    m_state = State::Running;
    const QString exe =
        QDir(project.rootDir).filePath(QStringLiteral("build/%1").arg(project.makeTarget));
    if (!QFile::exists(exe)) {
        m_output->append(ui::OutputWindow::Pane::DebugConsole,
                         QObject::tr("Executavel nao encontrado: %1\n").arg(exe));
        m_state = State::Idle;
        return;
    }

    m_output->activatePane(ui::OutputWindow::Pane::DebugConsole);
    m_output->append(ui::OutputWindow::Pane::DebugConsole,
                     QObject::tr("== %1 ==\n").arg(exe));
    m_lineBuffer.clear();

    m_runProc = new QProcess(this);
    m_runProc->setWorkingDirectory(project.rootDir);
    m_runProc->setProcessChannelMode(QProcess::MergedChannels);
    m_runProc->setProgram(exe);
    connect(m_runProc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_runProc)
            return;
        const QByteArray data = m_runProc->readAllStandardOutput();
        m_output->append(ui::OutputWindow::Pane::DebugConsole,
                         QString::fromUtf8(data));
    });
    connect(m_runProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BuildManager::onRunProcessFinished);
    m_runProc->start();
}

}  // namespace build