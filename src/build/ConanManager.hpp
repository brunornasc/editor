#pragma once

#include <QString>
#include <QStringList>

namespace proj {
struct Project;
}

namespace build {

//! Suporte a Conan 2 (PkgConfigDeps): gera `conanfile.txt` a partir das
//! dependencias do projeto e expoe os nomes de pacote usados via pkg-config.
class ConanManager {
public:
    //! Nome do pacote ("fmt/10.2.1" -> "fmt"); nomes arquitetonicos
    //! ("fmt/[>=10,<11]") sao reduzidos para a primeira parte.
    static QStringList packageNames(const proj::Project& project);

    //! Gera o conteudo do conanfile.txt para o projeto.
    static QString conanfileContent(const proj::Project& project);

    //! Grava rootDir/conanfile.txt se houver dependencias. Retorna o caminho
    //! (vazio se o projeto nao tem dependencias).
    static QString writeConanfile(const proj::Project& project);
};

}  // namespace build