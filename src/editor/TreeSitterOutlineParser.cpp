#include "TreeSitterOutlineParser.hpp"
#include "TreeSitterQueries.hpp"

#include <QHash>
#include <QDebug>
#include <QRegularExpression>

#include <cstring>
#include <cstdio>
#include <functional>
#include <algorithm>

extern "C" const TSLanguage* tree_sitter_cpp();

namespace editor {

namespace {

//! Nao e a arvore do C++ do projeto; evita conflito com o namespace editor.
const char* kNodeTypeNamespace = "namespace_definition";
const char* kNodeTypeClass = "class_specifier";
const char* kNodeTypeStruct = "struct_specifier";
const char* kNodeTypeUnion = "union_specifier";
const char* kNodeTypeEnum = "enum_specifier";

bool isScopeNodeType(const char* type)
{
    return std::strcmp(type, kNodeTypeNamespace) == 0
        || std::strcmp(type, kNodeTypeClass) == 0
        || std::strcmp(type, kNodeTypeStruct) == 0
        || std::strcmp(type, kNodeTypeUnion) == 0
        || std::strcmp(type, kNodeTypeEnum) == 0;
}

SymbolKind kindForScopeNode(const char* type)
{
    if (std::strcmp(type, kNodeTypeNamespace) == 0)
        return SymbolKind::Namespace;
    if (std::strcmp(type, kNodeTypeClass) == 0)
        return SymbolKind::Class;
    if (std::strcmp(type, kNodeTypeStruct) == 0)
        return SymbolKind::Struct;
    if (std::strcmp(type, kNodeTypeUnion) == 0)
        return SymbolKind::Union;
    return SymbolKind::Enum;
}

//! Uma captura de nome (um simbolo candidato) aguardando montagem.
struct Pending {
    int kind = 0;             // SymbolKind
    TSNode nameNode;
    TSNode defNode;           // no que declara (p/ escopo) ou nullptr
    uint32_t startByte = 0;
    int line = 0;             // 0-based
    QString text;             // texto do nome capturado
    QString signature;        // linha de declaracao
};

int kindFromCapture(const QString& capture)
{
    if (capture == QLatin1String("scope.name"))
        return int(SymbolKind::Namespace);  // refined later from the node type
    if (capture == QLatin1String("func.name"))
        return int(SymbolKind::Function);
    if (capture == QLatin1String("field.name"))
        return int(SymbolKind::Field);
    if (capture == QLatin1String("typedef.name"))
        return int(SymbolKind::Typedef);
    if (capture == QLatin1String("var.name"))
        return int(SymbolKind::Variable);
    if (capture == QLatin1String("enum_item.name"))
        return int(SymbolKind::EnumItem);
    if (capture == QLatin1String("macro.name"))
        return int(SymbolKind::Macro);
    return -1;
}

//! Texto da linha 0-based `line` do buffer.
QString lineText(const QByteArray& bytes, int line)
{
    int start = 0;
    for (int i = 0; i < line; ++i) {
        const int nl = bytes.indexOf('\n', start);
        if (nl < 0)
            return {};
        start = nl + 1;
    }
    int end = bytes.indexOf('\n', start);
    if (end < 0)
        end = bytes.size();
    return QString::fromUtf8(bytes.mid(start, end - start)).trimmed();
}

constexpr int kMaxSignatureLen = 160;

QString shortenSignature(const QString& sig)
{
    QString s = sig.simplified();
    if (s.size() > kMaxSignatureLen)
        s = s.left(kMaxSignatureLen) + QStringLiteral("...");
    return s;
}

//! Ultimo token identificador do texto capturado. Rede de seguranca: mesmo com
//! a query pegando o no certo, algumas declaracoes do libstdc++ vem como um
//! qualified-id que comeca linhas antes -- o nome e o ultimo token, nao o texto.
//! Header do libstdc++ tem macros e atributos que a gramatica nao digere, e
//! cada um deles quebra o parse do arquivo inteiro (a raiz vira `ERROR` e as
//! classes deixam de ficar dentro de `namespace std`):
//!
//!  1. Linha solta com um token em maiusculas -- `_GLIBCXX_BEGIN_NAMESPACE_VERSION`
//!     aparece dentro do corpo do namespace, sem `#`. Nunca e declaracao C++.
//!  2. Macro colada no nome do namespace -- `namespace std _GLIBCXX_VISIBILITY(default)`.
//!     Em C++11 so atributo `[[...]]` e valido depois do nome.
//!  3. `__attribute__((...))`: em `inline namespace __cxx11 __attribute__((__abi_tag__("cxx11")))`
//!     o no `namespace_identifier` da gramatica termina la dentro e o nome sai
//!     errado ("cxx11" em vez de "__cxx11"). Pior: em `<iostream>` o
//!     `_GLIBCXX_VISIBILITY(default)` e expandido com parenteses aninhados
//!     **varias linhas** -- `__attribute__ ((__visibility__ (` / `"default"` / `)))`
//!     -- e a gramatica nao aceita atributo nessa posicao: o `namespace std`
//!     inteiro vira `ERROR` e `std::cout`, `std::cin`... ficam sem prefixo.
//!
//! Tudo vira espacos do mesmo tamanho, **preservando os '\\n'**: e o SourceMap
//! que traduz linha da saida preprocessada para arquivo/linha do original, e
//! ele conta '\\n'. `[[...]]` de C++11 fica intacto, e uma linha com
//! `_GLIBCXX20_CONSTEXPR` isolado e apenas `constexpr` ou nada.

//! Apaga [from, from+len) trocando por espacos, mas mantem as quebras de linha.
void blankRange(QChar* data, int from, int len)
{
    for (int i = from; i < from + len; ++i) {
        if (data[i] != QLatin1Char('\n'))
            data[i] = QLatin1Char(' ');
    }
}

//! Compara `QChar` com uma palavra de `char`. `memcmp` nao serve: `QChar` tem
//! 2 bytes, entao o byte 0 do segundo caractere ja nao bate e nada casa.
bool startsWithAt(const QChar* d, int size, int i, const char* palavra)
{
    const int len = int(std::strlen(palavra));
    if (i + len > size)
        return false;
    for (int k = 0; k < len; ++k) {
        if (d[i + k] != QLatin1Char(palavra[k]))
            return false;
    }
    return true;
}

//! Uma passada so, com estados: comentario e literal tem prioridade sobre
//! palavra-chave -- `// don't` com apostrofo e `/* "aspas" */` abortam uma
//! varredura que tratasse aspas antes de comentario, e a contagem de parenteses
//! vira lixo: o apagamento come a `{` do namespace e o scope inteiro se perde.
int matchingParen(const QChar* d, int size, int open)
{
    int depth = 0;
    for (int i = open; i < size; ++i) {
        const QChar c = d[i];
        if (c == QLatin1Char('/') && i + 1 < size && d[i + 1] == QLatin1Char('/')) {
            while (i < size && d[i] != QLatin1Char('\n'))
                ++i;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < size && d[i + 1] == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < size && !(d[i] == QLatin1Char('*') && d[i + 1] == QLatin1Char('/')))
                ++i;
            ++i;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            for (++i; i < size; ++i) {
                if (d[i] == QLatin1Char('\\')) {
                    ++i;
                    continue;
                }
                if (d[i] == quote)
                    break;
            }
            continue;
        }
        if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')')) {
            if (--depth == 0)
                return i;
        }
    }
    return -1;
}

