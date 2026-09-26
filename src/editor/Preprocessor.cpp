// Preprocessor.cpp
#include "editor/Preprocessor.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace editor {
namespace {

//! "# 306 \"bits/c++config.h\" 3" -- aviso do gcc de que as proximas linhas
//! vem de outro arquivo (e' o unico lugar onde a saida se descola do original).
const QRegularExpression& linemarker()
{
    static const QRegularExpression re(
        QStringLiteral("^#\\s*(\\d+)\\s+\"([^\"]*)\""));
    return re;
}

}  // namespace

Preprocessor::Preprocessor(QString compiler)
    : m_compiler(std::move(compiler))
{
}

PreprocessResult Preprocessor::run(const QString& filePath) const
{
    PreprocessResult res;
    if (m_compiler.isEmpty() || filePath.isEmpty())
        return res;

    // -C mantem os comentarios: a "signature" dos simbolos no outline mostra a
    // linha original do cabecalho e, sem comentarios, viraria lixo.
    QStringList args = {QStringLiteral("-E"), QStringLiteral("-C")};
    args += m_flags;
    args += {QStringLiteral("-x"), QStringLiteral("c++"), filePath};

    QProcess proc;
    proc.start(m_compiler, args);
    if (!proc.waitForStarted(5000)) {
        res.error = QStringLiteral("nao foi possivel executar %1: %2")
                        .arg(m_compiler, proc.errorString());
        return res;
    }
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        proc.waitForFinished(1000);
        res.error = QStringLiteral("timeout preprocessando %1").arg(filePath);
        return res;
    }

    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    if (out.isEmpty()) {
        res.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (res.error.isEmpty())
            res.error = QStringLiteral("saida vazia do preprocessador");
        return res;
    }

    // --- mapa de linhas ---------------------------------------------------
    QString text = out;
    const QString base = QFileInfo(filePath).absolutePath();
    QString currentFile = QFileInfo(filePath).absoluteFilePath();
    int currentLine = 1;  // 1-based, como o gcc escreve nos marcadores

    res.map.files.reserve(out.count(QLatin1Char('\n')) + 1);
    res.map.lines.reserve(out.count(QLatin1Char('\n')) + 1);

    // O marcador vira espacos, sem apagar a linha: `# 44 "f" 3` nao e C++ valido
    // e a gramatica faz `ERROR` nele -- inclusive no meio de um `namespace std`,
    // entre o nome e a `{`, o que joga miles de simbolos para fora do `std`.
    // O SourceMap ja guardou a informacao do marcador antes.
    QChar* data = text.data();

    int pos = 0;
    const int n = text.size();
    while (pos <= n) {
        const int eol = out.indexOf(QLatin1Char('\n'), pos);
        const QString lineText = out.mid(pos, (eol < 0 ? n : eol) - pos);
        pos = (eol < 0 ? n + 1 : eol + 1);

        const QRegularExpressionMatch m = linemarker().match(lineText);
        if (m.hasMatch()) {
            currentLine = m.captured(1).toInt();
            const QString f = m.captured(2);
            // caminhos absolutos vem com as aspas; relativos sao relativos ao
            // diretorio do arquivo preprocessado
            currentFile = f.isEmpty() || QFileInfo(f).isAbsolute()
                              ? f
                              : QDir(base).absoluteFilePath(f);
            // a propria linha do marcador nao pertence a nenhum arquivo util
            res.map.files.append(QString());
            res.map.lines.append(-1);
            for (int k = pos - (lineText.size() + 1); k < pos; ++k) {
                if (k >= 0 && data[k] != QLatin1Char('\n'))
                    data[k] = QLatin1Char(' ');
            }
            continue;
        }

        res.map.files.append(currentFile);
        res.map.lines.append(currentLine - 1);
        ++currentLine;
    }

    res.text = text;
    res.ok = true;
    return res;
}

QStringList Preprocessor::includePaths() const
{
    if (m_compiler.isEmpty())
        return {};

    QStringList args = {QStringLiteral("-E"), QStringLiteral("-v"), QStringLiteral("-x"),
                        QStringLiteral("c++"), QStringLiteral("-")};
    args += m_flags;

    QProcess proc;
    proc.start(m_compiler, args);
    if (!proc.waitForStarted(5000))
        return {};
    proc.closeWriteChannel();
    if (!proc.waitForFinished(20000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }

    const QString err = QString::fromUtf8(proc.readAllStandardError());
    QStringList dirs;
    bool collecting = false;
    const QStringList lines = err.split(QLatin1Char('\n'));
    for (const QString& l : lines) {
        if (l.startsWith(QLatin1String("#include <...> search starts here:"))) {
            collecting = true;
            continue;
        }
        if (l.startsWith(QLatin1String("End of search list."))) {
            collecting = false;
            continue;
        }
        if (!collecting)
            continue;
        const QString d = l.trimmed();
        // a primeira e' o diretorio do proprio arquivo (entrada "-")
        if (d.isEmpty() || d == QLatin1String(".") || d == QLatin1String(".."))
            continue;
        dirs.append(d);
    }
    return dirs;
}

QStringList Preprocessor::searchPaths(const QString& filePath) const
{
    QStringList dirs;
    const QString own = QFileInfo(filePath).absolutePath();
    if (!own.isEmpty())
        dirs.append(own);
    // as flags do projeto podem ter -I; o gcc ja as devolve em includePaths()
    for (const QString& d : includePaths()) {
        if (!dirs.contains(d))
            dirs.append(d);
    }
    return dirs;
}

}  // namespace editor
