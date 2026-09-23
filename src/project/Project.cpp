#include "project/Project.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDebug>

namespace proj {

Project Project::create(const QString& name, const QString& rootDir,
                        const QStringList& files,
                        const QString& compiler, const QString& makeTarget)
{
    Project p;
    p.name = name;
    p.rootDir = QDir(rootDir).absolutePath();
    p.compiler = compiler;
    p.makeTarget = makeTarget.isEmpty() ? name : makeTarget;
    p.files = files;
    return p;
}

bool Project::load(const QString& jsonPath)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "Project: nao foi possivel ler" << jsonPath;
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Project: JSON invalido em" << jsonPath
                   << ":" << err.errorString();
        return false;
    }

    const QJsonObject o = doc.object();
    name = o.value("name").toString();
    compiler = o.value("compiler").toString();
    makeTarget = o.value("makeTarget").toString();
    dependencies.clear();
    for (const QJsonValue v : o.value("dependencies").toArray())
        dependencies << v.toString();

    files.clear();
    for (const QJsonValue v : o.value("files").toArray())
        files << v.toString();

    breakpoints.clear();
    for (const QJsonValue v : o.value("breakpoints").toArray()) {
        const QJsonObject bo = v.toObject();
        BreakpointRecord r;
        r.file = bo.value("file").toString();
        r.line = bo.value("line").toInt();
        r.enabled = bo.value("enabled").toBool(true);
        if (!r.file.isEmpty() && r.line >= 0)
            breakpoints.append(r);
    }

    rootDir = QFileInfo(jsonPath).absolutePath();
    if (name.isEmpty())
        name = QFileInfo(jsonPath).completeBaseName();
    if (compiler.isEmpty())
        compiler = QStringLiteral("g++");
    // Projetos antigos usavam "all" como target; agora e o nome do exe.
    if (makeTarget.isEmpty() || makeTarget == QStringLiteral("all"))
        makeTarget = name;

    return isValid();
}

bool Project::save(const QString& jsonPath) const
{
    QFileInfo info(jsonPath);
    if (!info.dir().exists())
        QDir().mkpath(info.absolutePath());

    QJsonObject o;
    o["name"] = name;
    o["compiler"] = compiler;
    o["makeTarget"] = makeTarget;

    QJsonArray arr;
    for (const QString& f : files)
        arr.append(f);
    o["files"] = arr;

    QJsonArray deps;
    for (const QString& d : dependencies)
        deps.append(d);
    o["dependencies"] = deps;

    QJsonArray bps;
    for (const BreakpointRecord& r : breakpoints) {
        QJsonObject bo;
        bo["file"] = r.file;
        bo["line"] = r.line;
        bo["enabled"] = r.enabled;
        bps.append(bo);
    }
    o["breakpoints"] = bps;

    QFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Project: falha ao salvar" << jsonPath;
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

QString Project::fileName(const QString& rel) const
{
    return rootDir + QLatin1Char('/') + rel;
}

}  // namespace proj