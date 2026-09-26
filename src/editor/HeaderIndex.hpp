// HeaderIndex.hpp
#pragma once

#include "editor/IncludeResolver.hpp"
#include "editor/Preprocessor.hpp"
#include "editor/SymbolTree.hpp"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <memory>

namespace editor {

//! Um simbolo offeredto no popup, ja com o nome que o usuario digita.
struct IndexedName {
    //! "cout" ou "std::cout" (o popup decide se usa o qualificado).
    QString name;
    SymbolKind kind = SymbolKind::Unknown;
    //! Arquivo/linha reais da declaracao (via SourceMap do preprocessador).
    QString file;
    int line = -1;
};

//! Cache de simbolos por header, compartilhado entre os arquivos do projeto.
//!
//! Chavear por include ("<vector>") e nao por arquivo indexado e o que faz o
//! `#include <vector>` em 30 .cpp custar um unico preprocessamento+parse: o
//! `bits/stl_vector.h` entra uma vez so. O conteudo real (a `SymbolTree`) vem
//! do preprocessador, que ja traz tudo que o header puxa.
//!
//! Thread-safe: os workers montam arvores em paralelo e publicam no fim.
class HeaderIndex : public QObject {
    Q_OBJECT

public:
    //! O que torna uma indexacao valida: o preprocessamento depende do
    //! compilador, das flags e do conteudo de TODOS os headers que entraram
    //! (eles vem nos marcadores do `g++ -E`). Sem isso, trocar `-std=c++20`
    //! ou editar um `.h` interno continuaria servindo o cache velho.
    struct CacheStamp {
        QString compiler;
        QStringList flags;
        //! Path real do header indexado (o spec sozinho nao identifica o
        //! arquivo: "utils/log.h" existe em varios projetos).
        QString entryFile;
        //! arquivo -> mtime em ms; inclui o header de entrada e todos os outros.
        QHash<QString, qint64> fileMtimes;
    };

    //! Uma indexacao concluida (compartilhada entre worker e thread da UI).
    struct Entry {
        SymbolTree tree;
        QStringList stdMembers;  // cache dos nomes sob `std`
        CacheStamp stamp;
    };

    static HeaderIndex* instance();

    //! Configuracao de preprocessamento (compilador + flags do projeto).
    void configure(const QString& compiler, const QStringList& flags,
                   const QStringList& projectDirs);
    void setProjectDirs(const QStringList& dirs);

    QString compiler() const { return m_compiler; }
    QStringList flags() const { return m_flags; }

    //! Headers ja indexados para este include (chave normalizada).
    bool isIndexed(const QString& includeSpec) const;
    //! Include specs ja indexados, na ordem em que foram pedidos.
    QStringList indexedIncludes() const;

    //! Include specs pendentes (pedidos e ainda em andamento).
    QStringList pendingIncludes() const;

    //! Agenda a indexacao de um include. Devolve false se ja estava em cache
    //! ou ja estava na fila.
    bool request(const QString& includeSpec, const QString& fromFile);

    //! Simbolos de um include (vazio se ainda nao indexado).
    QVector<IndexedName> names(const QString& includeSpec) const;

    //! Nomes "membros de std" (sem o prefixo `std::`) de um include.
    QStringList stdMembers(const QString& includeSpec) const;

    //! Procura um simbolo por nome qualificado em todos os includes ja
    //! indexados ("std::vector", "std::cout", "MinhaClasse::metodo").
    bool find(const QString& qualifiedName, IndexedName* out) const;

    //! Esvazia o cache em memoria (mantem o cache em disco).
    void clearMemory();

    //! Habilita/desabilita a gravacao do cache em disco no projeto.
    void setCacheDir(const QString& dir) { m_cacheDir = dir; }
    QString cacheDir() const { return m_cacheDir; }

    //! Em que arquivo real esse include foi resolvido (projeto -> includes do
    //! compilador -> relativo ao arquivo que o pediu). Vazio se nao achou.
    QString resolveHeaderPath(const QString& includeSpec, const QString& fromFile) const;

    //! Nome do arquivo de cache de um include. O mesmo spec ("utils/log.h") em
    //! dois projetos resolve para paths diferentes, entao compiler, flags e o
    //! path resolvido entram no hash: sem isso um projeto serviria o indice do
    //! outro.
    static QString cacheFileName(const QString& includeSpec, const QString& resolvedPath,
                                 const QString& compiler, const QStringList& flags);

    //! Chamado na thread da UI quando o worker termina um include.
    void publish(const QString& includeSpec, const std::shared_ptr<Entry>& entry,
                 bool ok);

    //! Agenda `publish` no event loop. O `includeIndexed` chega SEMPRE de
    //! forma assincrona, mesmo no hit de cache (que e sincrono): quem conecta
    //! no signal e espera o resultado nunca perde a notificacao.
    void publishLater(const QString& includeSpec, const std::shared_ptr<Entry>& entry,
                      bool ok);

private:
    HeaderIndex();
    ~HeaderIndex() override;

    //! Preprocessa + parseia (roda no worker).
    bool build(const QString& includeSpec, const QString& fromFile,
               std::shared_ptr<Entry>* out, QString* error) const;
    bool loadCache(const QString& includeSpec, const QString& fromFile, Entry* out) const;
    //! O carimbo ainda bate com o disco e com a configuracao atual?
    bool stampIsCurrent(const CacheStamp& stamp) const;
    void saveCache(const QString& includeSpec, const Entry& e) const;
    //! Include paths do compilador (cacheados: `-E -v` custa ~20ms).
    QStringList compilerIncludePaths() const;

    mutable QMutex m_mutex;
    QHash<QString, std::shared_ptr<Entry>> m_entries;  // include -> simbolos
    QSet<QString> m_pending;                          // includes em andamento
    QStringList m_order;               // ordem de insercao (para "ja indexados")

    QString m_compiler = QStringLiteral("g++");
    QStringList m_flags;
    QStringList m_projectDirs;
    QString m_cacheDir;
    mutable QStringList m_includePaths;  // cache de `-E -v`
    mutable bool m_includePathsValid = false;

    //! O worker e a propria thread: `QThread::run()` so e chamado no objeto
    //! derivado de QThread em que `start()` e invocado.
    class Worker;
    std::unique_ptr<Worker> m_worker;

signals:
    //! Um include terminou (ok ou falha).
    void includeIndexed(const QString& includeSpec, bool ok);
    //! Compilador/flags/dirs mudaram: o que estava em memoria foi descartado
    //! e os includes precisam ser pedidos de novo.
    void contextChanged();
};

//! Percorre uma `SymbolTree` emitindo nomes visiveis, com o prefixo de inline
//! namespace colapsado (`std::__cxx11::string` -> `std::string`).
template <typename Fn>
void forEachVisibleName(const SymbolNode& node, const QString& prefix, Fn fn)
{
    const QString qualified = prefix.isEmpty() ? node.name
                                               : prefix + QLatin1String("::") + node.name;
    if (!node.name.isEmpty())
        fn(qualified, node);
    // membro de inline namespace tambem e visivel no namespace pai
    const QString childPrefix = node.isInline ? prefix : qualified;
    for (const SymbolNode& c : node.children)
        forEachVisibleName(c, childPrefix, fn);
}

template <typename Fn>
void forEachVisibleName(const SymbolTree& tree, Fn fn)
{
    for (const SymbolNode& r : tree.roots)
        forEachVisibleName(r, QString(), fn);
}

}  // namespace editor