//! `__attribute__` / `__declspec` / `alignas` followed de `(`, e `__extension__`.
//! Aninhamento arbitrario, varias linhas, com guarda: um atributo nunca contem
//! `{`, `}` nem `;` -- se o trecho casado tiver um deles, o casamento estava
//! errado e nada e apagado.
void blankAttributes(QChar* d, int size)
{
    static const char* kComLista[] = {"__attribute__", "__attribute", "__declspec", "alignas"};
    for (int i = 0; i < size; ++i) {
        const QChar c = d[i];
        if (c != QLatin1Char('_') && c != QLatin1Char('a'))
            continue;
        const bool fronteira = (i == 0) || !(d[i - 1].isLetterOrNumber() || d[i - 1] == QLatin1Char('_'));
        if (!fronteira)
            continue;
        if (startsWithAt(d, size, i, "__extension__")) {
            blankRange(d, i, 13);
            i += 12;
            continue;
        }
        for (const char* chave : kComLista) {
            const int len = int(std::strlen(chave));
            if (!startsWithAt(d, size, i, chave))
                continue;
            if (i + len < size && (d[i + len].isLetterOrNumber() || d[i + len] == QLatin1Char('_')))
                continue;
            int j = i + len;
            while (j < size && (d[j] == QLatin1Char(' ') || d[j] == QLatin1Char('\t')))
                ++j;
            if (j >= size || d[j] != QLatin1Char('('))
                break;  // `__attribute` solto, sem lista: nao ha o que varrer
            const int close = matchingParen(d, size, j);
            if (close < 0)
                break;
            bool seguro = true;
            for (int k = i; k <= close; ++k) {
                if (d[k] == QLatin1Char('{') || d[k] == QLatin1Char('}') || d[k] == QLatin1Char(';')) {
                    seguro = false;
                    break;
                }
            }
            if (seguro)
                blankRange(d, i, close - i + 1);
            i = seguro ? close : j;
            break;
        }
    }
}

