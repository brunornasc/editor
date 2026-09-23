#include "debug/MiParser.hpp"

#include <QChar>

namespace debug {

namespace {

//! Valor "composto" ({...} ou [...]): devolve o conteudo interno (sem os
//! delimitadores). Valor simples: devolve unquote().
QString unpackValue(const QString& value)
{
    const QString v = value.trimmed();
    if (v.size() >= 2 && (v.front() == '{' || v.front() == '[')
        && (v.back() == '}' || v.back() == ']'))
        return v.mid(1, v.size() - 2);
    return unquote(v);
}

}  // namespace

QString unquote(const QString& value)
{
    QString v = value.trimmed();
    if (v.size() < 2 || !v.startsWith(QLatin1Char('"')) || !v.endsWith(QLatin1Char('"')))
        return v;

    QString out;
    out.reserve(v.size());
    bool esc = false;
    for (int i = 1; i + 1 < v.size(); ++i) {
        const QChar c = v.at(i);
        if (esc) {
            switch (c.unicode()) {
            case 'n': out += QLatin1Char('\n'); break;
            case 't': out += QLatin1Char('\t'); break;
            case 'r': out += QLatin1Char('\r'); break;
            default: out += c; break;
            }
            esc = false;
        } else if (c == QLatin1Char('\\')) {
            esc = true;
        } else {
            out += c;
        }
    }
    if (esc)
        out += QLatin1Char('\\');
    return out;
}

QStringList splitTopLevel(const QString& text)
{
    QStringList out;
    QString cur;
    int depth = 0;
    bool inQuote = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inQuote) {
            cur += c;
            if (c == QLatin1Char('"') && (i == 0 || text.at(i - 1) != QLatin1Char('\\')))
                inQuote = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuote = true;
            cur += c;
        } else if (c == QLatin1Char('{') || c == QLatin1Char('[')) {
            ++depth;
            cur += c;
        } else if (c == QLatin1Char('}') || c == QLatin1Char(']')) {
            --depth;
            cur += c;
        } else if (c == QLatin1Char(',') && depth == 0) {
            out.append(cur.trimmed());
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty() || out.isEmpty())
        out.append(cur.trimmed());
    return out;
}

QMap<QString, QString> parsePairs(const QString& body)
{
    QMap<QString, QString> pairs;
    const QStringList items = splitTopLevel(body);
    for (const QString& item : items) {
        int depth = 0;
        bool inQuote = false;
        int eq = -1;
        for (int i = 0; i < item.size(); ++i) {
            const QChar c = item.at(i);
            if (inQuote) {
                if (c == QLatin1Char('"') && item.at(i - 1) != QLatin1Char('\\'))
                    inQuote = false;
                continue;
            }
            if (c == QLatin1Char('"'))
                inQuote = true;
            else if (c == QLatin1Char('{') || c == QLatin1Char('['))
                ++depth;
            else if (c == QLatin1Char('}') || c == QLatin1Char(']'))
                --depth;
            else if (c == QLatin1Char('=') && depth == 0) {
                eq = i;
                break;
            }
        }
        if (eq < 0)
            continue;
        const QString key = item.left(eq).trimmed();
        const QString value = item.mid(eq + 1);
        if (!key.isEmpty())
            pairs.insert(key, unpackValue(value));
    }
    return pairs;
}

MiRecord parseRecord(const QString& line_in)
{
    MiRecord rec;
    QString line = line_in;
    line = line.trimmed();
    if (line.isEmpty())
        return rec;

    int i = 0;
    while (i < line.size() && line.at(i).isDigit()) {
        ++i;
    }
    if (i > 0)
        rec.token = line.left(i).toInt();
    if (i >= line.size())
        return rec;

    const QChar kind = line.at(i);
    if (kind != QLatin1Char('^') && kind != QLatin1Char('*')
        && kind != QLatin1Char('+') && kind != QLatin1Char('=')
        && kind != QLatin1Char('~') && kind != QLatin1Char('@')
        && kind != QLatin1Char('&'))
        return rec;  // nao e um registro MI
    rec.kind = kind.toLatin1();
    ++i;

    // Streams de console/target/log: o corpo e toda a linha restante.
    if (kind == QLatin1Char('~') || kind == QLatin1Char('@') || kind == QLatin1Char('&')) {
        rec.body = line.mid(i);
        return rec;
    }

    // Resultado / async / notify: "cls" ate ','.
    int j = i;
    while (j < line.size() && line.at(j).isLetterOrNumber()) {
        ++j;
    }
    rec.cls = line.mid(i, j - i);
    if (j < line.size() && line.at(j) == QLatin1Char(','))
        rec.body = line.mid(j + 1);
    rec.ok = !rec.cls.isEmpty();
    return rec;
}

QVector<MiFrame> parseFrames(const QString& body)
{
    QVector<MiFrame> frames;
    const int lb = body.indexOf(QLatin1Char('['));
    const int rb = body.lastIndexOf(QLatin1Char(']'));
    if (lb < 0 || rb < 0 || rb <= lb)
        return frames;

    const QString inner = body.mid(lb + 1, rb - lb - 1);
    const QStringList items = splitTopLevel(inner);
    for (const QString& item : items) {
        const QString t = item.trimmed();
        if (t.isEmpty())
            continue;
        QString tuple = t;
        // -stack-list-frames retorna "frame={level=...}" (chave=tupla).
        // -stack-list-arguments pode retornar "{...}" (tupla exata).
        if (!tuple.startsWith(QLatin1Char('{'))) {
            const int eq = tuple.indexOf(QLatin1Char('='), 1);
            if (eq < 0)
                continue;
            tuple = tuple.mid(eq + 1).trimmed();
        }
        if (tuple.startsWith(QLatin1Char('{')) && tuple.endsWith(QLatin1Char('}')))
            tuple = tuple.mid(1, tuple.size() - 2);
        const QMap<QString, QString> p = parsePairs(tuple);

        MiFrame f;
        f.func = p.value(QStringLiteral("func"));
        f.file = p.value(QStringLiteral("file"));
        f.fullname = p.value(QStringLiteral("fullname"));
        f.line = p.value(QStringLiteral("line")).toInt();
        frames.append(f);
    }
    return frames;
}

QVector<MiVar> parseVars(const QString& body)
{
    QVector<MiVar> vars;
    const int lb = body.indexOf(QLatin1Char('['));
    const int rb = body.lastIndexOf(QLatin1Char(']'));
    if (lb < 0 || rb < 0 || rb <= lb)
        return vars;

    const QString inner = body.mid(lb + 1, rb - lb - 1);
    const QStringList items = splitTopLevel(inner);
    for (const QString& item : items) {
        const QString t = item.trimmed();
        if (t.isEmpty())
            continue;
        QString tuple = t;
        if (tuple.startsWith(QLatin1Char('{')))
            tuple = tuple.mid(1, tuple.size() - 2);
        const QMap<QString, QString> p = parsePairs(tuple);

        MiVar v;
        v.name = p.value(QStringLiteral("name"));
        v.value = p.value(QStringLiteral("value"));
        v.type = p.value(QStringLiteral("type"));
        vars.append(v);
    }
    return vars;
}

}  // namespace debug