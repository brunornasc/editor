#include "editor/SymbolTree.hpp"

namespace editor {

const SymbolNode* SymbolTree::find(const QString& name) const
{
    const SymbolNode* found = nullptr;
    forEachSymbol(*this, [&](const SymbolNode& n) {
        if (!found && (n.name == name || n.qualifiedName == name))
            found = &n;
    });
    return found;
}

}  // namespace editor
