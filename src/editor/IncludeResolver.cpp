// IncludeResolver.cpp
#include "editor/IncludeResolver.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

namespace editor {
namespace {

bool isHeaderName(const QString& name)
{
    static const QStringList exts = {QStringLiteral(".h"),  QStringLiteral(".hpp"),
                                     QStringLiteral(".hxx"), QStringLiteral(".hh"),
                                     QStringLiteral(".H"),  QStringLiteral(".tcc"),
                                     QStringLiteral(".ipp")};
    for (const QString& e : exts) {
        if (name.endsWith(e, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

}  // namespace

QString IncludeResolver::resolve(const QString& spec) const
{
    if (spec.isEmpty())
        return {};
    const QString path = QDir::cleanPath(spec);
    if (QFileInfo(path).isAbsolute())
        return QFileInfo::exists(path) ? path : QString();

    // o que esta entre aspas no codigo do usuario vem primeiro
    for (const QString& d : m_projectDirs) {
        const QString cand = QDir(d).filePath(path);
        if (QFileInfo::exists(cand))
            return QFileInfo(cand).absoluteFilePath();
    }
    for (const QString& d : m_dirs) {
        const QString cand = QDir(d).filePath(path);
        if (QFileInfo::exists(cand))
            return QFileInfo(cand).absoluteFilePath();
    }
    return {};
}

QStringList IncludeResolver::complete(const QString& prefix, bool angled) const
{
    QStringList out;
    if (prefix.isEmpty())
        return out;

    const int slash = prefix.lastIndexOf(QLatin1Char('/'));
    const QString dirPart = (slash < 0) ? QString() : prefix.left(slash);
    const QString namePart = (slash < 0) ? prefix : prefix.mid(slash + 1);
    if (namePart.contains(QLatin1Char('.')) && !namePart.endsWith(QLatin1Char('h')))
        return out;  // ja digitou extensao, provavelmente nao quer incluir mais

    QStringList dirs = m_projectDirs + m_dirs;
    if (!dirPart.isEmpty()) {
        QStringList narrowed;
        for (const QString& d : dirs) {
            const QString cand = QDir(d).filePath(dirPart);
            if (QFileInfo(cand).isDir())
                narrowed.append(cand);
        }
        dirs = narrowed;
    }

    QSet<QString> seen;
    constexpr int kMaxResults = 300;
    for (const QString& d : dirs) {
        QDirIterator it(d, angled ? QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                  : QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QFileInfo fi(path);
            if (!fi.isFile() || !isHeaderName(fi.fileName()))
                continue;
            if (!fi.fileName().startsWith(namePart))
                continue;
            const QString rel = QDir(d).relativeFilePath(path);
            if (seen.contains(rel))
                continue;
            seen.insert(rel);
            out.append(rel);
            if (out.size() >= kMaxResults)
                return out;
        }
    }
    return out;
}

}  // namespace editor
