// Preprocessor.hpp
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace editor {

//! Mapa de linhas: saida do preprocessador -> arquivo/linha originais.
//!
//! `g++ -E`Achata tudo num arquivo so e usa "linemarkers"
//! (`# 306 "bits/c++config.h" 3`) para dizer de onde cada linha veio. Como o
//! Tree-Sitter parseia o arquivo achatado, o numero de linha que ele devolve
//! nao serve para "ir para declaracao" -- sem este mapa o usuario cairia na
//! linha 11581 de um temporario em vez da linha 428 de stl_vector.h.
struct SourceMap {
    QVector<QString> files;  // por linha 0-based do texto preprocessado
    QVector<int> lines;      // 0-based

    //! Arquivo e linha originais de uma linha do texto preprocessado.
    void lookup(int row, QString* file, int* line) const
    {
        if (row < 0 || row >= files.size() || row >= lines.size()) {
            if (file)
                file->clear();
            if (line)
                *line = -1;
            return;
        }
        if (file)
            *file = files.at(row);
        if (line)
            *line = lines.at(row);
    }
};

//! Resultado de preprocessar um arquivo com o compilador do projeto.
struct PreprocessResult {
    //! Texto gerado por `compilador -E` (vazio se falhar).
    QString text;
    //! true se o compilador rodou e devolveu texto.
    bool ok = false;
    SourceMap map;
    //! Mensagem de erro do compilador (util para log/diagnostico).
    QString error;
};

//! Executa o pre-processador do compilador do projeto.
//!
//! Headers do libstdc++ nao servem para indexacao sem passar por aqui:
//! `<vector>` tem 151 linhas e so faz `#include <bits/stl_vector.h>` -- o nome
//! `std::vector` nao e declarado no arquivo que o usuario inclui. Com o
//! preprocessador o header chega "pronto" para o Tree-Sitter: medido em
//! <vector> 3 simbolos (raso) contra 2365 (preprocessado), e <iostream> 5212.
class Preprocessor {
public:
    explicit Preprocessor(QString compiler = QStringLiteral("g++"));

    void setCompiler(const QString& compiler) { m_compiler = compiler; }
    QString compiler() const { return m_compiler; }

    //! Flags extras do projeto (-std=c++17, -I..., defines).
    void setFlags(const QStringList& flags) { m_flags = flags; }
    QStringList flags() const { return m_flags; }

    //! Preprocessa `filePath` (caminho absoluto).
    PreprocessResult run(const QString& filePath) const;

    //! Include paths que o compilador usa, lidos de `-E -v` (com as flags).
    //! Vazio se o compilador nao responder.
    QStringList includePaths() const;

    //! Diretorios onde procurar headers, na ordem de prioridade: o do arquivo
    //! e o `includePaths()` do compilador.
    QStringList searchPaths(const QString& filePath) const;

private:
    QString m_compiler;
    QStringList m_flags;
};

}  // namespace editor