QString sanitizeForParse(const QString& text)
{
    static const QRegularExpression reLinhaSolta(
        QStringLiteral("^([ \\t]*)([A-Z_][A-Z0-9_]*)([ \\t]*(?://[^\\n]*)?$)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression reNamespace(
        QStringLiteral("^([ \\t]*namespace[ \\t]+[A-Za-z_][\\w:]*)"
                       "([ \\t]+[A-Z_][A-Z0-9_]*\\([^\\n]*\\))"),
        QRegularExpression::MultilineOption);

    const auto blank = [](QString& out, const QRegularExpression& re) {
        auto it = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out.replace(m.capturedStart(), m.capturedLength(),
                        QString(m.capturedLength(), QLatin1Char(' ')));
        }
    };

    if (!reLinhaSolta.match(text).hasMatch() && !reNamespace.match(text).hasMatch()
        && !text.contains(QLatin1String("__attribute")) && !text.contains(QLatin1String("__declspec"))
        && !text.contains(QLatin1String("alignas"))
        && !text.contains(QLatin1String("__extension__"))) {
        return text;
    }

    QString out = text;
    blank(out, reLinhaSolta);
    blank(out, reNamespace);
    blankAttributes(out.data(), out.size());
    return out;
}

//! `inline namespace __cxx11 { }`: a gramatica nao da campo pro `inline`, ele e
//! um token anonimo antes do nome, entao procura-se o texto entre o `namespace`
//! e o nome.
bool isInlineNamespace(TSNode defNode, const QByteArray& bytes)
{
    for (uint32_t i = 0; i < ts_node_child_count(defNode); ++i) {
        const TSNode c = ts_node_child(defNode, i);
        const uint32_t sb = ts_node_start_byte(c);
        const uint32_t n = ts_node_end_byte(c) - sb;
        if (n == 6 && bytes.mid(int(sb), 6) == QByteArrayLiteral("inline"))
            return true;
    }
    return false;
}

QString lastIdentifier(const QString& text)
{
    const auto isId = [](QChar c) {
        return c.isLetterOrNumber() || c == QLatin1Char('_');
    };
    int start = text.size();
    while (start > 0 && isId(text.at(start - 1)))
        --start;
    int end = start;
    while (end < text.size() && isId(text.at(end)))
        ++end;
    return text.mid(start, end - start);
}

}  // namespace

// ---------------------------------------------------------------------------

TreeSitterOutlineParser::TreeSitterOutlineParser()
{
    m_parser = ts_parser_new();
    ts_parser_set_language(m_parser, tree_sitter_cpp());
}

TreeSitterOutlineParser::~TreeSitterOutlineParser()
{
    if (m_parser)
        ts_parser_delete(m_parser);
}

