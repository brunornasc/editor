#pragma once

#include "editor/CodeOutlineParser.hpp"

#include <Qsci/qsciscintilla.h>
#include <QByteArray>
#include <QPoint>
#include <QStringList>
#include <QWidget>

#include <utility>

class QComboBox;
class QEvent;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QsciAPIs;
class QTimer;

namespace ui {

//! Widget de edicao: QScintilla (lexer por linguagem, margins, folding) com
//! barra de navegacao superior estilo VS2005 (classes/namespaces + membros)
//! e intellisense por linguagem (arquivo de keywords + scan de #includes).
class CodeEditorWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CodeEditorWidget(QWidget* parent = nullptr);

    //! Abre um arquivo em disco. Retorna false se nao for possivel ler.
    bool openFile(const QString& filePath);

    //! Cria/retorna editor referencia (Parte 4/5 acessam por aqui).
    QsciScintilla* editor() const;

    QString filePath() const { return m_filePath; }
    bool isModified() const { return m_editor->isModified(); }

    //! Redefine o caminho do documento (apos "Save As" / rename).
    void setFilePath(const QString& path) { m_filePath = path; }

    //! Marca erros do compilador com indicador/margem vermelha.
    //! `errors` = pares (linha 0-based, mensagem).
    void setCompilerErrors(const QVector<std::pair<int, QString>>& errors);
    void clearCompilerErrors();

    //! Retorna o escopo na linha (para o Project explorer sincronizar).
    const editor::OutlineData& outline() const { return m_outline; }

    //! Move o cursor para uma linha (0-based) e garante visibilidade.
    void gotoLine(int line);

    //! Marca/desmarca breakpoints (linhas 0-based) na margem de simbolos.
    //! `disabled` = linhas com breakpoint desativado (exibido esmaecido).
    void setBreakpoints(const QList<int>& lines,
                        const QSet<int>& disabled = {});
    void clearAllBreakpoints();

    //! Seta de execucao do depurador (linha 0-based; -1 limpa). A linha
    //! ganha um realce de fundo visivel independente da posicao do cursor.
    void setExecutionLine(int line);

    //! Aplica estilo base do editor (usado por todos os documentos).
    static void configureCommonStyle(QsciScintilla* ed);

protected:
    //! Auto-completa pares de abertura ({ ( [) ao digitar.
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    //! Clicou na margem de breakpoints (linha 0-based). Parte 5 consome.
    void breakpointMarginClicked(int line);
    //! Arquivo foi carregado (Parte 3 popula arvore Project com isto).
    void documentLoaded(const QString& filePath);
    //! Dicionario de simbolos atualizado (novo parse).
    void outlineUpdated();
    //! Mouse parado sobre um simbolo (depurador parado consome).
    void hoverWordRequested(const QString& word, const QPoint& globalPos);

private slots:
    //! O HeaderIndex preprocessou+indexou um include: entra no popup.
    void onHeaderIndexed(const QString& includeSpec, bool ok);
    void onScopeSelected(int index);
    void onMemberSelected(int index);
    void onCursorPositionChanged(int line, int index);

private:
    void setupNavigationBar();
    void populateNavigationBars();
    void refreshOutline();
    void updateNavigationForCursor(int line);
    void jumpToLine(int line);
    void completeBracketPair(const QChar open);

    //! (Re)configura lexer + intellisense conforme a linguagem do arquivo.
    void applyLanguage(const QString& language);
    //! (Re)prepara o QsciAPIs com a lista atual m_apiWords.
    void rebuildApis();
    //! Monta a lista do popup "std::" (nomes reais vindos dos headers indexados).
    void updateStdWords();
    //! Le os #includes do buffer atual e agenda a indexacao dos que faltam.
    void detectIncludes();
    //! Agenda `detectIncludes` (o usuario pode estar no meio de "#include <vec").
    void scheduleIncludeScan();
    //! Refeita o popup com o que o HeaderIndex ja tem para este documento.
    void applyIndexedSymbols();
    //! Completar estilo IDE: Ctrl+Espaco, "ns::" ou prefixo de identificador.
    void triggerIdeCompletion(bool force);
    //! Abre a lista Scintilla (insere o item escolhido no cursor).
    void showAutocompleteList(QStringList words, int alreadyTyped);

    //! Preenche a combo direita com as funcoes/metodos do escopo.
    void fillFunctionMembers(int scopeIdx);
    //! Escopo listado (classe/struct) mais interno que contem a linha.
    int scopeAtLine(int line) const;
    //! Linha da combo esquerda para o escopo (0 = "(global)").
    int rowForScope(int scopeIdx) const;

    QsciScintilla* m_editor = nullptr;
    QComboBox* m_scopeBox = nullptr;   // classes/namespaces (esq.)
    QComboBox* m_memberBox = nullptr;  // funcoes/metodos (dir.)
    QsciAPIs* m_apis = nullptr;        // intellisense inline do documento

    QString m_filePath;
    QString m_language = QStringLiteral("cpp");
    QStringList m_langKeywords;        // if/int/class... (sem membros de std)
    QStringList m_apiWords;            // default + scan (completar geral)
    QStringList m_stdWords;            // so membros do namespace std
    QByteArray m_autocBuffer;          // buffer vivo p/ SCI_AUTOCSHOW
    editor::OutlineData m_outline;
    QColor m_errorColor = QColor(Qt::red);
    bool m_updatingNav = false;
    QStringList m_indexedIncludes;   // specs deste documento ja indexados
    QTimer* m_includeTimer = nullptr;
    QTimer* m_hoverTimer = nullptr;
    QString m_hoverWord;
    QPoint m_hoverPos;
};

}  // namespace ui