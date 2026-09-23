#include "core/ThemeManager.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <QDebug>

namespace core {

namespace {

constexpr auto kResourceQss = ":/vs2005.qss";

QString readQss()
{
    QFile file(kResourceQss);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(file.readAll());
    }

    // Fallback: tenta ler do disco (configuracoes de dev/customizacao).
    const QString disk = QDir::current().filePath("resources/vs2005.qss");
    if (QFileInfo::exists(disk)) {
        QFile fd(disk);
        if (fd.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString::fromUtf8(fd.readAll());
        }
    }

    qWarning() << "ThemeManager: nao foi possivel carregar o QSS" << kResourceQss;
    return {};
}

}  // namespace

QString ThemeManager::stylesheet()
{
    return readQss();
}

void ThemeManager::apply(QApplication& app)
{
    const QString qss = stylesheet();
    if (!qss.isEmpty()) {
        app.setStyleSheet(qss);
    }
}

}  // namespace core