SymbolTree TreeSitterOutlineParser::parseSymbols(const QString& text,
                                                 const QString& filePath,
                                                 const SourceMap* map)
{
    SymbolTree out;

    // Os #includes sao lidos por regex: valem inclusive para arquivos que a
    // arvore sintatica nao modela (o include e preprocessamento).
    {
        static const QRegularExpression re(
            QStringLiteral("^[ \\t]*#[ \\t]*include[ \\t]*([<\"])([^>\"]+)[>\"]"),
            QRegularExpression::MultilineOption);
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out.includes.append(m.captured(1) + m.captured(2)
                                + (m.captured(1) == QLatin1String("<")
                                       ? QStringLiteral(">")
                                       : QStringLiteral("\"")));
        }
    }

    const QByteArray bytes = text.toUtf8();
    if (!m_parser || bytes.isEmpty())
        return out;

    // Mesmos bytes de tamanho, mesmas linhas: so a arvore muda.
    const QByteArray parseBytes = sanitizeForParse(text).toUtf8();

    // Parse completo (sem reuso incremental: a arvore antiga nao recebe as
    // TSInputEdit do buffer, reaproveita-la produziria nos invalidos).
    TSTree* tree = ts_parser_parse_string(m_parser, nullptr, parseBytes.constData(),
                                          static_cast<uint32_t>(parseBytes.size()));
    if (!tree)
        return out;

    uint32_t errOffset = 0;
    TSQueryError errType = TSQueryErrorNone;
    TSQuery* query = ts_query_new(tree_sitter_cpp(), K_CPP_SYMBOL_QUERY,
                                  static_cast<uint32_t>(std::strlen(K_CPP_SYMBOL_QUERY)),
                                  &errOffset, &errType);
    if (!query) {
        qWarning("TreeSitter: query de simbolos invalida (offset %u, erro %d)",
                 errOffset, int(errType));
        ts_tree_delete(tree);
        return out;
    }


    // --- 1) coleta as capturas de nome -----------------------------------
    QVector<Pending> pending;
    {
        TSQueryCursor* cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match)) {
            for (uint16_t i = 0; i < match.capture_count; ++i) {
                const TSQueryCapture c = match.captures[i];
                uint32_t nameLen = 0;
                const char* capName =
                    ts_query_capture_name_for_id(query, c.index, &nameLen);
                const QString cap = QString::fromUtf8(capName, nameLen);
                const int kind = kindFromCapture(cap);
                if (kind < 0)
                    continue;

                Pending p;
                p.kind = kind;
                p.nameNode = c.node;
                p.startByte = ts_node_start_byte(c.node);
                p.line = int(ts_node_start_point(c.node).row);
                p.text = lastIdentifier(QString::fromUtf8(
                    bytes.mid(int(p.startByte),
                              int(ts_node_end_byte(c.node)) - int(p.startByte))));
                p.signature = shortenSignature(lineText(bytes, p.line));
                p.defNode = c.node;
                if (kind == int(SymbolKind::Namespace)) {
                    // sobe ate a declaracao do escopo (namespace/class/...)
                    TSNode up = ts_node_parent(c.node);
                    while (!ts_node_is_null(up)
                           && !isScopeNodeType(ts_node_type(up))) {
                        up = ts_node_parent(up);
                    }
                    p.defNode = up;
                }
                pending.append(p);
            }
        }
        ts_query_cursor_delete(cursor);
    }

    // --- 2) monta a arvore em ordem de declaracao ------------------------
    // Ordenar por byte inicial garante que um escopo sempre e criado antes
    // dos simbolos que ele contem.
    std::sort(pending.begin(), pending.end(),
              [](const Pending& a, const Pending& b) {
                  return a.startByte < b.startByte;
              });

    QVector<SymbolNode> nodes;      // arena plana
    QVector<int> nodeParent;        // -1 = raiz
    QVector<QVector<int>> nodeKids;
    QHash<const void*, int> scopeByDefNode;  // no de declaracao -> indice
    QHash<QString, int> scopeByName;          // "parent/nome" -> indice

    const auto addNode = [&](SymbolNode&& n, int parent) {
        nodes.append(std::move(n));
        nodeParent.append(parent);
        nodeKids.append(QVector<int>());
        if (parent >= 0)
            nodeKids[parent].append(int(nodes.size()) - 1);
        return int(nodes.size()) - 1;
    };

    // Escopo que contem `node` (o mais interno ja registrado).
    const auto enclosingScope = [&](TSNode node) {
        for (TSNode p = ts_node_parent(node); !ts_node_is_null(p);
             p = ts_node_parent(p)) {
            const auto it = scopeByDefNode.constFind(p.id);
            if (it != scopeByDefNode.constEnd())
                return *it;
        }
        return -1;
    };

    for (const Pending& p : pending) {
        if (p.text.isEmpty())
            continue;

        if (p.kind == int(SymbolKind::Namespace)) {
            if (ts_node_is_null(p.defNode))
                continue;  // namespace anonimo
            const SymbolKind kind = kindForScopeNode(ts_node_type(p.defNode));
            const int parent = enclosingScope(p.defNode);
            const QString key = QString::number(parent) + QLatin1Char('/')
                                + p.text;
            const auto existing = scopeByName.constFind(key);
            if (existing != scopeByName.constEnd()) {
                // namespace/classe reaberta: reaproveita o no ja criado
                scopeByDefNode.insert(p.defNode.id, *existing);
                continue;
            }
            SymbolNode n;
            n.name = p.text;
            n.kind = kind;
            n.signature = p.signature;
            if (map) {
                map->lookup(p.line, &n.file, &n.line);
                if (n.file.isEmpty())
                    n.file = filePath;
            } else {
                n.file = filePath;
                n.line = p.line;
            }
            n.isInline = kind == SymbolKind::Namespace && isInlineNamespace(p.defNode, parseBytes);
            const int idx = addNode(std::move(n), parent);
            scopeByDefNode.insert(p.defNode.id, idx);
            scopeByName.insert(key, idx);
            continue;
        }

        SymbolNode n;
        n.name = p.text;
        n.kind = SymbolKind(p.kind);
        n.signature = p.signature;
        if (map) {
            map->lookup(p.line, &n.file, &n.line);
            if (n.file.isEmpty())
                n.file = filePath;
        } else {
            n.file = filePath;
            n.line = p.line;
        }

        // Definicao fora do corpo da classe: o nome capturado veio de um
        // qualified-id escrito no codigo ("Foo::bar"), e nao de um
        // field_identifier simples. Depois o qualifiedName completo e
        // montado a partir do escopo.
        const TSNode nameParent = ts_node_parent(p.nameNode);
        n.isOutOfLine = !ts_node_is_null(nameParent)
                        && qstrcmp(ts_node_type(nameParent),
                                   "qualified_identifier") == 0;

        const int parent = enclosingScope(p.nameNode);
        if (parent >= 0) {
            // Membro de classe/struct vira Method quando e funcao.
            if (n.kind == SymbolKind::Function
                && nodes[parent].kind != SymbolKind::Namespace) {
                n.kind = SymbolKind::Method;
            }
            n.qualifiedName = nodes[parent].name + QLatin1String("::") + n.name;
        }
        addNode(std::move(n), parent);
    }

    // "using namespace std;" (o no inteiro, para nao depender do nome).
    {
        static const QRegularExpression re(
            QStringLiteral("\\busing\\s+namespace\\s+([A-Za-z_][\\w:]*)"));
        auto it = re.globalMatch(text);
        while (it.hasNext())
            out.usingNamespaces.append(it.next().captured(1));
    }

    // --- 3) materializa a arena em arvore --------------------------------
    // Pre-ordem: cada no e copiado com os filhos ja aninhados.
    std::function<SymbolNode(int)> build = [&](int idx) {
        SymbolNode n = nodes.at(idx);
        for (int kid : nodeKids.at(idx))
            n.children.append(build(kid));
        return n;
    };
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodeParent.at(i) < 0)
            out.roots.append(build(i));
    }

    ts_query_delete(query);
    ts_tree_delete(tree);
    return out;
}

