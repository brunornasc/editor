#include "lsp/LspClient.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace lsp {

namespace {

//! Teto defensivo: um Content-Length absurdo indica stream corrompido, e
//! alocar ele inteiro mataria o editor.
constexpr qint64 kMaxFrameBytes = 64 * 1024 * 1024;

QJsonObject completionCaps()
{
    // Só o que a UI realmente sabe aplicar. Preferimos item simples (textEdit
    // com o texto) a snippet: o Qsci insere texto, nao expande placeholder.
    QJsonObject item;
    item.insert(QStringLiteral("snippetSupport"), false);
    item.insert(QStringLiteral("documentationFormat"),
                QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")});
    item.insert(QStringLiteral("labelDetailsSupport"), true);
    item.insert(QStringLiteral("insertReplaceSupport"), false);

    QJsonObject completion;
    completion.insert(QStringLiteral("completionItem"), item);

    QJsonObject hover;
    hover.insert(QStringLiteral("contentFormat"),
                 QJsonArray{QStringLiteral("markdown"), QStringLiteral("plaintext")});

    QJsonObject sync;
    sync.insert(QStringLiteral("didSave"), true);
    sync.insert(QStringLiteral("dynamicRegistration"), false);

    QJsonObject diagnostics;
    diagnostics.insert(QStringLiteral("relatedInformation"), true);
    diagnostics.insert(QStringLiteral("versionSupport"), true);
    diagnostics.insert(QStringLiteral("tagSupport"),
                       QJsonObject{{QStringLiteral("valueSet"),
                                    QJsonArray{1, 2}}});  // Unnecessary, Deprecated

    QJsonObject textDocument;
    textDocument.insert(QStringLiteral("synchronization"), sync);
    textDocument.insert(QStringLiteral("completion"), completion);
    textDocument.insert(QStringLiteral("hover"), hover);
    textDocument.insert(QStringLiteral("publishDiagnostics"), diagnostics);

    QJsonObject caps;
    caps.insert(QStringLiteral("textDocument"), textDocument);

    QJsonObject window;
    window.insert(QStringLiteral("workDoneProgress"), true);
    window.insert(QStringLiteral("showMessage"), QJsonObject());
    caps.insert(QStringLiteral("window"), window);

    // `configuration: true` faz o servidor nos perguntar as settings; se
    // declarassemos que nao sabemos, o clangd ignoraria um .clangd do projeto.
    QJsonObject workspace;
    workspace.insert(QStringLiteral("configuration"), true);
    workspace.insert(QStringLiteral("workspaceFolders"), false);
    workspace.insert(QStringLiteral("didChangeConfiguration"),
                     QJsonObject{{QStringLiteral("dynamicRegistration"), true}});
    caps.insert(QStringLiteral("workspace"), workspace);
    return caps;
}

}  // namespace

LspClient::LspClient(const QString& languageId, QObject* parent)
    : QObject(parent), m_languageId(languageId)
{
}

LspClient::~LspClient()
{
    stop();
}

QString LspClient::findProgram(const QString& name, QString* tried)
{
    QStringList seen;
    const auto remember = [&](const QString& p) {
        if (!p.isEmpty() && !seen.contains(p))
            seen << p;
    };

    // 1) PATH
    const QString fromPath = QStandardPaths::findExecutable(name);
    remember(fromPath);
    if (!fromPath.isEmpty())
        return fromPath;

    // 2) Versoes explicitas (clangd-18, clangd-17...), do mais novo pro mais
    //    antigo: e o que o apt costuma instalar sem link em /usr/bin.
    for (int minor = 22; minor >= 12; --minor) {
        const QString cand = QStandardPaths::findExecutable(
            QStringLiteral("clangd-%1").arg(minor));
        remember(cand);
        if (!cand.isEmpty() && name == QLatin1String("clangd"))
            return cand;
    }

    // 3) LLVM instalado em /usr/lib/llvm-*/bin (sem root, sem link).
    const QDir lib(QStringLiteral("/usr/lib"));
    QStringList llvmDirs;
    for (const QString& entry : lib.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (entry.startsWith(QLatin1String("llvm-")))
            llvmDirs << entry;
    // Nome inverso: llvm-22 > llvm-9, entao ordena decrescente.
    std::sort(llvmDirs.begin(), llvmDirs.end(), [](const QString& a, const QString& b) {
        return a.section(QLatin1Char('-'), 1).toInt() > b.section(QLatin1Char('-'), 1).toInt();
    });
    for (const QString& d : llvmDirs) {
        const QString cand = QDir(QStringLiteral("/usr/lib/") + d + QStringLiteral("/bin/"))
                                 .filePath(name);
        remember(cand);
        if (QFileInfo::exists(cand))
            return cand;
    }

    if (tried)
        *tried = seen.join(QLatin1String(", "));
    return QString();
}

QString LspClient::toUri(const QString& path)
{
    QString p = QDir::cleanPath(path);
    if (!p.startsWith(QLatin1Char('/')))
        p.prepend(QLatin1Char('/'));  // file:// exige caminho absoluto
    // Escapa o que o RFC 3986 proibe em um path de file, sem tocar no "/".
    p.replace(QLatin1Char('%'), QStringLiteral("%25"));
    static const QRegularExpression re(QStringLiteral("[?#\\[\\]@!$&'()*+,;=: ]"));
    int last = 0;
    QString out;
    auto m = re.globalMatch(p);
    while (m.hasNext()) {
        const auto one = m.next();
        out += p.mid(last, one.capturedStart() - last);
        // Nao usar QStringLiteral("%%%1").arg(): em arg(), "%%" nao e escape e
        // sairia "%%20" em vez de "%20".
        out += QLatin1Char('%');
        out += one.captured().toLatin1().toHex().toUpper();
        last = one.capturedEnd();
    }
    out += p.mid(last);
    return QStringLiteral("file://") + out;
}

QString LspClient::fromUri(const QString& uri)
{
    QString p = uri;
    if (p.startsWith(QLatin1String("file://")))
        p = p.mid(7);
    // Todos os escapes %XX (inclusive %20 = espaco) sao tratados pelo regex
    // abaixo; nao ha replace antecipado, que deixaria "%" orfao.
    static const QRegularExpression re(QStringLiteral("%([0-9A-Fa-f]{2})"));
    int last = 0;
    QString out;
    auto it = re.globalMatch(p);
    while (it.hasNext()) {
        const auto one = it.next();
        out += p.mid(last, one.capturedStart() - last);
        out += QChar(char(one.captured(1).toInt(nullptr, 16)));
        last = one.capturedEnd();
    }
    out += p.mid(last);
    return out;
}

QJsonObject LspClient::position(int line, int character)
{
    QJsonObject p;
    p.insert(QStringLiteral("line"), line);
    p.insert(QStringLiteral("character"), character);
    return p;
}

QJsonObject LspClient::range(int startLine, int startChar, int endLine, int endChar)
{
    QJsonObject r;
    r.insert(QStringLiteral("start"), position(startLine, startChar));
    r.insert(QStringLiteral("end"), position(endLine, endChar));
    return r;
}

bool LspClient::start(const QString& program, const QStringList& args, const QString& rootPath,
                     QString* error)
{
    if (isRunning()) {
        if (error)
            error->clear();
        return true;
    }
    m_stopping = false;
    m_rootPath = rootPath;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LspClient::onStdoutReady);
    connect(m_process, &QProcess::readyReadStandardError, this, &LspClient::onStderrReady);
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart) {
                    m_running = false;
                    emit terminated(-1, true);
                }
            });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_running = false;
        failAllPending(QStringLiteral("servidor de linguagem terminou"));
        emit terminated(code, !m_stopping);
    });

    m_process->start(program, args);
    if (!m_process->waitForStarted(5000)) {
        if (error) {
            *error = QStringLiteral("nao foi possivel iniciar '%1': %2")
                         .arg(program, m_process->errorString());
        }
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }
    m_running = true;

    // initialize: rootUri aponta a raiz do projeto para o servidor achar o
    // compile_commands.json subindo a arvore.
    QJsonObject params;
    params.insert(QStringLiteral("processId"), QCoreApplication::applicationPid());
    params.insert(QStringLiteral("clientInfo"),
                  QJsonObject{{QStringLiteral("name"), QStringLiteral("Editor")},
                              {QStringLiteral("version"), QStringLiteral("0.1.0")}});
    params.insert(QStringLiteral("capabilities"), completionCaps());
    if (m_rootPath.isEmpty())
        params.insert(QStringLiteral("rootUri"), QJsonValue::Null);
    else
        params.insert(QStringLiteral("rootUri"), toUri(m_rootPath));

    sendRequest(
        QStringLiteral("initialize"), params,
        [this](const QJsonValue& result, const QJsonObject&) {
            m_initializeResult = result.toObject();
            m_capabilities = result.toObject().value(QStringLiteral("capabilities")).toObject();
            m_initialized = true;
            sendNotification(QStringLiteral("initialized"), QJsonObject());
            emit initialized();
        },
        [this](const QJsonValue&, const QJsonObject& err) {
            qWarning("lsp: initialize falhou: %s",
                     qPrintable(err.value(QStringLiteral("message")).toString()));
        });
    if (error)
        error->clear();
    return true;
}

