#pragma once

#include <QString>

namespace proj {
struct Project;
}

namespace build {

//! Gera um Makefile GNU para o projeto (estilo VS2005: target nomeado pelo
//! projeto, pasta build/, selection Debug/Release via variavel CONFIG).
//!
//! Integracao Conan: se o projeto declara dependencias, o Makefile e emitido
//! com PKG_CONFIG_PATH=build/conan (PkgConfigDeps) e usa `pkg-config --cflags
//! --libs` para injetar includes/libs.
class MakefileGenerator {
public:
    //! Conteudo textual do Makefile.
    static QString generate(const proj::Project& project);

    //! Grava Makefile (e build/) em rootDir do projeto. Conanfile e criado
    //! separadamente por ConanManager. Retorna false em falha de escrita.
    static bool writeToDisk(const proj::Project& project);
};

}  // namespace build