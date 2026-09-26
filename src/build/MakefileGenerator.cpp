#include "build/MakefileGenerator.hpp"

#include "build/ConanManager.hpp"

#include "project/Project.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace build {

namespace {

//! Extensoes consideradas fontes compilaveis para o Makefile.
bool isSourceFile(const QString& rel)
{
    const QString ext = QFileInfo(rel).suffix().toLower();
    return ext == QLatin1String("cpp") || ext == QLatin1String("cc") ||
           ext == QLatin1String("cxx") || ext == QLatin1String("c");
}

}  // namespace

QString MakefileGenerator::generate(const proj::Project& p)
{
    QStringList sources;
    QStringList objs;
    int idx = 0;
    for (const QString& rel : p.files) {
        if (!isSourceFile(rel))
            continue;
        const QString obj = QStringLiteral("build/obj/%1_%2.o")
                                .arg(QFileInfo(rel).completeBaseName())
                                .arg(idx++);
        sources << rel;
        objs << obj;
    }

    QString out = QStringLiteral(
        "# Gerado pela IDE Editor (Parte 4).\n"
        "# Uso: make [all|clean|rebuild] [CONFIG=Debug|Release]\n\n");
    out += QStringLiteral("CXX := %1\n").arg(p.compiler);
    out += QStringLiteral("TARGET := build/%1\n\n").arg(p.makeTarget);

    out += QStringLiteral(
        "CXXFLAGS := -std=c++17 -Wall -Wextra -O0 -g3\n"
        "ifeq ($(CONFIG),Release)\n"
        "CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -DNDEBUG\n"
        "endif\n\n");

    const QStringList pkgNames = ConanManager::packageNames(p);
    if (!pkgNames.isEmpty()) {
        out += QStringLiteral(
            "# Dependencias Conan via PkgConfigDeps (`conan install -of build/conan`)\n"
            "CONAN_PKG_CONFIG_PATH := build/conan\n");
        out += QStringLiteral("CONAN_PACKAGES := %1\n\n")
                   .arg(pkgNames.join(QLatin1Char(' ')));
        out += QStringLiteral(
            "ifeq ($(strip $(CONAN_PACKAGES)),)\n"
            "CONAN_CFLAGS :=\n"
            "CONAN_LIBS :=\n"
            "else\n"
            "CONAN_CFLAGS := $(shell PKG_CONFIG_PATH=\"$(CONAN_PKG_CONFIG_PATH)\" "
            "pkg-config --cflags 2>/dev/null $(CONAN_PACKAGES))\n"
            "CONAN_LIBS := $(shell PKG_CONFIG_PATH=\"$(CONAN_PKG_CONFIG_PATH)\" "
            "pkg-config --libs 2>/dev/null $(CONAN_PACKAGES))\n"
            "endif\n\n");
    } else {
        out += QStringLiteral("CONAN_CFLAGS :=\nCONAN_LIBS :=\n\n");
    }

    for (int i = 0; i < sources.size(); ++i) {
        out += QStringLiteral("%1: %2\n"
                              "\tmkdir -p build/obj\n"
                              "\t$(CXX) $(CXXFLAGS) $(CONAN_CFLAGS) -c \"$<\" -o \"$@\"\n\n")
                   .arg(objs[i], sources[i]);
    }

    out += QStringLiteral(
        "all: $(TARGET)\n\n"
        "$(TARGET): %1\n"
        "\t$(CXX) $(CXXFLAGS) -o \"$@\" %1 $(CONAN_LIBS)\n\n")
               .arg(objs.join(QLatin1Char(' ')));

    out += QStringLiteral(
        "clean:\n"
        "\trm -rf build\n\n"
        "rebuild: clean all\n\n"
        ".PHONY: all clean rebuild\n");
    return out;
}

bool MakefileGenerator::writeToDisk(const proj::Project& p)
{
    const QString makePath = QDir(p.rootDir).filePath(QStringLiteral("Makefile"));
    QDir(p.rootDir).mkpath(QStringLiteral("build"));

    QFile f(makePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(generate(p).toUtf8());
        f.close();
        return true;
    }
    return false;
}

}  // namespace build