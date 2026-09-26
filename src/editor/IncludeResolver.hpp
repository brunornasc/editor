// IncludeResolver.hpp
#pragma once

#include <QString>
#include <QStringList>

namespace editor {

//! Resolve `#include "x.h"` / `#include <x>` para um caminho absoluto.
//!
//! A ordem importa: o que esta escrito entre aspas no codigo do usuario
//! quase sempre e um header do proprio projeto, entao os diretorios do projeto
//! vem antes dos diretorios do sistema. Para `<...>` vale a ordem do
//! compilador (system paths).
class IncludeResolver {
public:
    //! Diretores onde procurar (absolutos), em ordem de prioridade.
    void setSearchPaths(const QStringList& dirs) { m_dirs = dirs; }
    QStringList searchPaths() const { return m_dirs; }

    //! Diretorios do projeto: raiz + `include/`, `src/`, `inc/`, ...
    void setProjectDirs(const QStringList& dirs) { m_projectDirs = dirs; }

    //! `spec` e' como aparece no codigo: `<vector>` ou `"utils/log.h"`.
    //! Devolve caminho absoluto, ou string vazio se nao achar.
    QString resolve(const QString& spec) const;

    //! Igual a `resolve`, mas o include nao precisa estar completo: usado
    //! enquanto o usuario digita (`#include <vec`).
    //! `prefix` = o que ja foi digitado dentro dos sinais.
    //! Devolve os candidatos ordenados (o que casa primeiro por cima).
    QStringList complete(const QString& prefix, bool angled) const;

private:
    QStringList m_dirs;          // system + projeto, prioridade
    QStringList m_projectDirs;   // apenas os do projeto (vem antes)
};

}  // namespace editor