void LspClient::stop()
{
    if (!m_process)
        return;
    m_stopping = true;
    if (m_process->state() != QProcess::NotRunning) {
        // shutdown/exit e o par educado; se o processo nao responder em 1.5 s,
        // terminate() mesmo assim para nao deixar orfao.
        if (m_initialized)
            sendRequest(QStringLiteral("shutdown"), QJsonObject());
        sendNotification(QStringLiteral("exit"), QJsonObject());
        if (!m_process->waitForFinished(1500))
            m_process->terminate();
    }
    m_initialized = false;
    m_running = false;
    failAllPending(QStringLiteral("servidor de linguagem fechado"));
    m_process->deleteLater();
    m_process = nullptr;
    m_stdoutBuf.clear();
    m_stderrBuf.clear();
}

bool LspClient::isRunning() const
{
    return m_running && m_process && m_process->state() != QProcess::NotRunning;
}

void LspClient::writeFrame(const QByteArray& payload)
{
    if (!m_process)
        return;
    m_process->write("Content-Length: " + QByteArray::number(payload.size()) + "\r\n\r\n");
    m_process->write(payload);
}

void LspClient::sendRaw(const QByteArray& payload)
{
    writeFrame(payload);
}

int LspClient::sendRequest(const QString& method, const QJsonObject& params,
                           ReplyHandler onReply, ReplyHandler onError)
{
    const int id = m_nextId++;
    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("id"), id);
    msg.insert(QStringLiteral("method"), method);
    msg.insert(QStringLiteral("params"), params);

    Pending p;
    p.onReply = std::move(onReply);
    p.onError = std::move(onError);
    m_pending.insert(id, p);

    sendRaw(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    return id;
}

