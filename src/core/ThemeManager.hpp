#pragma once

#include <QString>

#include <string>

class QApplication;

namespace core {

//! Responsavel por carregar e aplicar o tema (QSS) VS2005 na aplicacao.
class ThemeManager final {
public:
    //! Aplica o QSS embutido via resource (:/theme/vs2005.qss).
    static void apply(QApplication& app);

    //! Exposicao do tema em forma textual (para testes / tooling).
    static QString stylesheet();
};

}  // namespace core