OutlineData TreeSitterOutlineParser::parse(const QString& text)
{
    return toOutlineData(parseSymbols(text));
}

OutlineData TreeSitterOutlineParser::toOutlineData(const SymbolTree& tree)
{
    OutlineData data;

    std::function<void(const SymbolNode&, const QString&, int)> convert =
        [&](const SymbolNode& n, const QString& prefix, int parentScope) {
        int scopeIdx = -1;
        if (n.kind == SymbolKind::Class || n.kind == SymbolKind::Struct
            || n.kind == SymbolKind::Enum) {
            OutlineScope s;
            s.name = n.name;
            s.kind = n.kind == SymbolKind::Enum ? ScopeKind::Enum
                      : n.kind == SymbolKind::Struct ? ScopeKind::Struct
                                                     : ScopeKind::Class;
            s.line = n.line;
            s.fullPath = prefix.isEmpty() ? n.name
                                          : prefix + QLatin1String("::") + n.name;
            s.parent = parentScope;
            data.scopes.append(s);
            scopeIdx = int(data.scopes.size()) - 1;
        } else if (n.kind == SymbolKind::Namespace) {
            // A nav bar nao lista namespaces como escopo; seus membros ficam
            // sob o escopo pai (ou global).
            const QString inner = prefix.isEmpty()
                                      ? n.name
                                      : prefix + QLatin1String("::") + n.name;
            for (const SymbolNode& c : n.children)
                convert(c, inner, parentScope);
            return;
        } else {
            OutlineMember m;
            m.name = n.name;
            m.signature = n.signature;
            m.line = n.line;
            m.isFunction = n.kind == SymbolKind::Function
                || n.kind == SymbolKind::Method;
            m.isOutOfLine = n.isOutOfLine;
            if (scopeIdx >= 0)
                data.scopes[scopeIdx].members.append(m);
            else
                data.globalMembers.append(m);
            return;
        }

        const QString inner = data.scopes[scopeIdx].fullPath;
        for (const SymbolNode& c : n.children)
            convert(c, inner, scopeIdx);
    };

    for (const SymbolNode& r : tree.roots)
        convert(r, QString(), -1);
    return data;
}

}  // namespace editor
