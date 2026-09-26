#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace editor {

//! Natureza de um simbolo declarado no codigo.
enum class SymbolKind : int {
    Unknown,
    Namespace,
    Class,
    Struct,
    Union,
    Enum,
    EnumItem,
    Function,   // funcao livre
    Method,     // funcao/membro de classe
    Field,      // campo de classe/struct
    Variable,   // variavel global ou de escopo
    Typedef,    // typedef / using X = ...
    Macro,      // #define
};

//! Um simbolo (escopo ou membro) com sua arvore de filhos.
struct SymbolNode {
    QString name;                // nome simples ("cout", "basic_string")
    SymbolKind kind = SymbolKind::Unknown;
    QString signature;           // linha de declaracao (tooltip)
    QString file;                // arquivo onde foi declarado
    int line = -1;               // linha 0-based
    QString qualifiedName;       // "std::literals" / "std::string" (vazio se global)
    bool isOutOfLine = false;    // definido fora do corpo (Foo::bar)
    //! Namespace `inline` (std::__cxx11): os membros aparecem tambem no
    //! namespace pai, entao o indice oferece `std::string` e nao so
    //! `std::__cxx11::string`.
    bool isInline = false;
    QVector<SymbolNode> children;

    //! Escopos que possuem membros (namespace/classe/struct/enum).
    bool isScope() const
    {
        return kind == SymbolKind::Namespace || kind == SymbolKind::Class
            || kind == SymbolKind::Struct || kind == SymbolKind::Union
            || kind == SymbolKind::Enum;
    }
};

//! Arvore de simbolos de um arquivo (header ou .cpp).
struct SymbolTree {
    //! Escopos de topo (namespace std, class Foo, ...) e simbolos livres.
    QVector<SymbolNode> roots;
    //! "using namespace std;" declarados no arquivo (usados no intellisense).
    QStringList usingNamespaces;
    //! cabecalhos incluidos pelo proprio arquivo ("<iostream>", "\"x.h\"").
    QStringList includes;

    //! Acha um simbolo pelo nome (simples ou qualificado), em profundidade.
    const SymbolNode* find(const QString& name) const;
};

//! Percorre a arvore em profundidade, chamando `fn` para cada no.
template <typename Fn>
void forEachSymbol(const SymbolTree& tree, Fn fn)
{
    QVector<const SymbolNode*> stack;
    for (const SymbolNode& r : tree.roots)
        stack.append(&r);
    while (!stack.isEmpty()) {
        const SymbolNode* n = stack.takeLast();
        fn(*n);
        for (const SymbolNode& c : n->children)
            stack.append(&c);
    }
}

}  // namespace editor
