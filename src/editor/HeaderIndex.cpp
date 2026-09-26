// HeaderIndex.cpp
#include "editor/HeaderIndex.hpp"
#include "editor/TreeSitterOutlineParser.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>
#include <QSaveFile>
#include <QWaitCondition>

namespace editor {
namespace {

constexpr quint32 kCacheVersion = 4;

//! "<vector>" e "vector" sao a mesma chave; "\"utils/log.h\"" -> "utils/log.h".
QString normalizeSpec(const QString& spec)
{
    QString s = spec.trimmed();
    if (s.startsWith(QLatin1Char('<')) && s.endsWith(QLatin1Char('>')))
        s = s.mid(1, s.size() - 2);
    else if (s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        s = s.mid(1, s.size() - 2);
    return s;
}

void writeNode(QDataStream& out, const SymbolNode& n)
{
    out << n.name << int(n.kind) << n.signature << n.file << n.line
        << n.qualifiedName << n.isOutOfLine << n.isInline << int(n.children.size());
    for (const SymbolNode& c : n.children)
        writeNode(out, c);
}

void readNode(QDataStream& in, SymbolNode* n)
{
    int kind = 0;
    int count = 0;
    in >> n->name >> kind >> n->signature >> n->file >> n->line >> n->qualifiedName
        >> n->isOutOfLine >> n->isInline >> count;
    n->kind = SymbolKind(kind);
    n->children.resize(count);
    for (int i = 0; i < count; ++i)
        readNode(in, &n->children[i]);
}

QStringList membersOfStd(const SymbolTree& tree)
{
    QStringList out;
    QSet<QString> seen;
    forEachVisibleName(tree, [&](const QString& qualified, const SymbolNode& n) {
        static const QString prefix = QStringLiteral("std::");
        if (!qualified.startsWith(prefix))
            return;
        const QString rest = qualified.mid(prefix.size());
        if (rest.isEmpty() || rest.contains(QLatin1String("::")))
            return;  // so os membros diretos de std
        if (seen.contains(rest))
            return;
        seen.insert(rest);
        out.append(rest);
    });
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Worker: roda o preprocessamento + parse fora da thread da UI
// ---------------------------------------------------------------------------

class HeaderIndex::Worker final : public QThread {
public:
    explicit Worker(HeaderIndex* owner) : m_owner(owner) {}

    void submit(const QVector<QPair<QString, QString>>& jobs)
    {
        QMutexLocker lock(&m_mutex);
        m_jobs.append(jobs);
        m_hasWork.wakeAll();
    }

    void shutdown()
    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_hasWork.wakeAll();
    }

protected:
    void run() override
    {
        for (;;) {
            QVector<QPair<QString, QString>> jobs;
            {
                QMutexLocker lock(&m_mutex);
                while (m_running && m_jobs.isEmpty())
                    m_hasWork.wait(&m_mutex);
                if (!m_running)
                    return;
                jobs.swap(m_jobs);
            }

            for (const auto& job : jobs) {
                auto entry = std::make_shared<HeaderIndex::Entry>();
                QString error;
                const bool ok = m_owner->build(job.first, job.second, &entry, &error);
                if (!ok) {
                    qWarning("HeaderIndex: nao indexei '%s': %s", qPrintable(job.first),
                             qPrintable(error));
                } else {
                    m_owner->saveCache(job.first, *entry);
                }

                // publica na thread da UI: a arvore vai por shared_ptr, nao
                // por copia no signal (sao milhares de nos)
                m_owner->publishLater(job.first, entry, ok);
            }
        }
    }

private:
    HeaderIndex* m_owner;
    QMutex m_mutex;
    QWaitCondition m_hasWork;
    QVector<QPair<QString, QString>> m_jobs;
    bool m_running = true;
};

// ---------------------------------------------------------------------------

HeaderIndex* HeaderIndex::instance()
{
    static HeaderIndex idx;
    return &idx;
}

HeaderIndex::HeaderIndex()
{
    m_worker = std::make_unique<Worker>(this);
    m_worker->setObjectName(QStringLiteral("HeaderIndexWorker"));
    m_worker->start();
}

HeaderIndex::~HeaderIndex()
{
    if (m_worker) {
        m_worker->shutdown();
        m_worker->wait();
    }
}

void HeaderIndex::publish(const QString& includeSpec,
                          const std::shared_ptr<Entry>& entry, bool ok)
{
    {
        QMutexLocker lock(&m_mutex);
        m_pending.remove(includeSpec);
        if (ok && !m_entries.contains(includeSpec)) {
            m_entries.insert(includeSpec, entry);
            m_order.append(includeSpec);
        }
    }
    emit includeIndexed(includeSpec, ok);
}

void HeaderIndex::publishLater(const QString& includeSpec,
                               const std::shared_ptr<Entry>& entry, bool ok)
{
    HeaderIndex* self = this;
    QMetaObject::invokeMethod(
        self, [self, includeSpec, entry, ok]() { self->publish(includeSpec, entry, ok); },
        Qt::QueuedConnection);
}

void HeaderIndex::configure(const QString& compiler, const QStringList& flags,
                            const QStringList& projectDirs)
{
    bool mudou = false;
    {
        QMutexLocker lock(&m_mutex);
        mudou = m_compiler != compiler || m_flags != flags || m_projectDirs != projectDirs;
        m_compiler = compiler;
        m_flags = flags;
        m_projectDirs = projectDirs;
        m_includePaths.clear();
        m_includePathsValid = false;
        if (mudou) {
            // O que ja estava em memoria foi preprocessado com o contexto
            // antigo (outro compilador, outras flags): nao vale mais.
            m_entries.clear();
            m_order.clear();
        }
    }
    // Fora do lock: um slot pode chamar request()/names() de volta.
    if (mudou)
        emit contextChanged();
}

void HeaderIndex::setProjectDirs(const QStringList& dirs)
{
    QMutexLocker lock(&m_mutex);
    m_projectDirs = dirs;
}

bool HeaderIndex::isIndexed(const QString& includeSpec) const
{
    QMutexLocker lock(&m_mutex);
    return m_entries.contains(normalizeSpec(includeSpec));
}

QStringList HeaderIndex::indexedIncludes() const
{
    QMutexLocker lock(&m_mutex);
    return m_order;
}

QStringList HeaderIndex::pendingIncludes() const
{
    QMutexLocker lock(&m_mutex);
    return QStringList(m_pending.constBegin(), m_pending.constEnd());
}

bool HeaderIndex::request(const QString& includeSpec, const QString& fromFile)
{
    const QString key = normalizeSpec(includeSpec);
    if (key.isEmpty())
        return false;

    {
        QMutexLocker lock(&m_mutex);
        if (m_entries.contains(key) || m_pending.contains(key))
            return false;
        m_pending.insert(key);
    }

    // Cache em disco: nao ha preprocesso nem parse, so desserializar. O
    // `publish` pega o mutex, entao fica fora do bloco acima.
    auto cached = std::make_shared<Entry>();
    if (loadCache(key, fromFile, cached.get()) && !cached->tree.roots.isEmpty()) {
        publishLater(key, cached, true);
        return true;
    }

    m_worker->submit({{key, fromFile}});
    return true;
}

QVector<IndexedName> HeaderIndex::names(const QString& includeSpec) const
{
    const QString key = normalizeSpec(includeSpec);
    QMutexLocker lock(&m_mutex);
    QVector<IndexedName> out;
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || !*it)
        return out;
    forEachVisibleName((*it)->tree, [&](const QString& qualified, const SymbolNode& n) {
        IndexedName e;
        e.name = qualified;
        e.kind = n.kind;
        e.file = n.file;
        e.line = n.line;
        out.append(e);
    });
    return out;
}

QStringList HeaderIndex::stdMembers(const QString& includeSpec) const
{
    const QString key = normalizeSpec(includeSpec);
    QMutexLocker lock(&m_mutex);
    const auto it = m_entries.constFind(key);
    return (it == m_entries.constEnd() || !*it) ? QStringList() : (*it)->stdMembers;
}

bool HeaderIndex::find(const QString& qualifiedName, IndexedName* out) const
{
    QMutexLocker lock(&m_mutex);
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        bool found = false;
        IndexedName hit;
        if (!*it)
            continue;
        forEachVisibleName((*it)->tree, [&](const QString& qualified, const SymbolNode& n) {
            if (found)
                return;
            if (qualified != qualifiedName)
                return;
            hit.name = qualified;
            hit.kind = n.kind;
            hit.file = n.file;
            hit.line = n.line;
            found = true;
        });
        if (found) {
            if (out)
                *out = hit;
            return true;
        }
    }
    return false;
}

void HeaderIndex::clearMemory()
{
    QMutexLocker lock(&m_mutex);
    m_entries.clear();
    m_order.clear();
}

// ---------------------------------------------------------------------------
// build / cache
// ---------------------------------------------------------------------------

QString HeaderIndex::resolveHeaderPath(const QString& includeSpec,
                                      const QString& fromFile) const
{
    QStringList dirs;
    {
        QMutexLocker lock(&m_mutex);
        dirs = m_projectDirs;
    }
    // O preprocessador so aceita um caminho real, entao resolvemos antes:
    // projeto primeiro (o que o user escreve entre aspas), depois os
    // diretorios do proprio compilador.
    if (QFileInfo(includeSpec).isAbsolute() && QFileInfo::exists(includeSpec))
        return includeSpec;
    IncludeResolver resolver;
    resolver.setProjectDirs(dirs);
    resolver.setSearchPaths(compilerIncludePaths());
    QString path = resolver.resolve(includeSpec);
    if (path.isEmpty() && !fromFile.isEmpty()) {
        // include relativo ao arquivo que o pediu ("config.h" ao lado)
        const QString cand = QDir(fromFile).absoluteFilePath(includeSpec);
        if (QFileInfo::exists(cand))
            path = QFileInfo(cand).absoluteFilePath();
    }
    return path;
}

bool HeaderIndex::build(const QString& includeSpec, const QString& fromFile,
                        std::shared_ptr<Entry>* out, QString* error) const
{
    QString compiler;
    QStringList flags;
    {
        QMutexLocker lock(&m_mutex);
        compiler = m_compiler;
        flags = m_flags;
    }

    // Onde esta o header?
    const QString path = resolveHeaderPath(includeSpec, fromFile);
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("header nao encontrado: %1").arg(includeSpec);
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    Preprocessor cpp(compiler);
    cpp.setFlags(flags);
    const PreprocessResult res = cpp.run(path);
    if (!res.ok) {
        if (error)
            *error = res.error;
        return false;
    }

    // Instancia propria: o parser nao e thread-safe.
    TreeSitterOutlineParser parser;
    const SymbolTree tree = parser.parseSymbols(res.text, path, &res.map);

    int count = 0;
    forEachSymbol(tree, [&](const SymbolNode&) { ++count; });
    qInfo("HeaderIndex: '%s' -> %d simbolos em %d ms", qPrintable(includeSpec), count,
          int(timer.elapsed()));

    if (out) {
        auto entry = std::make_shared<Entry>();
        entry->tree = tree;
        entry->stdMembers = membersOfStd(tree);
        entry->stamp.compiler = compiler;
        entry->stamp.flags = flags;
        entry->stamp.entryFile = path;
        // Todo arquivo citado nos marcadores entrou no resultado, entao o mtime
        // de cada um diz se o cache ainda vale (mesmo os `.h` internos).
        QSet<QString> vistos;
        for (const QString& f : res.map.files) {
            if (f.isEmpty() || vistos.contains(f))
                continue;
            vistos.insert(f);
            const QFileInfo info(f);
            if (info.exists())
                entry->stamp.fileMtimes.insert(f, info.lastModified().toMSecsSinceEpoch());
        }
        vistos.insert(path);
        entry->stamp.fileMtimes.insert(path,
                                       QFileInfo(path).lastModified().toMSecsSinceEpoch());
        *out = entry;
    }
    if (error)
        error->clear();
    return true;
}

QStringList HeaderIndex::compilerIncludePaths() const
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_includePathsValid)
            return m_includePaths;
    }
    QString compiler;
    QStringList flags;
    {
        QMutexLocker lock(&m_mutex);
        compiler = m_compiler;
        flags = m_flags;
    }
    Preprocessor cpp(compiler);
    cpp.setFlags(flags);
    const QStringList paths = cpp.includePaths();
    {
        QMutexLocker lock(&m_mutex);
        m_includePaths = paths;
        m_includePathsValid = true;
    }
    return paths;
}