void LspClient::sendNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    msg.insert(QStringLiteral("method"), method);
    if (!params.isEmpty())
        msg.insert(QStringLiteral("params"), params);
    sendRaw(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void LspClient::didOpen(const QString& uri, const QString& languageId, const QString& text)
{
    QJsonObject doc;
    doc.insert(QStringLiteral("uri"), uri);
    doc.insert(QStringLiteral("languageId"), languageId);
    doc.insert(QStringLiteral("version"), 1);
    doc.insert(QStringLiteral("text"), text);
    sendNotification(QStringLiteral("textDocument/didOpen"),
                     QJsonObject{{QStringLiteral("textDocument"), doc}});
}

void LspClient::didChange(const QString& uri, int version, const QString& text)
{
    QJsonObject doc;
    doc.insert(QStringLiteral("uri"), uri);
    doc.insert(QStringLiteral("version"), version);
    // Mudanca de corpo inteiro: o mais simples e o que o Qsci ja tem pronto.
    doc.insert(QStringLiteral("text"), text);
    sendNotification(
        QStringLiteral("textDocument/didChange"),
        QJsonObject{{QStringLiteral("textDocument"), doc},
                    {QStringLiteral("contentChanges"), QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}}});
}

void LspClient::didClose(const QString& uri)
{
    sendNotification(QStringLiteral("textDocument/didClose"),
                     QJsonObject{{QStringLiteral("textDocument"),
                                 QJsonObject{{QStringLiteral("uri"), uri}}}});
}

void LspClient::cancel(int id)
{
    if (id <= 0 || !m_pending.contains(id))
        return;
    QJsonObject p;
    p.insert(QStringLiteral("id"), id);
    sendNotification(QStringLiteral("$/cancelRequest"), p);
}

