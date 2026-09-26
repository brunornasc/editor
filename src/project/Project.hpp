#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace proj {

//! Um breakpoint persistido por projeto (line e 0-based, como no editor).
struct BreakpointRecord {
    QString file;    // caminho absoluto
    int line = -1;   // 0-based
    bool enabled = true;
};

//! Projeto da IDE. Persistido como JSON (*.json / *.vproj).
//!
//! `makeTarget` e o NOME DO EXECUTAVEL (ex.: "MeuProjeto") -- o gerador de
//! Makefile (Parte 4) produz `build/<makeTarget>`. `dependencies` sao
//! requisitos Conan (ex.: "fmt/10.2.1"), integrados ao build via
//! `conan install` + PkgConfigDeps + pkg-config.
struct Project {
    QString name;
    QString rootDir;      // diretorio absoluto do projeto
    QString compiler;     // identificador de toolchain ("g++","clang++",...)
    //! Flags passadas ao preprocessador do intellisense ("-std=c++20", ...).
    QStringList compilerFlags;
    //! Diretorios de include extras, relativos a rootDir ("include", "src").
    QStringList includeDirs;
    QString makeTarget;   // nome do executavel (padrao: nome do projeto)
    QStringList files;    // caminhos relativos a rootDir, separador '/'
    QStringList dependencies;  // requisitos Conan ("pacote/versao", ...)
    QVector<BreakpointRecord> breakpoints;  // state de debug persistido

    static Project create(const QString& name, const QString& rootDir,
                          const QStringList& files,
                          const QString& compiler = QStringLiteral("g++"),
                          const QString& makeTarget = QString());

    //! Carrega de um arquivo JSON de projeto. rootDir = dir do json.
    bool load(const QString& jsonPath);

    //! Salva em JSON (criando o diretorio pai quando necessario).
    bool save(const QString& jsonPath) const;

    //! Caminho absoluto de um arquivo do projeto.
    QString fileName(const QString& rel) const;

    //! includeDirs (relativos) resolvidos contra rootDir + os padroes
    //! (raiz, include/, src/, inc/, third_party/include/).
    QStringList resolvedIncludeDirs() const;

    //! Flags do compilador + -I de cada includeDir, prontos para o `g++ -E`.
    QStringList preprocessFlags() const;

    //! Compiladores encontrados no PATH, com o do projeto primeiro se existir.
    static QStringList detectCompilers();

    QString displayName() const { return name; }
    bool isValid() const { return !rootDir.isEmpty() && !name.isEmpty(); }
};

}  // namespace proj