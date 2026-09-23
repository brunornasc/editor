#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace core {

//! Assinatura de um arquivo incluido no scan (p/ validacao do cache).
//! (tipo interno/documentacao; usado por KeywordIndex e pelos helpers do .cpp)
struct DepStamp {
    QString path;
    qint64 mtime = 0;
    qint64 size = 0;
};

//! Palavras-chave de autocompletar por linguagem (intellisense indexado).
//!
//! Duas fontes, configuráveis:
//!  1. **Default por linguagem**: a extensao do arquivo aberto escolhe a
//!     linguagem; a lista base vem de um arquivo proprio dela:
//!     override do usuario em `~/.config/<app>/keywords/<lang>.txt`, senao o
//!     recurso embutido `:/keywords/<lang>.txt`. Basta criar o arquivo para
//!     personalizar/estender.
//!  2. **Scan de includes**: processa em thread (assincrono) o arquivo e os
//!     cabeçalhos que ele inclui (#include), indexa identificadores
//!     frequentes e guarda o resultado num cache em pasta oculta
//!     (`~/.config/<app>/keywords/cache/`), ligado ao arquivo principal por
//!     um JSON (`index.json`). Cache valido reabre instantaneo; o editor
//!     mescla default + scan no QsciAPIs de cada documento.
class KeywordIndex final : public QObject {
    Q_OBJECT

public:
    //! Instancia unica (caches e fila compartilhados entre documentos).
    static KeywordIndex* instance();

    //! Linguagem do arquivo pela extensao ("cpp", "sql", "asm", ...; "" = nenhuma).
    static QString languageForFile(const QString& filePath);
    //! Se a linguagem usa #include a escanear (C/C++ por enquanto).
    static bool scansIncludes(const QString& language);

    //! Lista base da linguagem (arquivo proprio por linguagem, sem scan).
    QStringList defaultKeywords(const QString& language) const;

    //! Cache valido do arquivo (leitura instantanea); vazio se inexistente/stale.
    QStringList cachedKeywordsFor(const QString& filePath) const;

    //! Escaneia o arquivo como o QThreadPool faria e grava o cache.
    //! (sincrono; reusa o cache quando ainda valido).
    static QStringList scanAndCache(const QString& filePath,
                                    const QString& language);

    //! Agenda o scan assincrono do arquivo (resultado via scanReady).
    void requestScan(const QString& filePath, const QString& language);

    //! Diretorio do indice/keywords (pasta oculta/configuravel).
    static QString keywordsDir();

signals:
    //! Scan concluido (sempre emitido na thread GUI).
    void scanReady(const QString& filePath, const QString& language,
                   const QStringList& keywords);

private slots:
    //! Recebe o resultado do worker e reemite como scanReady.
    void onScanFinished(const QString& filePath, const QString& language,
                        const QStringList& keywords);

private:
    explicit KeywordIndex(QObject* parent = nullptr);

    //! Testa se o cache do arquivo ainda bate com mtime/size dos deps.
    static bool isCacheValid(const QString& filePath, const QString& language);
    //! Le/grava index.json (serializado por mutex).
    static void saveIndexEntry(const QString& filePath, const QString& language,
                               const QStringList& keywords,
                               const QList<DepStamp>& depStamps);
    //! Tokens ja na lista base (nao repetir nem no cache nem no popup).
    mutable QHash<QString, QSet<QString>> m_baseWords;
    mutable QRecursiveMutex m_baseMutex;
    static QMutex m_ioMutex;        // serializa index.json/cache entre threads
    QMutex m_inFlightMutex;
    QSet<QString> m_inFlight;  // "path\flang" sendo processado
};

}  // namespace core