void LspClient::onStdoutReady()
{
    if (!m_process)
        return;
    m_stdoutBuf += m_process->readAllStandardOutput();

    for (;;) {
        const int sep = m_stdoutBuf.indexOf("\r\n\r\n");
        if (sep < 0)
            break;  // ainda nao chegou o header inteiro
        const QByteArray head = m_stdoutBuf.left(sep);
        qint64 len = -1;
        for (const QByteArray& line : head.split('\n')) {
            const QByteArray t = line.trimmed();
            if (t.toLower().startsWith("content-length:"))
                len = t.mid(t.indexOf(':') + 1).trimmed().toLongLong();
        }
        if (len < 0 || len > kMaxFrameBytes) {
            // Header invalido: nao da para ressincronizar, e melhor derrubar o
            // servidor do que acumular lixo e reparsear eternamente.
            emit logLine(QStringLiteral("lsp: header invalido, reiniciando o servidor"));
            stop();
            return;
        }
        if (m_stdoutBuf.size() < sep + 4 + len)
            break;  // corpo incompleto
        const QByteArray body = m_stdoutBuf.mid(sep + 4, int(len));
        m_stdoutBuf.remove(0, sep + 4 + int(len));

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit logLine(QStringLiteral("lsp: json invalido: %1").arg(err.errorString()));
            continue;
        }
        handleMessage(doc.object());
    }
}

void LspClient::onStderrReady()
{
    if (!m_process)
        return;
    m_stderrBuf += m_process->readAllStandardError();
    for (;;) {
        const int nl = m_stderrBuf.indexOf('\n');
        if (nl < 0)
            break;
        const QString line = QString::fromUtf8(m_stderrBuf.left(nl)).trimmed();
        m_stderrBuf.remove(0, nl + 1);
        if (!line.isEmpty())
            emit logLine(line);
    }
}

void LspClient::handleMessage(const QJsonObject& msg)
{
    const bool hasId = msg.contains(QStringLiteral("id"));
    const QString method = msg.value(QStringLiteral("method")).toString();

    if (hasId && !method.isEmpty()) {
        // Request do servidor para nos (registro de capability, config...).
        const QJsonObject id = msg.value(QStringLiteral("id")).toObject();
        const QJsonObject result = handleServerRequest(method, msg.value(QStringLiteral("params")).toObject());
        QJsonObject reply;
        reply.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        reply.insert(QStringLiteral("id"), id);
        if (result.isEmpty())
            reply.insert(QStringLiteral("result"), QJsonValue::Null);
        else
            reply.insert(QStringLiteral("result"), result);
        sendRaw(QJsonDocument(reply).toJson(QJsonDocument::Compact));
        return;
    }

    if (hasId) {
        const int id = msg.value(QStringLiteral("id")).toInt(-1);
        const auto it = m_pending.find(id);
        if (it == m_pending.end())
            return;  // resposta de request ja cancelado/descartado
        Pending p = it.value();
        m_pending.erase(it);
        const QJsonValue result = msg.value(QStringLiteral("result"));
        const QJsonObject err = msg.value(QStringLiteral("error")).toObject();
        if (!err.isEmpty()) {
            if (p.onError)
                p.onError(result, err);
        } else if (p.onReply) {
            p.onReply(result, QJsonObject());
        }
        return;
    }

    if (!method.isEmpty())
        emit notification(method, msg.value(QStringLiteral("params")).toObject());
}

QJsonObject LspClient::handleServerRequest(const QString& method, const QJsonObject&)
{
    if (method == QLatin1String("client/registerCapability")) {
        return QJsonObject();  // null = aceito
    }
    if (method == QLatin1String("client/unregisterCapability"))
        return QJsonObject();
    if (method == QLatin1String("window/workDoneProgress/create")
        || method == QLatin1String("workspace/diagnostic/refresh")
        || method == QLatin1String("window/showDocument"))
        return QJsonObject();
    if (method == QLatin1String("workspace/configuration")) {
        // Uma entrada por item pedido, todas nulas: o servidor usa o default
        // dele (e ainda le o .clangd do projeto).
        return QJsonObject{{QStringLiteral("items"), QJsonArray{}}};
    }
    if (method == QLatin1String("workspace/applyEdit"))
        return QJsonObject{{QStringLiteral("applied"), false}};
    if (method == QLatin1String("window/showMessageRequest")) {
        return QJsonObject{{QStringLiteral("items"), QJsonArray{}}};
    }
    // Metodo desconhecido: null e a resposta valida da spec.
    return QJsonObject();
}

void LspClient::failAllPending(const QString& why)
{
    const auto pending = m_pending;
    m_pending.clear();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        if (!it.value().onError)
            continue;
        QJsonObject err;
        err.insert(QStringLiteral("code"), -1);
        err.insert(QStringLiteral("message"), why);
        it.value().onError(QJsonValue(), err);
    }
}

}  // namespace lsp
