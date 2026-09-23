#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace debug {

//! Uma variavel da stack (locais/args do quadro atual).
struct MiVar {
    QString name;
    QString value;
    QString type;
};

//! Um quadro de chamada da pilha.
struct MiFrame {
    QString func;
    QString file;      // caminho (possivelmente relativo) informado pelo gdb
    QString fullname;  // caminho absoluto (quando disponivel)
    int line = -1;
};

//! Um registro da fica de saida do gdb MI2.
struct MiRecord {
    int token = -1;  // token numerico do comando (-1 se ausente)
    char kind = 0;   // '^' (resultado), '+'/'*' (async), '=' (notify),
                     // '~' (console), '@' (target), '&' (log)
    QString cls;     // "done", "stopped", "running", "breakpoint-hit", ...
    QString body;    // resto apos a classe
    bool ok = true;  // falso se a linha nao parecia um registro MI
};

//! Interpreta uma linha do protocolo MI2.
MiRecord parseRecord(const QString& line);

//! Separa pares top-level key=value respeitando '"' e aninhamento {} [].
QStringList splitTopLevel(const QString& text);

//! Remove aspas duplas envolventes e escapes simples de um valor MI.
QString unquote(const QString& value);

//! Converte um corpo "k=v,k=v" num mapa (compostos {}/[] mantidos "crus").
QMap<QString, QString> parsePairs(const QString& body);

//! Interpreta "frames=[{...},{...}]" (resposta de -stack-list-frames).
QVector<MiFrame> parseFrames(const QString& body);

//! Interpreta "variables=[{...},{...}]" (-stack-list-variables).
QVector<MiVar> parseVars(const QString& body);

}  // namespace debug