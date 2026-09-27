#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace lsp {

//! Cliente de Language Server Protocol: um processo, JSON-RPC 2.0 sobre stdio.
//!
//! Cuida do que costuma travar uma IDE na primeira tentativa:
//! - o framing `Content-Length` (e nada mais: nao ha separador de linhas);
//! - respostas que chegam fora de ordem e requests que o servidor faz em nos
//!   (`client/registerCapability`, `workspace/configuration`), que precisam de
//!   resposta ou o servidor fica esperando;
//! - requests pendentes quando o processo morre, que precisam falhar em vez de
//!   vazar callbacks para widgets ja destruidos.
//!
//! NAO bloqueia: o QProcess e assincrono e as respostas chegam por sinal.
class LspClient : public QObject {
    Q_OBJECT

public:
    //! `result` = campo "result" (pode ser object OU array: `definition` e
    //! `completion` sem wrapper devolvem lista); `error` = campo "error".
    using ReplyHandler = std::function<void(const QJsonValue& result, const QJsonObject& error)>;

    explicit LspClient(const QString& languageId, QObject* parent = nullptr);
    ~LspClient() override;

    QString languageId() const { return m_languageId; }

    //! Sobe o servidor. `program` e o executavel, `args` os argumentos e
    //! `rootPath` a raiz do projeto (vai no initialize como rootUri, para o
    //! servidor achar o compile_commands.json subindo a arvore).
    //! Devolve false (com `error` preenchido) se nao achou o programa ou o
    //! processo morreu no start.
    bool start(const QString& program, const QStringList& args, const QString& rootPath,
               QString* error = nullptr);

    //! Envia `shutdown` + `exit` e espera o processo encerrar.
    void stop();

    bool isRunning() const;

    //! Procura um executavel: PATH e depois os directorios do LLVM
    //! installed (/usr/lib/llvm-*/bin), onde o clangd costuma ficar sem
    //! link em /usr/bin.
    static QString findProgram(const QString& name, QString* tried = nullptr);

    //! Converte caminho <-> URI file:// (com escapes percent).
    static QString toUri(const QString& path);
    static QString fromUri(const QString& uri);

    //! Linha/coluna do LSP sao 0-based; o resto do editor usa 0-based tambem,
    //! mas o Qsci quer 1-based. Estes helpers deixam a conversao explicita.
    static QJsonObject position(int line, int character);
    static QJsonObject range(int startLine, int startChar, int endLine, int endChar);

    //! Request com id proprio. Devolve o id (para `cancel`).
    int sendRequest(const QString& method, const QJsonObject& params,
                    ReplyHandler onReply = {}, ReplyHandler onError = {});
    void sendNotification(const QString& method, const QJsonObject& params = QJsonObject());
    //! `$/cancelRequest` para o request pendente.
    void cancel(int id);

    // Sincronizacao de documento: o envelope `{"textDocument": {...}}` e
    // obrigatorio (o clangd rejeita os campos achatados com
    // "missing value at (root).textDocument"), entao fica escondido aqui.
    void didOpen(const QString& uri, const QString& languageId, const QString& text);
    //! `version` precisa subir a cada edicao (o clangd ignora versoes repetidas).
    void didChange(const QString& uri, int version, const QString& text);
    void didClose(const QString& uri);

    //! Respostas de initialize (guardadas para o manager ler).
    QJsonObject serverCapabilities() const { return m_capabilities; }
    QJsonObject initializeResult() const { return m_initializeResult; }

signals:
    //! Notificacao do servidor (publishDiagnostics, logMessage, ...).
    void notification(const QString& method, const QJsonObject& params);
    //! Uma linha do stderr do servidor (log para o OutputWindow).
    void logLine(const QString& line);
    //! O processo terminou. `restarting` = quem chamou ja vai subir outro.
    void terminated(int exitCode, bool byCrash);
    //! Resposta de initialize chegou.
    void initialized();

private:
    struct Pending {
        ReplyHandler onReply;
        ReplyHandler onError;
    };

    void onStdoutReady();
    void onStderrReady();
    void handleMessage(const QJsonObject& msg);
    //! Requests que o servidor nos faz (registro de capability, config...).
    QJsonObject handleServerRequest(const QString& method, const QJsonObject& id);
    void failAllPending(const QString& why);
    void sendRaw(const QByteArray& payload);
    void writeFrame(const QByteArray& payload);

    QString m_languageId;
    QProcess* m_process = nullptr;
    QByteArray m_stdoutBuf;
    QByteArray m_stderrBuf;
    int m_nextId = 1;
    QHash<int, Pending> m_pending;
    QJsonObject m_capabilities;
    QJsonObject m_initializeResult;
    QString m_rootPath;
    bool m_initialized = false;
    bool m_running = false;
    bool m_stopping = false;
    //! Metodos que o servidor pediu para registrar via
    //! client/registerCapability: anotamos para o manager saber o queturned
    //! dinâmico (hoje so respondemos, mas fica o gancho).
    QStringList m_clientCapabilities;
};

}  // namespace lsp
