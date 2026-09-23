#pragma once

#include <QString>
#include <QVector>

namespace editor {

//! Tipo sintatico de um escopo top-level (nav bar esquerda).
enum class ScopeKind : int {
    Namespace,
    Class,
    Struct,
    Enum,
};

//! Membro/declaracao dentro de um escopo (nav bar direita).
struct OutlineMember {
    QString name;       // nome simples (ex.: "show")
    QString signature;  // linha de assinatura (para tooltip)
    int line = -1;      // linha 0-based no arquivo
    bool isFunction = false;
    bool isOutOfLine = false;  // definido fora do corpo da classe (foo::bar)
};

//! Escopo de alto nivel (classe/struct/namespace/enum).
struct OutlineScope {
    QString name;
    QString fullPath;   // Ex.: "app::ui::MainWindow"
    ScopeKind kind = ScopeKind::Class;
    int line = -1;          // linha 0-based da declaracao
    int openDepth = -1;     // profundidade de chaves no ponto de abertura
    int parent = -1;        // indice em scopes[] do pai (-1 => global)
    QVector<OutlineMember> members;
};

//! Outline completo de um arquivo C/C++.
//!
//! `scopes` contem TODOS os escopos detectados (inclusive aninhados).
//! `globalMembers` sao funcoes fora de qualquer classe/namespace.
struct OutlineData {
    QVector<OutlineScope> scopes;
    QVector<OutlineMember> globalMembers;
};

//! Parser heuristico de outline (regex de escopos + rastreio de chaves).
//!
//! NAO e um parser completo (Ctags/AST ficam como evolucao na Parte 3);
//! basta para as navigation bars. Limitacoes aceitas:
//!   * templates em varias linhas nao sao reconhecidos;
//!   * '{}' dentro de preprocessamento nao mapeado (usamos texto limpo);
//!   * declaracoes com sintaxe exotica podem nao ser indexadas.
class CodeOutlineParser final {
public:
    //! Re-analisa o texto completo do documento.
    static OutlineData parse(const QString& text);

private:
    //! Troca por espacos comentarios // /* */ e literais "..."/'...',
    //! preservando o tamanho da linha (posicoes de chaves se mantem).
    static QString sanitizeLine(const QString& line, bool& inBlockComment);
};

}  // namespace editor