#pragma once

#include <QDockWidget>
#include <QVector>

class QPlainTextEdit;
class QTabWidget;
class QTreeWidget;

namespace ui {

//! Uma linha da janela de erros (BuildManager alimenta).
struct ErrorEntry {
    QString file;         // caminho absoluto (ou vazio)
    int line = 0;         // 1-based, como exibido ao usuario
    int column = 0;
    bool isError = true;  // false => warning
    QString message;
};

//! Dock inferior "Output Window" (VS 2005). Abas fixas:
//!   1. Build Output  -> redirect do BuildManager (Parte 4)
//!   2. Error List    -> erros/avisos com navegacao por clique (Parte 4)
//!   3. Debug Console -> console de execucao / GDB (Parte 5)
//!   4. Conan Logs    -> saida do `conan install` (Parte 4)
class OutputWindow final : public QDockWidget {
    Q_OBJECT

public:
    enum class Pane : int {
        BuildOutput = 0,
        ErrorList = 1,
        DebugConsole = 2,
        ConanLogs = 3,
    };

    explicit OutputWindow(QWidget* parent = nullptr);

    //! Exibe/ativa uma das abas (BuildManager e GdbDebugger chamam isto).
    void activatePane(Pane pane);

    //! Acesso aos consoles de texto para anexar QProcess output.
    QPlainTextEdit* console(Pane pane) const;

    //! Acesso a arvore de erros (tamanho da coluna, selecao, etc.).
    QTreeWidget* errorList() const;

    //! Anexa texto a uma aba especifica sem trocar a selecao atual.
    void append(Pane pane, const QString& text);

    //! Anexa texto formatado em HTML a uma aba de console.
    void appendHtml(Pane pane, const QString& html);

    //! Limpa o conteudo de uma aba.
    void clear(Pane pane);

    //! Substitui o conteudo da Error List.
    void setErrors(const QVector<ErrorEntry>& entries);

signals:
    //! Duplo-clique/Enter numa linha de erro: abre o arquivo e posiciona.
    void errorActivated(const QString& filePath, int line);

private:
    QTabWidget* m_tabs = nullptr;
    QTreeWidget* m_errors = nullptr;
    QPlainTextEdit* m_console[(int)Pane::ConanLogs + 1] = {};
};

}  // namespace ui