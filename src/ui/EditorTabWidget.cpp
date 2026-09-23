#include "ui/EditorTabWidget.hpp"

#include "ui/CodeEditorWidget.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTabBar>

#include <QDebug>

namespace ui {

EditorTabWidget::EditorTabWidget(QWidget* parent)
    : QTabWidget(parent)
{
    setTabsClosable(true);
    setMovable(true);
    setUsesScrollButtons(true);

    connect(this, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget* page = widget(index);
        removeTab(index);
        page->deleteLater();
    });

    connect(this, &QTabWidget::currentChanged, this, [this](int index) {
        const auto* ed = editorAt(index);
        emit currentFileChanged(ed ? ed->filePath() : QString());
    });
}

CodeEditorWidget* EditorTabWidget::editorAt(int index) const
{
    return qobject_cast<CodeEditorWidget*>(widget(index));
}

int EditorTabWidget::indexOfPath(const QString& filePath) const
{
    for (int i = 0; i < count(); ++i) {
        const auto* ed = editorAt(i);
        if (ed && QFileInfo(ed->filePath()) == QFileInfo(filePath))
            return i;
    }
    return -1;
}

int EditorTabWidget::openFile(const QString& filePath)
{
    const int existing = indexOfPath(filePath);
    if (existing >= 0) {
        setCurrentIndex(existing);
        return existing;
    }

    auto* editor = new CodeEditorWidget(this);
    if (!editor->openFile(filePath)) {
        editor->deleteLater();
        return -1;
    }

    // Deduplicacao de nome na tab (arquivos homonimos de pastas distintas).
    QString base = QFileInfo(filePath).fileName();
    int suffix = 1;
    while (true) {
        bool clash = false;
        for (int i = 0; i < count(); ++i) {
            if (tabText(i) == base) {
                clash = true;
                break;
            }
        }
        if (!clash)
            break;
        base = QStringLiteral("%1 (%2)").arg(QFileInfo(filePath).fileName()).arg(suffix++);
    }

    const int index = addEditorTab(editor, base);
    setCurrentIndex(index);
    emit editorCreated(editor);
    return index;
}

int EditorTabWidget::newDocument(const QString& title)
{
    auto* editor = new CodeEditorWidget(this);
    const int index = addEditorTab(editor, title);
    setCurrentIndex(index);
    emit editorCreated(editor);
    return index;
}

int EditorTabWidget::addEditorTab(CodeEditorWidget* editor, const QString& title)
{
    const int index = addTab(editor, title);
    setTabToolTip(index, editor->filePath().isEmpty() ? title : editor->filePath());
    return index;
}

void EditorTabWidget::retitleEditor(CodeEditorWidget* editor)
{
    const int index = indexOf(editor);
    if (index < 0)
        return;
    if (!editor->filePath().isEmpty())
        setTabText(index, QFileInfo(editor->filePath()).fileName());
    setTabToolTip(index, editor->filePath());
}

bool EditorTabWidget::saveDocument(int index)
{
    auto* ed = editorAt(index);
    if (!ed)
        return true;
    if (ed->filePath().isEmpty())
        return false;  // "Save As" fica para a Parte 3 (dialogo)

    QFile f(ed->filePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(ed->editor()->text().toUtf8());
        ed->editor()->setModified(false);
        return true;
    }
    return false;
}

void EditorTabWidget::closeAllEditors()
{
    while (count() > 0) {
        QWidget* page = widget(0);
        removeTab(0);
        page->deleteLater();
    }
}

}  // namespace ui