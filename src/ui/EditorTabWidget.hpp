#pragma once

#include <QTabWidget>

class QWidget;

namespace ui {

class CodeEditorWidget;

//! QTabWidget central de documentos.
//!
//! Cada aba e um CodeEditorWidget (QScintilla + navigation bars).
//! Parte 4/5 anexam Build/Debug por documento via `editorAt(index)`.
class EditorTabWidget final : public QTabWidget {
    Q_OBJECT

public:
    explicit EditorTabWidget(QWidget* parent = nullptr);

    //! Abre um arquivo em disco numa aba (deduplica: reutiliza se ja aberto).
    //! Retorna o indice da aba.
    int openFile(const QString& filePath);

    //! Cria um documento vazio (arquivo novo). Retorna o indice da aba.
    int newDocument(const QString& title);

    //! Editor da aba (nullptr se a aba nao for um editor).
    CodeEditorWidget* editorAt(int index) const;

    //! Aba que contem um dado arquivo (-1 se nao estiver aberto).
    int indexOfPath(const QString& filePath) const;

    //! Salva o documento da aba atual no disco.
    bool saveDocument(int index);

    //! Redefine o titulo da aba a partir do relatorio do documentLoaded.
    void retitleEditor(CodeEditorWidget* editor);

public slots:
    void closeAllEditors();

signals:
    //! Emitido quando o arquivo da aba ativa muda (MainWindow atualiza titulo).
    void currentFileChanged(const QString& filePath);

    //! Um novo editor (aberta de arquivo ou doc novo) foi criado. O
    //! DebugController (Parte 5) usa para anexar breakpoints da margem.
    void editorCreated(ui::CodeEditorWidget* editor);

private:
    int addEditorTab(CodeEditorWidget* editor, const QString& title);
};

}  // namespace ui