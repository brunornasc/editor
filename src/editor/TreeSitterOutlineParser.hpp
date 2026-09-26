#pragma once

#include "editor/CodeOutlineParser.hpp"  // OutlineData/OutlineScope (nav bars)
#include "editor/SymbolTree.hpp"

#include <tree_sitter/api.h>

namespace editor {

//! Parser de outline C++ via Tree-sitter (gramatica vendorizada em
//! `vendor/tree-sitter-cpp`).
//!
//! Produz uma arvore de simbolos (`SymbolTree`) com namespace/class/struct/
//! enum aninhados, membros, typedefs, variaveis e macros -- cada um com
//! arquivo e linha, para o intellisense e o "ir para declaracao".
//!
//! NAO e thread-safe: cada thread/worker deve ter a sua instancia.
class TreeSitterOutlineParser final {
public:
    TreeSitterOutlineParser();
    ~TreeSitterOutlineParser();

    TreeSitterOutlineParser(const TreeSitterOutlineParser&) = delete;
    TreeSitterOutlineParser& operator=(const TreeSitterOutlineParser&) = delete;

    //! Arvore de simbolos do texto. `filePath` so entra nos simbolos.
    SymbolTree parseSymbols(const QString& text, const QString& filePath = QString());

    //! Atalho: arvore de simbolos convertida no formato antigo das nav bars.
    OutlineData parse(const QString& text);

    //! Converte a arvore de simbolos no formato das navigation bars.
    static OutlineData toOutlineData(const SymbolTree& tree);

private:
    TSParser* m_parser = nullptr;
};

}  // namespace editor