QString HeaderIndex::cacheFileName(const QString& includeSpec, const QString& resolvedPath,
                                  const QString& compiler, const QStringList& flags)
{
    QString key = normalizeSpec(includeSpec);
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    // Nome legivel + hash do contexto. O mesmo include em outro projeto (ou
    // com outras flags) resolve para outro arquivo, entao um "-I /outro/include"
    // nao acaba lendo o indice do projeto vizinho.
    const QByteArray ctx = (resolvedPath + QLatin1Char('\n') + compiler + QLatin1Char('\n')
                            + flags.join(QLatin1Char(' ')))
                               .toUtf8();
    return key + QLatin1Char('-')
           + QString::fromLatin1(QCryptographicHash::hash(ctx, QCryptographicHash::Sha1).toHex().left(8))
           + QStringLiteral(".idx");
}

bool HeaderIndex::stampIsCurrent(const CacheStamp& stamp) const
{
    QString compiler;
    QStringList flags;
    {
        QMutexLocker lock(&m_mutex);
        compiler = m_compiler;
        flags = m_flags;
    }
    if (stamp.compiler != compiler || stamp.flags != flags)
        return false;
    if (stamp.entryFile.isEmpty() || !QFileInfo::exists(stamp.entryFile))
        return false;
    for (auto it = stamp.fileMtimes.constBegin(); it != stamp.fileMtimes.constEnd(); ++it) {
        const QFileInfo info(it.key());
        if (!info.exists())
            return false;  // header removido/renomeado: reindexa
        if (info.lastModified().toMSecsSinceEpoch() != it.value())
            return false;  // editado depois do cache
    }
    return true;
}

