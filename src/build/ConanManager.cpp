#include "build/ConanManager.hpp"

#include "project/Project.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

namespace build {

namespace {

//! "fmt/10.2.1" -> "fmt"; "boost/1.83.0*" -> "boost"; "opencv[>=4]" -> "opencv".
QString packageNameBase(const QString& require)
{
    const QString base = require.section(QLatin1Char('/'), 0, 0);
    static const QRegularExpression re(QStringLiteral("[\\[*]"));  // '[', '*'
    const int idx = base.indexOf(re);
    return (idx > 0) ? base.left(idx) : base;
}

}  // namespace

QStringList ConanManager::packageNames(const proj::Project& project)
{
    QStringList names;
    for (const QString& d : project.dependencies) {
        const QString n = packageNameBase(d);
        if (!n.isEmpty())
            names << n;
    }
    return names;
}

QString ConanManager::conanfileContent(const proj::Project& project)
{
    QString out = QStringLiteral(
        "# Gerado pela IDE Editor (Parte 4).\n"
        "# Uso: conan install . --output-folder=build/conan\n\n"
        "[requires]\n");
    if (project.dependencies.isEmpty())
        out += QStringLiteral("# (nenhuma dependencia declarada)\n");
    for (const QString& d : project.dependencies)
        out += d + QLatin1Char('\n');
    out += QStringLiteral(
        "\n[generators]\n"
        "PkgConfigDeps\n");
    return out;
}

QString ConanManager::writeConanfile(const proj::Project& project)
{
    if (project.dependencies.isEmpty())
        return QString();
    const QString path = QDir(project.rootDir).filePath(QStringLiteral("conanfile.txt"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(conanfileContent(project).toUtf8());
        f.close();
        return path;
    }
    return QString();
}

}  // namespace build