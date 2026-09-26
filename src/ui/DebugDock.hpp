#pragma once

#include "debug/MiParser.hpp"

#include <QDockWidget>
#include <QHash>
#include <QVector>

class QListWidget;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace ui {

//! Uma linha da janela de Breakpoints (line e 0-based).
struct BreakpointEntry {
    QString file;    // caminho absoluto (navegacao)
    QString label;   // exibicao "arquivo:linha"
    int line = -1;   // 0-based
    bool enabled = true;
};

//! Dock "Depuracao": abas Pilha de Chamadas + Locais + Inspecao (watch) +
//! Breakpoints (VS2005). Fica na MESMA area do dock "Project", separado em
//! abas. O DebugController alimenta pelas set*(); duplo-clique nas listas
//! navega ate o quadro (frameActivated).
class DebugDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit DebugDock(QWidget* parent = nullptr);

    void setCallStack(const QVector<debug::MiFrame>& frames);
    void setLocals(const QString& func, const QVector<debug::MiVar>& vars);
    void clearAll();

    //! Janela de Breakpoints: preenche a tabela (linhas 0-based).
    void setBreakpointsList(const QVector<BreakpointEntry>& entries);
    //! Janela de Inspecao (watch).
    void setWatchExpressions(const QStringList& exprs);
    void setWatchValue(const QString& expr, const QString& value);
    void clearWatchValues();

signals:
    //! Duplo-clique/Enter num quadro da pilha.
    void frameActivated(const QString& filePath, int line);
    //! Checkbox da janela de Breakpoints: (des)ativar um breakpoint.
    void breakpointToggled(const QString& filePath, int line, bool enabled);
    //! Remover um breakpoint pela janela de Breakpoints.
    void breakpointRemoved(const QString& filePath, int line);
    //! "Adicionar a Inspecao" via menu de contexto dos Locais.
    void addToWatchRequested(const QString& expr);
    //! "Remover Inspecao" da tabela de watch.
    void watchRemoved(const QString& expr);

private:
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_stack = nullptr;   // Pilha de Chamadas
    QTreeWidget* m_locals = nullptr;  // Locais
    QTableWidget* m_bpTable = nullptr;    // Breakpoints
    QTableWidget* m_watchTable = nullptr; // Inspecao (watch)
    QHash<QString, QString> m_watchValues;
};

}  // namespace ui