bool HeaderIndex::loadCache(const QString& includeSpec, const QString& fromFile,
                            Entry* out) const
{
    if (m_cacheDir.isEmpty())
        return false;
    // Mesma resolucao do build: e ela que decide qual arquivo de cache vale.
    const QString path = resolveHeaderPath(includeSpec, fromFile);
    if (path.isEmpty())
        return false;
    QString compiler;
    QStringList flags;
    {
        QMutexLocker lock(&m_mutex);
        compiler = m_compiler;
        flags = m_flags;
    }
    QFile f(QDir(m_cacheDir).filePath(cacheFileName(includeSpec, path, compiler, flags)));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QDataStream in(&f);
    quint32 version = 0;
    in >> version;
    if (version != kCacheVersion)
        return false;
    CacheStamp stamp;
    in >> stamp.compiler >> stamp.flags >> stamp.entryFile;
    int stampCount = 0;
    in >> stampCount;
    if (stampCount < 0 || stampCount > 100000)
        return false;
    for (int i = 0; i < stampCount; ++i) {
        QString f;
        qint64 mtime = 0;
        in >> f >> mtime;
        stamp.fileMtimes.insert(f, mtime);
    }
    if (!stampIsCurrent(stamp))
        return false;
    out->stamp = stamp;
    int rootCount = 0;
    in >> rootCount;
    if (rootCount < 0 || rootCount > 100000)
        return false;
    out->tree.roots.resize(rootCount);
    for (int i = 0; i < rootCount; ++i)
        readNode(in, &out->tree.roots[i]);
    in >> out->tree.usingNamespaces >> out->tree.includes;
    in >> out->stdMembers;
    return in.status() == QDataStream::Ok;
}

void HeaderIndex::saveCache(const QString& includeSpec, const Entry& e) const
{
    if (m_cacheDir.isEmpty())
        return;
    QDir().mkpath(m_cacheDir);
    QSaveFile f(QDir(m_cacheDir).filePath(cacheFileName(includeSpec, e.stamp.entryFile,
                                                       e.stamp.compiler, e.stamp.flags)));
    if (!f.open(QIODevice::WriteOnly))
        return;
    QDataStream out(&f);
    out.setVersion(QDataStream::Qt_6_0);
    out << kCacheVersion;
    out << e.stamp.compiler << e.stamp.flags << e.stamp.entryFile;
    out << int(e.stamp.fileMtimes.size());
    for (auto it = e.stamp.fileMtimes.constBegin(); it != e.stamp.fileMtimes.constEnd(); ++it)
        out << it.key() << it.value();
    out << int(e.tree.roots.size());
    for (const SymbolNode& r : e.tree.roots)
        writeNode(out, r);
    out << e.tree.usingNamespaces << e.tree.includes << e.stdMembers;
    f.commit();
}

}  // namespace editor
