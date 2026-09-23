#include "core/Keywords.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRecursiveMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVector>

#include <functional>

#include <QDebug>

namespace core {

namespace {

constexpr int kMaxFiles = 48;          // limite total no scan (anti-explosao)
constexpr qint64 kMaxBytes = 3 * 1024 * 1024;  // por arquivo
constexpr int kMinFreq = 2;            // aparecer 2x+ p/ entrar no popup
constexpr int kMaxWords = 400;         // teto de palavras escaneadas

QString cacheDir()
{
    return KeywordIndex::keywordsDir() + QStringLiteral("/cache");
}

QString indexFile()
{
    return cacheDir() + QStringLiteral("/index.json");
}

QJsonObject loadIndex()
{
    QFile f(indexFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void saveIndex(const QJsonObject& idx)
{
    QFile f(indexFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(idx).toJson(QJsonDocument::Indented));
}

//! Le uma lista de palavras de um arquivo (linha por linha; '#' = comentario).
QStringList readWordList(const QString& path)
{
    QStringList out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const QStringList lines =
        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        out << line;
    }
    return out;
}

//! Substitui comentarios/literais por espacos (preserva comprimento: nada
//! de fundir "int/*x*/y" em "inty").
QString stripCommentsAndStrings(const QString& src)
{
    QString out = src;
    const int n = out.size();
    int i = 0;
    enum class S { Text, Line, Block, Str, Ch } state = S::Text;
    while (i < n) {
        const QChar c = out.at(i);
        const QChar nx = (i + 1 < n) ? out.at(i + 1) : QChar();
        switch (state) {
        case S::Text:
            if (c == QLatin1Char('/') && nx == QLatin1Char('*')) { state = S::Block; out[i]=QLatin1Char(' '); out[i+1]=QLatin1Char(' '); i += 2; }
            else if (c == QLatin1Char('/') && nx == QLatin1Char('/')) { state = S::Line; out[i]=QLatin1Char(' '); out[i+1]=QLatin1Char(' '); i += 2; }
            else if (c == QLatin1Char('"')) { state = S::Str; out[i]=QLatin1Char(' '); ++i; }
            else if (c == QLatin1Char('\'')) { state = S::Ch; out[i]=QLatin1Char(' '); ++i; }
            else ++i;
            break;
        case S::Line:
            if (c == QLatin1Char('\n')) { state = S::Text; }
            else out[i] = QLatin1Char(' ');
            ++i; break;
        case S::Block:
            if (c == QLatin1Char('*') && nx == QLatin1Char('/')) { state = S::Text; out[i]=QLatin1Char(' '); out[i+1]=QLatin1Char(' '); i += 2; }
            else { out[i]=QLatin1Char(' '); ++i; }
            break;
        case S::Str:
            if (c == QLatin1Char('\\')) { if (i+1<n) out[i+1]=QLatin1Char(' '); out[i]=QLatin1Char(' '); i += 2; }
            else if (c == QLatin1Char('"')) { state = S::Text; out[i]=QLatin1Char(' '); ++i; }
            else { out[i]=QLatin1Char(' '); ++i; }
            break;
        case S::Ch:
            if (c == QLatin1Char('\\')) { if (i+1<n) out[i+1]=QLatin1Char(' '); out[i]=QLatin1Char(' '); i += 2; }
            else if (c == QLatin1Char('\'')) { state = S::Text; out[i]=QLatin1Char(' '); ++i; }
            else { out[i]=QLatin1Char(' '); ++i; }
            break;
        }
    }
    return out;
}

//! Resolve um nome de include: '"' relativo ao diretorio do arquivo; '<'
//! (ou nao achado localmente) nos diretorios de sistema.
QString resolveInclude(const QString& name, QChar delim,
                       const QString& ownDir, const QStringList& sysDirs)
{
    if (delim == QLatin1Char('"')) {
        const QString local = QDir(ownDir).filePath(name);
        if (QFileInfo::exists(local))
            return local;
    }
    for (const QString& d : sysDirs) {
        const QString p = QDir(d).filePath(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return {};
}

//! Tokens que nunca entram no popup (preprocessor/ruido).
bool isNoiseKeyword(const QString& w)
{
    static const QSet<QString> stop = {
        "ifdef", "ifndef", "define", "undef", "endif", "else", "elif",
        "include", "pragma", "once", "error", "warning", "line", "import",
        "__cplusplus", "stdext", "_MSC_VER", "__GNUC__", "_GLIBCXX", "__extension__",
        "typedef", "asm", "register", "goto",
    };
    if (stop.contains(w))
        return true;
    if (w.startsWith(QLatin1String("__")))
        return true;  // detalhe de implementacao
    bool allDigit = true;
    for (const QChar ch : w)
        if (!ch.isDigit()) { allDigit = false; break; }
    return allDigit;
}

//! Diretorios de sistema do include de C/C++ (resolve <iostream>,
//! <vector>, ... que vivem em /usr/include/c++/<versao>/...). Cacheado.
QStringList systemIncludeDirs()
{
    static const QStringList dirs = [] {
        QStringList out = { QStringLiteral("/usr/local/include") };
        for (const QString& root : {
                 QStringLiteral("/usr/include"),
                 QStringLiteral("/usr/include/x86_64-linux-gnu") })
            for (const QString& sub :
                 QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (sub.startsWith(QLatin1String("c++")))
                    out << QDir(root).filePath(sub);
        // c++/<versao>[/<arquitetura>] ficam UM nivel abaixo (ex.:
        // /usr/include/c++/13/iostream). Expande um nivel p/ cada raiz c++.
        const QStringList raizes = out;
        for (const QString& r : raizes) {
            const QFileInfo fi(r);
            if (!fi.fileName().startsWith(QLatin1String("c++")))
                continue;
            for (const QString& sub2 :
                 QDir(r).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (sub2 != QLatin1String("backward"))
                    out << QDir(r).filePath(sub2);
        }
        out << QStringLiteral("/usr/include/x86_64-linux-gnu");
        out << QStringLiteral("/usr/include");
        return out;
    }();
    return dirs;
}

//! Scan em si: coletor de tokens do arquivo + #includes (ancorado).
QStringList collectScanWords(const QString& mainPath,
                             const QSet<QString>& baseWords,
                             QList<DepStamp>* depOut)
{
    QFileInfo main(mainPath);
    if (!main.exists())
        return {};
    const QStringList sysDirs = systemIncludeDirs();
    const QString ownDir = main.absolutePath();

    static const QRegularExpression includeRe(
        QStringLiteral("#[ \\t]*include[ \\t]*([<\"])([^>\"]+)[>\"]"));
    static const QRegularExpression idRe(
        QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*\\b"));

    QHash<QString, int> freq;
    QSet<QString> scannedFiles;
    QList<DepStamp> deps;
    int nFiles = 0;

    std::function<void(const QString&, int)> scanFile =
        [&](const QString& path, int depth) {
            if (scannedFiles.contains(path) || nFiles >= kMaxFiles)
                return;
            scannedFiles.insert(path);
            ++nFiles;
            QFileInfo fi(path);
            if (!fi.exists() || fi.size() > kMaxBytes)
                return;
            deps.append({fi.absoluteFilePath(),
                         fi.lastModified().toMSecsSinceEpoch(), fi.size()});

            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                return;
            const QString text =
                stripCommentsAndStrings(QString::fromUtf8(f.readAll()));

            // Incluidos -> recursao (depth 0 = main; ate depth 2).
            if (depth < 2) {
                auto it = includeRe.globalMatch(text);
                while (it.hasNext()) {
                    const auto m = it.next();
                    const QString resolved =
                        resolveInclude(m.captured(2), m.captured(1).at(0),
                                       ownDir, sysDirs);
                    if (!resolved.isEmpty())
                        scanFile(resolved, depth + 1);
                }
            }

            // Tokens do proprio arquivo.
            auto idit = idRe.globalMatch(text);
            while (idit.hasNext())
                ++freq[idit.next().captured()];
        };
    scanFile(mainPath, 0);

    if (depOut)
        *depOut = deps;

    QVector<QPair<int, QString>> ranked;
    for (auto it = freq.constBegin(); it != freq.constEnd(); ++it) {
        const QString& w = it.key();
        const int c = it.value();
        if (c < kMinFreq || w.size() < 3)
            continue;
        if (baseWords.contains(w) || isNoiseKeyword(w))
            continue;
        ranked.append({c, w});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                  if (a.first != b.first)
                      return a.first > b.first;
                  return a.second < b.second;
              });
    QStringList out;
    out.reserve(qMin(ranked.size(), kMaxWords));
    for (const auto& r : ranked) {
        out << r.second;
        if (out.size() >= kMaxWords)
            break;
    }
    return out;
}

//! QRunnable do scan (roda no QThreadPool; marshals p/ GUI ao concluir).
class KeywordScanTask final : public QRunnable {
public:
    KeywordScanTask(KeywordIndex* index, QString path, QString lang)
        : m_index(index), m_path(std::move(path)), m_lang(std::move(lang))
    {
    }

    void run() override
    {
        const QStringList keywords =
            KeywordIndex::scanAndCache(m_path, m_lang);
        if (!m_index)
            return;
        QMetaObject::invokeMethod(
            m_index, "onScanFinished", Qt::QueuedConnection,
            Q_ARG(QString, m_path), Q_ARG(QString, m_lang),
            Q_ARG(QStringList, keywords));
    }

private:
    QPointer<KeywordIndex> m_index;
    QString m_path;
    QString m_lang;
};

}  // namespace

//! Le/grava index.json (serializado pelo mutex estatico da classe).
QMutex KeywordIndex::m_ioMutex;

KeywordIndex* KeywordIndex::instance()
{
    static KeywordIndex* k = new KeywordIndex;
    return k;
}

QString KeywordIndex::keywordsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/VSEditor/keywords");
}

QString KeywordIndex::languageForFile(const QString& filePath)
{
    const QString suffix =
        QFileInfo(filePath).suffix().toLower();
    if (suffix == QLatin1String("cpp") || suffix == QLatin1String("cc")
        || suffix == QLatin1String("cxx") || suffix == QLatin1String("h")
        || suffix == QLatin1String("hpp") || suffix == QLatin1String("hxx")
        || suffix == QLatin1String("c") || suffix == QLatin1String("hh"))
        return QStringLiteral("cpp");
    if (suffix == QLatin1String("sql"))
        return QStringLiteral("sql");
    if (suffix == QLatin1String("asm") || suffix == QLatin1String("s")
        || suffix == QLatin1String("S"))
        return QStringLiteral("asm");
    if (suffix == QLatin1String("sh") || suffix == QLatin1String("bash"))
        return QStringLiteral("bash");
    if (suffix == QLatin1String("py"))
        return QStringLiteral("python");
    return {};
}

bool KeywordIndex::scansIncludes(const QString& language)
{
    return language == QLatin1String("cpp");
}

QStringList KeywordIndex::defaultKeywords(const QString& language) const
{
    const QString lang = language.trimmed();
    if (lang.isEmpty())
        return {};

    QStringList base;
    {
        QMutexLocker guard(&m_baseMutex);
        const auto it = m_baseWords.constFind(lang);
        if (it != m_baseWords.constEnd())
            base = it->values();
    }
    if (!base.isEmpty())
        return base;
    // Lista vazia NAO e caxada: o arquivo pode aparecer depois (ex.: o
    // usuario cria ~/.config/VSEditor/keywords/cpp.txt ao adicionar 500
    // simbolos) e a proxima chamada deve enxerga-lo.

    // 1) override do usuario; 2) recurso embutido.
    const QDir dir(keywordsDir());
    QString path = dir.filePath(lang + QStringLiteral(".txt"));
    if (!QFileInfo::exists(path))
        path = QStringLiteral(":/keywords/") + lang + QStringLiteral(".txt");
    base = readWordList(path);
    if (!base.isEmpty()) {
        QMutexLocker guard(&m_baseMutex);
        m_baseWords.insert(lang, QSet<QString>(base.constBegin(),
                                               base.constEnd()));
    }
    return base;
}

bool KeywordIndex::isCacheValid(const QString& filePath,
                                const QString& language)
{
    if (filePath.isEmpty() || cacheDir().isEmpty())
        return false;
    QMutexLocker guard(&m_ioMutex);
    const QJsonObject idx = loadIndex();
    const QJsonObject e = idx.value(filePath).toObject();
    if (e.isEmpty() || e.value(QLatin1String("lang")).toString() != language)
        return false;
    const QString kwl = e.value(QLatin1String("kwl")).toString();
    if (kwl.isEmpty()
        || !QFileInfo::exists(QDir(cacheDir()).filePath(kwl)))
        return false;
    const QFileInfo main(filePath);
    if (!main.exists())
        return false;
    if (e.value(QLatin1String("main_mtime")).toVariant().toLongLong()
        != main.lastModified().toMSecsSinceEpoch())
        return false;
    const QJsonArray deps = e.value(QLatin1String("deps")).toArray();
    for (const auto& v : deps) {
        const QJsonObject d = v.toObject();
        const QFileInfo di(d.value(QLatin1String("path")).toString());
        if (!di.exists())
            return false;
        if (di.lastModified().toMSecsSinceEpoch()
                != d.value(QLatin1String("mtime")).toVariant().toLongLong()
            || di.size() != d.value(QLatin1String("size")).toVariant().toLongLong())
            return false;
    }
    return true;
}

QStringList KeywordIndex::cachedKeywordsFor(const QString& filePath) const
{
    if (!isCacheValid(filePath, languageForFile(filePath)))
        return {};
    QMutexLocker guard(&m_ioMutex);
    const QJsonObject idx = loadIndex();
    const QString kwl =
        idx.value(filePath).toObject().value(QLatin1String("kwl")).toString();
    if (kwl.isEmpty())
        return {};
    return readWordList(QDir(cacheDir()).filePath(kwl));
}

void KeywordIndex::saveIndexEntry(
    const QString& filePath, const QString& language,
    const QStringList& keywords, const QList<DepStamp>& depStamps)
{
    QMutexLocker guard(&m_ioMutex);
    const QFileInfo main(filePath);
    if (!main.exists())
        return;
    const QString kwlName = QString::fromLatin1(
        QCryptographicHash::hash(filePath.toUtf8(),
                                 QCryptographicHash::Sha1).toHex())
        + QStringLiteral(".kwl");

    QFile kwl(QDir(cacheDir()).filePath(kwlName));
    if (!kwl.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    for (const QString& w : keywords)
        kwl.write((w + QLatin1Char('\n')).toUtf8());
    kwl.close();

    QJsonObject e;
    e.insert(QLatin1String("lang"), language);
    e.insert(QLatin1String("main_mtime"), main.lastModified().toMSecsSinceEpoch());
    e.insert(QLatin1String("kwl"), kwlName);
    QJsonArray da;
    for (const DepStamp& d : depStamps) {
        QJsonObject o;
        o.insert(QLatin1String("path"), d.path);
        o.insert(QLatin1String("mtime"), d.mtime);
        o.insert(QLatin1String("size"), d.size);
        da.append(o);
    }
    e.insert(QLatin1String("deps"), da);

    QJsonObject idx = loadIndex();
    idx.insert(filePath, e);
    saveIndex(idx);
}

QStringList KeywordIndex::scanAndCache(const QString& filePath,
                                       const QString& language)
{
    if (filePath.isEmpty() || !scansIncludes(language))
        return {};
    // Cache ainda valido: nao reescaneia (reusa o que aplicacao ja leu).
    if (isCacheValid(filePath, language))
        return instance()->cachedKeywordsFor(filePath);

    const QStringList baseWords = instance()->defaultKeywords(language);
    const QSet<QString> base(baseWords.constBegin(), baseWords.constEnd());
    QList<DepStamp> deps;
    const QStringList words = collectScanWords(filePath, base, &deps);
    if (!words.isEmpty())
        saveIndexEntry(filePath, language, words, deps);
    return words;
}

void KeywordIndex::requestScan(const QString& filePath,
                               const QString& language)
{
    if (filePath.isEmpty() || !scansIncludes(language))
        return;
    const QString key = filePath + QLatin1Char(0x1f) + language;
    {
        QMutexLocker guard(&m_inFlightMutex);
        if (m_inFlight.contains(key))
            return;
        // Nada a fazer: cache valido (o chamador ja o usa sincrono).
        if (isCacheValid(filePath, language))
            return;
        m_inFlight.insert(key);
    }
    QThreadPool::globalInstance()->start(
        new KeywordScanTask(this, filePath, language));
}

void KeywordIndex::onScanFinished(const QString& filePath,
                                  const QString& language,
                                  const QStringList& keywords)
{
    const QString key = filePath + QLatin1Char(0x1f) + language;
    {
        QMutexLocker guard(&m_inFlightMutex);
        m_inFlight.remove(key);
    }
    emit scanReady(filePath, language, keywords);
}

KeywordIndex::KeywordIndex(QObject* parent)
    : QObject(parent)
{
    // Garante que a pasta do indice exista (o worker so escreve nela).
    QDir().mkpath(cacheDir());
}

}  // namespace core