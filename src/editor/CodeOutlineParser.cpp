#include "editor/CodeOutlineParser.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace editor {

namespace {

const QRegularExpression& typeDeclRe()
{
    static const QRegularExpression re(
        R"(^(?:template\s*<[^>]*>\s*)?(class|struct|namespace|enum(?:\s+class)?)\s+([A-Za-z_]\w*))");
    return re;
}

const QRegularExpression& funcOpenRe()
{
    static const QRegularExpression re(
        R"(\b((?:[A-Za-z_~]\w*::)*[A-Za-z_~]\w*)\s*\()");
    return re;
}

//! Caracteres que indicam contexto de CHAMADA de funcao (nao declaracao),
//! quando aparecem imediatamente antes do nome candidato.
bool isCallContextPreChar(const QString& s, int idx)
{
    if (idx <= 0)
        return false;
    static const QSet<QChar> puncts = {
        QLatin1Char(':'), QLatin1Char('.'), QLatin1Char('('),
        QLatin1Char(','), QLatin1Char('='), QLatin1Char('<'),
        QLatin1Char('>'), QLatin1Char('+'), QLatin1Char('-'),
        QLatin1Char('/'), QLatin1Char('%'), QLatin1Char('!'),
        QLatin1Char('['),
    };
    return puncts.contains(s[idx - 1]);
}

const QSet<QString>& keywordSet()
{
    static const QSet<QString> keys = {
        "if", "for", "while", "switch", "catch", "do", "return",
        "sizeof", "delete", "new", "throw", "assert", "break", "case",
        "default", "static_cast", "const_cast", "reinterpret_cast",
        "dynamic_cast", "decltype", "noexcept", "alignof", "typeid",
        "try", "else", "template", "synchronized",
    };
    return keys;
}

ScopeKind kindFromText(const QString& text)
{
    if (text == QLatin1String("namespace"))
        return ScopeKind::Namespace;
    if (text == QLatin1String("struct"))
        return ScopeKind::Struct;
    if (text == QLatin1String("enum"))
        return ScopeKind::Enum;
    return ScopeKind::Class;
}

//! Indice do parenteses de fechamento correspondente a openIdx.
int findClosingParen(const QString& s, int openIdx)
{
    int depth = 0;
    for (int i = openIdx; i < s.size(); ++i) {
        if (s[i] == QLatin1Char('('))
            ++depth;
        else if (s[i] == QLatin1Char(')')) {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return -1;
}

bool isDefinition(const QString& after)
{
    QString t = after.trimmed();
    if (t.isEmpty())
        return false;
    // Modificadores pos-assinatura (const/noexcept/override/final/volatile/-> ret)
    static const QRegularExpression mods(
        R"(\{\s*$|^(?:const|volatile|noexcept|override|final|&|&&|->).*\{\s*$)");
    return t.startsWith(QLatin1Char('{')) || mods.match(t).hasMatch();
}

//! Acha o indice (em scope) do dono de uma funcao qualificada.
//! Resolve "app::MainWindow::show" para o escopo "app::MainWindow" e
//! "MainWindow::helper" (inline, sem namespace) para o escopo nomeado
//! "MainWindow" mais interno.
int resolveOwnerByQualifiedName(const QVector<OutlineScope>& scopes,
                                const QString& qualified, int fallback)
{
    const int ld = qualified.lastIndexOf(QLatin1String("::"));
    if (ld <= 0)
        return fallback;
    const QString prefix = qualified.left(ld);
    const QString lastSeg = prefix.mid(prefix.lastIndexOf(QLatin1String("::")) + 1);

    // Tenta correspondencia exata de fullPath; senao, pelo nome simples.
    int best = -1;
    int bestLen = -1;
    for (int i = 0; i < scopes.size(); ++i) {
        const bool exact = scopes[i].fullPath == prefix;
        const bool byName = scopes[i].name == lastSeg;
        if ((exact || byName) && scopes[i].fullPath.size() > bestLen) {
            best = i;
            bestLen = scopes[i].fullPath.size();
        }
    }
    return best >= 0 ? best : fallback;
}

struct PendingScope {
    int index = -1;      // indice em scopes[]
    int openDepth = -1;  // profundidade de chaves quando abriu
};

}  // namespace

// ---------------------------------------------------------------------------

QString CodeOutlineParser::sanitizeLine(const QString& line, bool& inBlockComment)
{
    QString out = line;
    const int n = out.size();
    int i = 0;
    while (i < n) {
        const QChar c = out[i];
        if (inBlockComment) {
            if (c == QLatin1Char('*') && i + 1 < n && out[i + 1] == QLatin1Char('/')) {
                out[i] = QLatin1Char(' ');
                out[i + 1] = QLatin1Char(' ');
                inBlockComment = false;
                i += 2;
            } else {
                out[i] = QLatin1Char(' ');
                ++i;
            }
        } else if (c == QLatin1Char('/')) {
            if (i + 1 < n && out[i + 1] == QLatin1Char('/')) {
                for (int j = i; j < n; ++j)
                    out[j] = QLatin1Char(' ');
                break;
            }
            if (i + 1 < n && out[i + 1] == QLatin1Char('*')) {
                out[i] = QLatin1Char(' ');
                out[i + 1] = QLatin1Char(' ');
                inBlockComment = true;
                i += 2;
            } else {
                ++i;
            }
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            out[i] = QLatin1Char(' ');
            ++i;
            while (i < n) {
                if (out[i] == QLatin1Char('\\')) {
                    out[i] = QLatin1Char(' ');
                    if (i + 1 < n)
                        out[i + 1] = QLatin1Char(' ');
                    i += 2;
                    continue;
                }
                if (out[i] == c) {
                    out[i] = QLatin1Char(' ');
                    ++i;
                    break;
                }
                out[i] = QLatin1Char(' ');
                ++i;
            }
        } else {
            ++i;
        }
    }
    return out;
}

OutlineData CodeOutlineParser::parse(const QString& text)
{
    OutlineData data;

    QVector<PendingScope> stack;   // escopos abertos (topo = mais interno)
    int pendingType = -1;          // decl de tipo aguardando '{'
    bool inBlockComment = false;

    const QStringList lines = text.split(QLatin1Char('\n'));
    int braceDepth = 0;

    auto currentOwner = [&]() -> int {
        if (pendingType >= 0)
            return data.scopes[pendingType].parent >= 0
                       ? data.scopes[pendingType].parent
                       : -1;
        return stack.isEmpty() ? -1 : stack.last().index;
    };

    auto attachFunction = [&](int ownerIdx, const QString& name,
                              const QString& signature, int lineIdx, bool def,
                              bool outOfLine) {
        OutlineMember m;
        m.name = name;
        m.signature = signature;
        m.line = lineIdx;
        m.isFunction = true;
        m.isOutOfLine = outOfLine && ownerIdx >= 0;
        if (ownerIdx >= 0)
            data.scopes[ownerIdx].members.push_back(m);
        else
            data.globalMembers.push_back(m);
    };

    for (int li = 0; li < lines.size(); ++li) {
        const QString clean = sanitizeLine(lines[li], inBlockComment);
        const QString trimmed = clean.trimmed();
        if (trimmed.isEmpty())
            continue;

        // --- 1) declaracao de tipo (classe/struct/namespace/enum) ----------
        const auto tm = typeDeclRe().match(trimmed);
        if (tm.hasMatch()) {
            const QString kindText = tm.captured(1);
            const QString name = tm.captured(2);

            OutlineScope sc;
            sc.name = name;
            sc.kind = kindFromText(kindText);
            sc.line = li;
            sc.parent = stack.isEmpty() ? -1 : stack.last().index;
            sc.fullPath = sc.parent < 0
                              ? name
                              : data.scopes[sc.parent].fullPath + QLatin1String("::") + name;

            pendingType = data.scopes.size();
            data.scopes.push_back(sc);
            if (sc.parent >= 0)
                ;  // aninhados ficam visiveis individualmente (Parte 2)
        }

        int owner = currentOwner();

        // --- 2) funcoes -----------------------------------------------------
        {
            // spans ja "consumidos" por match qualificado (evita duplicar
            // "app::MainWindow::show" e "show" como dois membros).
            QVector<QPair<int, int>> coveredSpans;

            auto it = funcOpenRe().globalMatch(clean);
            while (it.hasNext()) {
                const auto m = it.next();
                const QString name = m.captured(1);
                const int matchStart = m.capturedStart(1);

                if (keywordSet().contains(name))
                    continue;
                if (isCallContextPreChar(clean, matchStart))
                    continue;

                bool insideCovered = false;
                for (const auto& [s, e] : coveredSpans) {
                    if (matchStart >= s && matchStart < e) {
                        insideCovered = true;
                        break;
                    }
                }
                if (insideCovered)
                    continue;

                const int openIdx = m.capturedEnd(1);
                const int closeIdx = findClosingParen(clean, openIdx);
                if (closeIdx < 0)
                    continue;
                const QString after = clean.mid(closeIdx + 1).trimmed();
                if (!after.startsWith(QLatin1Char('{')) &&
                    !after.startsWith(QLatin1Char(';')) &&
                    !isDefinition(after) &&
                    !after.endsWith(QLatin1Char('{')))
                    continue;

                // Nome qualificado ("app::MainWindow::show") vira membro da classe.
                const bool qualified = name.contains(QLatin1String("::"));
                const int qualIndex = qualified
                    ? resolveOwnerByQualifiedName(data.scopes, name, owner)
                    : owner;
                const QString memberName = qualified
                    ? name.mid(name.lastIndexOf(QLatin1String("::")) + 2)
                    : name;

                const bool def = !after.startsWith(QLatin1Char(';'));
                attachFunction(qualIndex, memberName, trimmed, li, def, qualified);

                if (qualified)
                    coveredSpans.push_back({matchStart, openIdx});
            }
        }

        // --- 3) campos (dentro de class/struct) e enumeradores -----------
        const bool inClassOrStruct = owner >= 0 &&
            (data.scopes[owner].kind == ScopeKind::Class ||
             data.scopes[owner].kind == ScopeKind::Struct);
        const bool inEnum = owner >= 0 &&
            data.scopes[owner].kind == ScopeKind::Enum;

        if (!trimmed.endsWith(QLatin1Char(':'))) {  // ignora public:/case:
            if (inEnum) {
                static const QRegularExpression enumItem(
                    R"(^([A-Za-z_]\w*)\s*(?:=|,|$))");
                auto m = enumItem.match(trimmed);
                if (m.hasMatch() &&
                    !trimmed.startsWith(QLatin1String("//"))) {
                    OutlineMember it;
                    it.name = m.captured(1);
                    it.signature = trimmed;
                    it.line = li;
                    it.isFunction = false;
                    data.scopes[owner].members.push_back(it);
                }
            } else if (inClassOrStruct &&
                       !trimmed.contains(QLatin1Char('(')) &&
                       !trimmed.startsWith(QLatin1String("using")) &&
                       !trimmed.startsWith(QLatin1String("typedef")) &&
                       !trimmed.startsWith(QLatin1String("friend")) &&
                       !trimmed.startsWith(QLatin1String("template"))) {
                QString fieldLine = trimmed;
                const int eq = fieldLine.indexOf(QLatin1Char('='));
                if (eq >= 0)
                    fieldLine = fieldLine.left(eq);
                if (fieldLine.endsWith(QLatin1Char(';')))
                    fieldLine.chop(1);
                fieldLine = fieldLine.trimmed();
                bool skipField = fieldLine.endsWith(QLatin1Char('}'));

                // remove qualificadores de tipo mais comuns
                static const QStringList qualifiers = {
                    "static", "const", "mutable", "volatile", "unsigned",
                    "signed", "long", "short",
                };
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const QString& q : qualifiers) {
                        if (fieldLine.startsWith(q) &&
                            (fieldLine.size() == q.size() ||
                             fieldLine[q.size()].isSpace())) {
                            fieldLine = fieldLine.mid(q.size()).trimmed();
                            changed = true;
                        }
                    }
                }

                const int lastSpace = fieldLine.lastIndexOf(QLatin1Char(' '));
                const QString candidate =
                    lastSpace < 0 ? fieldLine : fieldLine.mid(lastSpace + 1);
                if (!skipField && candidate.size() > 0 && candidate[0].isLetter()) {
                    OutlineMember f;
                    f.name = candidate;
                    f.signature = trimmed;
                    f.line = li;
                    f.isFunction = false;
                    data.scopes[owner].members.push_back(f);
                }
            }
        }

        // --- 4) rastreio de chaves -----------------------------------------
        for (const QChar ch : clean) {
            if (ch == QLatin1Char('{')) {
                if (pendingType >= 0) {
                    data.scopes[pendingType].openDepth = braceDepth;
                    stack.push_back({pendingType, braceDepth});
                    pendingType = -1;
                }
                ++braceDepth;
            } else if (ch == QLatin1Char('}')) {
                --braceDepth;
                while (!stack.isEmpty() && stack.last().openDepth == braceDepth)
                    stack.removeLast();
            }
        }

        // declaracao avante (class X; / enum Y;) sem corpo
        if (pendingType >= 0 && trimmed.contains(QLatin1Char(';')))
            pendingType = -1;
    }

    return data;
}

}  // namespace editor