#include "ui/CodeEditorWidget.hpp"

#include "core/Keywords.hpp"

#include <Qsci/qsciapis.h>
#include <Qsci/qscilexerasm.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <QDebug>

namespace ui {

namespace {

constexpr int kLineNumberMargin = 0;
constexpr int kBreakpointMargin = 1;
constexpr int kFoldMargin = 2;
constexpr int kErrorIndicator = 1;

// Markers desenhados na margem de simbolos (Parte 5).
constexpr int kBreakpointMarker = 0;
constexpr int kExecutionMarker = 1;        // seta amarela
constexpr int kExecutionLineMarker = 2;    // realce de fundo da linha
constexpr int kBreakpointDisabledMarker = 3;

QFont editorFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setFamily(QLatin1String("DejaVu Sans Mono"));  // fallback sistemico ok
    f.setPointSize(10);
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------

CodeEditorWidget::CodeEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    setupNavigationBar();
    root->addWidget(m_scopeBox->parentWidget());  // barra superior fixa

    m_editor = new QsciScintilla(this);
    configureCommonStyle(m_editor);
    applyLanguage(m_language);  // lexer + intellisense do documento inicial
    root->addWidget(m_editor, 1);

    // Scan assincrono de #includes atualiza o intellisense por documento.
    connect(core::KeywordIndex::instance(), &core::KeywordIndex::scanReady,
            this, &CodeEditorWidget::onLanguageScanReady);

    // Auto-completar chaves/colchetes (tratado em eventFilter).
    m_editor->installEventFilter(this);

    // Hover (mouse parado) para tooltip de valor no depurador.
    m_editor->setMouseTracking(true);
    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    m_hoverTimer->setInterval(450);
    connect(m_hoverTimer, &QTimer::timeout, this, [this] {
        if (!m_hoverWord.isEmpty())
            emit hoverWordRequested(m_hoverWord, m_hoverPos);
    });

    // Margem de breakpoints clicavel (Parte 5 a consome).
    connect(m_editor, &QsciScintilla::marginClicked, this,
            [this](int margin, int line, Qt::KeyboardModifiers) {
                if (margin == kBreakpointMargin)
                    emit breakpointMarginClicked(line);
            });

    connect(m_scopeBox, &QComboBox::activated,
            this, &CodeEditorWidget::onScopeSelected);
    connect(m_memberBox, &QComboBox::activated,
            this, &CodeEditorWidget::onMemberSelected);
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, &CodeEditorWidget::onCursorPositionChanged);
}

void CodeEditorWidget::configureCommonStyle(QsciScintilla* ed)
{
    // (O lexer de cada linguagem e definido em applyLanguage(); aqui ficam
    // apenas as preferencias compartilhadas de todos os documentos.)
    ed->setCaretForegroundColor(QColor(0x10, 0x10, 0x10));
    ed->setCaretWidth(2);
    ed->setSelectionBackgroundColor(QColor(0xAD, 0xC4, 0xEE));

    ed->setUtf8(true);
    ed->setWrapMode(QsciScintilla::WrapNone);

    // Indentacao + tabulacao
    ed->setTabWidth(4);
    ed->setIndentationsUseTabs(false);
    ed->setAutoIndent(true);
    ed->setBackspaceUnindents(true);

    // Chaves casadas
    ed->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    ed->setMatchedBraceForegroundColor(QColor(Qt::red));
    ed->setMatchedBraceBackgroundColor(QColor(0xFF, 0xF0, 0xC0));

    // Linha do cursor destacada (estilo VS2005)
    ed->setCaretLineVisible(true);
    ed->setCaretLineBackgroundColor(QColor(0xF0, 0xF5, 0xFC));

    // --- Intellisense (autocompletar) -----------------------------------
    // Palavras/APIs por linguagem vêm do KeywordIndex via applyLanguage();
    // aqui apenas o comportamento: lista de todas as fontes, filtro a cada
    // tecla, abre ate com 1 caractere.
    ed->setAutoCompletionSource(QsciScintilla::AcsAll);
    ed->setAutoCompletionCaseSensitivity(false);
    ed->setAutoCompletionThreshold(1);
    ed->setAutoCompletionShowSingle(false);

    // --- Margens ---------------------------------------------------------
    // 0: numeros de linha
    ed->setMarginType(kLineNumberMargin, QsciScintilla::NumberMargin);
    ed->setMarginLineNumbers(kLineNumberMargin, true);
    ed->setMarginWidth(kLineNumberMargin, QString(QLatin1Char('0')).repeated(5));

    // 1: simbolos (breakpoints / setas de debug)
    ed->setMarginType(kBreakpointMargin, QsciScintilla::SymbolMargin);
    ed->setMarginWidth(kBreakpointMargin, 16);
    ed->setMarginSensitivity(kBreakpointMargin, true);

    // Markers de debug (ponto vermelho = breakpoint, seta amarela = execucao)
    {
        QPixmap bp(16, 16);
        bp.fill(Qt::transparent);
        {
            QPainter p(&bp);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(QColor(0xC0, 0x00, 0x00));
            p.setPen(QColor(Qt::white));
            p.drawEllipse(QRectF(2.5, 2.5, 11, 11));
        }
        ed->markerDefine(bp, kBreakpointMarker);

        QPixmap arr(16, 16);
        arr.fill(Qt::transparent);
        {
            QPainter p(&arr);
            p.setRenderHint(QPainter::Antialiasing);
            QPolygon tri;
            tri << QPoint(3, 2) << QPoint(14, 8) << QPoint(3, 14);
            p.setBrush(QColor(0xFF, 0xD0, 0x00));
            p.setPen(QColor(0x80, 0x60, 0x00));
            p.drawPolygon(tri);
        }
        ed->markerDefine(arr, kExecutionMarker);

        // Realce de fundo da linha de execucao (SC_MARK_BACKGROUND): cor
        // visivel mesmo sem o cursor na linha (VS2005).
        ed->markerDefine(QsciScintilla::Background, kExecutionLineMarker);
        ed->setMarkerForegroundColor(QColor(0xFF, 0xF4, 0xB0),
                                     kExecutionLineMarker);

        // Breakpoint desativado: circulo cinza (nao dispara no gdb).
        ed->markerDefine(QsciScintilla::Circle, kBreakpointDisabledMarker);
        ed->setMarkerForegroundColor(QColor(0x9A, 0x9A, 0x9A),
                                     kBreakpointDisabledMarker);
        ed->setMarkerBackgroundColor(QColor(0xCF, 0xCF, 0xCF),
                                     kBreakpointDisabledMarker);
    }

    // 2: dobramento de codigo
    ed->setMarginType(kFoldMargin, QsciScintilla::SymbolMargin);
    ed->setFolding(QsciScintilla::BoxedTreeFoldStyle, kFoldMargin);
    ed->setFoldMarginColors(QColor(0xDF, 0xE2, 0xE6), QColor(0xF4, 0xF5, 0xF7));
    ed->setMarginSensitivity(kFoldMargin, true);

    // Cor de fundo das margins segue a janela (VS2005)
    ed->setMarginsBackgroundColor(QColor(0xF4, 0xF5, 0xF7));
    ed->setMarginsForegroundColor(QColor(0x66, 0x66, 0x66));
    QFont marginFont = editorFont();
    marginFont.setPointSize(8);
    ed->setMarginsFont(marginFont);

    // Indicador de erro (SquiggleUnder draw)
    ed->setIndicatorDrawUnder(true, kErrorIndicator);
    ed->setIndicatorForegroundColor(QColor(0xC0, 0x00, 0x00), kErrorIndicator);
    ed->setIndicatorOutlineColor(QColor(0xC0, 0x00, 0x00), kErrorIndicator);

    // Annotations (mensagens de erro do compilador)
    ed->setAnnotationDisplay(QsciScintilla::AnnotationStandard);

    // Sombra/edge opcional a 100 colunas
    ed->setEdgeColumn(100);
    ed->setEdgeMode(QsciScintilla::EdgeLine);
    ed->setEdgeColor(QColor(0xE0, 0xE0, 0xE0));
}

void CodeEditorWidget::applyLanguage(const QString& language)
{
    // Linguagem (a extensao do arquivo determina; padrao C++).
    m_language = language.isEmpty() ? QStringLiteral("cpp") : language;

    // Troca o lexer (e libera o anterior + suas APIs).
    QsciLexer* old = m_editor->lexer();
    QsciLexer* lexer = nullptr;
    if (m_language == QLatin1String("cpp"))
        lexer = new QsciLexerCPP(m_editor);
    else if (m_language == QLatin1String("sql"))
        lexer = new QsciLexerSQL(m_editor);
    else if (m_language == QLatin1String("bash"))
        lexer = new QsciLexerBash(m_editor);
    // (asm fica sem lexer: QsciLexerAsm e abstrato na versao instalada.)
    m_editor->setLexer(lexer);
    delete old;

    if (lexer) {
        lexer->setFont(editorFont());
        lexer->setPaper(QColor(0xFF, 0xFF, 0xFF));
    }

    // Lista base (arquivo proprio da linguagem) + scan ja cacheado.
    core::KeywordIndex* kw = core::KeywordIndex::instance();
    m_apiWords = kw->defaultKeywords(m_language);
    const QStringList cached = kw->cachedKeywordsFor(m_filePath);
    for (const QString& w : cached) {
        if (!m_apiWords.contains(w))
            m_apiWords.append(w);
    }

    m_apis = lexer ? new QsciAPIs(lexer) : nullptr;
    rebuildApis();
    requestLanguageScan();
}

void CodeEditorWidget::rebuildApis()
{
    if (!m_apis)
        return;
    m_apis->clear();
    for (const QString& w : m_apiWords)
        m_apis->add(w);
    // gera a API preparada em background (o QScintilla consome sob demanda;
    // nao bloqueamos o GUI nem esperamos o arquivo .qsci ficar pronto).
    m_apis->prepare();
}

void CodeEditorWidget::requestLanguageScan()
{
    if (!core::KeywordIndex::scansIncludes(m_language)
        || m_filePath.isEmpty())
        return;
    core::KeywordIndex::instance()->requestScan(m_filePath, m_language);
}

void CodeEditorWidget::onLanguageScanReady(const QString& filePath,
                                           const QString& language,
                                           const QStringList& extraWords)
{
    if (filePath != m_filePath || language != m_language)
        return;
    // Merge: base da linguagem + tokens escaneados (dedupe preservando ordem).
    QStringList merged = core::KeywordIndex::instance()->defaultKeywords(m_language);
    for (const QString& w : extraWords) {
        if (!merged.contains(w))
            merged.append(w);
    }
    m_apiWords = merged;
    rebuildApis();
}

void CodeEditorWidget::setupNavigationBar()
{
    auto* bar = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(6);

    auto* lb1 = new QLabel(QObject::tr("Classe:"), bar);
    m_scopeBox = new QComboBox(bar);
    m_scopeBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_scopeBox->setToolTip(QObject::tr("Classes / namespaces do arquivo atual"));

    auto* lb2 = new QLabel(QObject::tr("Membro:"), bar);
    m_memberBox = new QComboBox(bar);
    m_memberBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_memberBox->setToolTip(QObject::tr("Funcoes/metodos da selecao"));

    layout->addWidget(lb1);
    layout->addWidget(m_scopeBox, 1);
    layout->addWidget(lb2);
    layout->addWidget(m_memberBox, 1);

    bar->setObjectName("navigationBar");
    bar->setFixedHeight(30);
}

// ---------------------------------------------------------------------------
// Arquivo / outline
// ---------------------------------------------------------------------------

bool CodeEditorWidget::openFile(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "CodeEditorWidget: falha ao abrir" << filePath;
        return false;
    }
    m_filePath = filePath;
    m_editor->setText(QString::fromUtf8(f.readAll()));
    m_editor->setModified(false);
    m_editor->setCursorPosition(0, 0);

    // Lexer/intellisense conforme a linguagem (extensao) do arquivo.
    applyLanguage(core::KeywordIndex::languageForFile(filePath));

    refreshOutline();
    emit documentLoaded(filePath);
    return true;
}

void CodeEditorWidget::refreshOutline()
{
    m_outline = editor::CodeOutlineParser::parse(m_editor->text());
    populateNavigationBars();
    emit outlineUpdated();
}

// ---------------------------------------------------------------------------
// Navigation bars
// ---------------------------------------------------------------------------

namespace {

//! Soh classes/structs aparecem na combo esquerda (paginado pelo cursor).
bool isScopeListed(editor::ScopeKind kind)
{
    return kind == editor::ScopeKind::Class || kind == editor::ScopeKind::Struct;
}

}  // namespace

void CodeEditorWidget::populateNavigationBars()
{
    m_updatingNav = true;

    // Combo esquerda: classes/structs rotuladas como "namespace::classe".
    m_scopeBox->clear();
    m_scopeBox->addItem(QObject::tr("(global)"), -1);
    for (int i = 0; i < m_outline.scopes.size(); ++i) {
        if (isScopeListed(m_outline.scopes[i].kind))
            m_scopeBox->addItem(m_outline.scopes[i].fullPath, i);
    }

    // Combo direita: apenas FUNCOES/metodos do escopo selecionado.
    m_memberBox->clear();
    m_memberBox->addItem(QObject::tr("(nenhum)"), -1);
    const int currentScope = m_scopeBox->currentData().toInt();
    fillFunctionMembers(currentScope);

    m_updatingNav = false;
    updateNavigationForCursor(m_editor->firstVisibleLine());
}

void CodeEditorWidget::fillFunctionMembers(int scopeIdx)
{
    const QVector<editor::OutlineMember>* members =
        (scopeIdx >= 0) ? &m_outline.scopes[scopeIdx].members
                        : &m_outline.globalMembers;
    for (const auto& m : *members) {
        if (!m.isFunction)
            continue;  // somente funcoes/metodos
        QString text = (m.isOutOfLine ? QLatin1String(":: ") : QString())
            + m.name;
        m_memberBox->addItem(text, static_cast<int>(m.line));
        m_memberBox->setItemData(m_memberBox->count() - 1, m.signature,
                                 Qt::ToolTipRole);
    }
}

void CodeEditorWidget::onScopeSelected(int index)
{
    if (m_updatingNav)
        return;
    m_updatingNav = true;
    m_memberBox->clear();
    m_memberBox->addItem(QObject::tr("(nenhum)"), -1);

    const int scopeIdx = m_scopeBox->itemData(index).toInt();
    fillFunctionMembers(scopeIdx);
    m_updatingNav = false;

    if (m_memberBox->count() == 1) {
        // escopo sem funcoes: pula para a declaracao do proprio escopo
        if (scopeIdx >= 0)
            jumpToLine(m_outline.scopes[scopeIdx].line);
    } else {
        m_memberBox->setCurrentIndex(1);
        jumpToLine(m_memberBox->itemData(1).toInt());
    }
}

void CodeEditorWidget::onMemberSelected(int index)
{
    if (m_updatingNav || index <= 0)
        return;
    const int line = m_memberBox->itemData(index).toInt();
    if (line >= 0)
        jumpToLine(line);
}

// Sincroniza as duas combos com a posicao real do cursor (estilo VS2005).
void CodeEditorWidget::onCursorPositionChanged(int line, int)
{
    if (m_updatingNav)
        return;
    updateNavigationForCursor(line);
}

//! Escopo listado (classe/struct) mais interno que contem a linha (-1 global).
int CodeEditorWidget::scopeAtLine(int line) const
{
    int bestIdx = -1;
    int bestLine = -1;
    int bestDepth = -1;
    for (int i = 0; i < m_outline.scopes.size(); ++i) {
        const auto& s = m_outline.scopes[i];
        if (!isScopeListed(s.kind) || s.line > line)
            continue;
        // recente / mais aninhado vence
        if (s.line > bestLine ||
            (s.line == bestLine && s.openDepth > bestDepth)) {
            bestIdx = i;
            bestLine = s.line;
            bestDepth = s.openDepth;
        }
    }
    return bestIdx;
}

int CodeEditorWidget::rowForScope(int scopeIdx) const
{
    for (int r = 0; r < m_scopeBox->count(); ++r)
        if (m_scopeBox->itemData(r).toInt() == scopeIdx)
            return r;
    return 0;  // (global)
}

void CodeEditorWidget::updateNavigationForCursor(int line)
{
    m_updatingNav = true;

    const int scopeIdx = scopeAtLine(line);
    const int comboIdx = rowForScope(scopeIdx);
    if (m_scopeBox->currentIndex() != comboIdx)
        m_scopeBox->setCurrentIndex(comboIdx);

    // Funcao mais proxima (acima ou na propria linha), so entre funcoes.
    const QVector<editor::OutlineMember>* members =
        (scopeIdx >= 0) ? &m_outline.scopes[scopeIdx].members
                        : &m_outline.globalMembers;
    int fnCount = 0;
    int selected = 0;  // "(nenhum)"
    for (const auto& m : *members) {
        if (!m.isFunction)
            continue;
        ++fnCount;
        if (m.line <= line)
            selected = fnCount;
    }

    if ((m_memberBox->count() - 1) != fnCount)
        populateNavigationBars();  // escopo mudou; repopula de forma segura
    else if (m_memberBox->currentIndex() != selected)
        m_memberBox->setCurrentIndex(selected);

    m_updatingNav = false;
}

void CodeEditorWidget::jumpToLine(int line)
{
    m_editor->setCursorPosition(line, 0);
    m_editor->ensureLineVisible(line + 1);
    m_editor->setFocus();
}

void CodeEditorWidget::gotoLine(int line)
{
    jumpToLine(line);
}

// ---------------------------------------------------------------------------
// Auto-fechamento de pares ({, (, [) com suporte a wrap de selecao
// ---------------------------------------------------------------------------

bool CodeEditorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_editor) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            const Qt::KeyboardModifiers mods = ke->modifiers();
            if (!(mods & (Qt::ControlModifier | Qt::AltModifier
                          | Qt::MetaModifier))) {
                const QString text = ke->text();
                if (text.size() == 1) {
                    const QChar c = text.at(0);
                    if (c == QLatin1Char(':')) {
                        // "std::" -> lista de todos os membros conhecidos
                        // (VS2005). O ':' nao e caractere de palavra, entao o
                        // autocompletar automatico nao abriria por si so.
                        QTimer::singleShot(0, this, [this] {
                            triggerNamespaceCompletion();
                        });
                    } else if (c == QLatin1Char('{')
                               || c == QLatin1Char('(')
                               || c == QLatin1Char('[')) {
                        completeBracketPair(c);
                        ke->accept();
                        return true;
                    }
                }
            }
        } else if (event->type() == QEvent::MouseMove) {
            // Hover: debounce -> palavra sob o mouse para o depurador.
            const auto* me = static_cast<QMouseEvent*>(event);
            const long pos = m_editor->SendScintilla(
                QsciScintillaBase::SCI_POSITIONFROMPOINT,
                (unsigned long)me->pos().x(), (unsigned long)me->pos().y());
            if (pos < 0) {
                m_hoverTimer->stop();
                m_hoverWord.clear();
                return QWidget::eventFilter(watched, event);
            }
            int line = 0, index = 0;
            m_editor->lineIndexFromPosition(static_cast<int>(pos), &line,
                                            &index);
            const QString word = m_editor->wordAtLineIndex(line, index);
            m_hoverWord = word;
            if (word.isEmpty()) {
                m_hoverTimer->stop();
                return QWidget::eventFilter(watched, event);
            }
            m_hoverPos = me->globalPosition().toPoint();
            m_hoverTimer->start();
        } else if (event->type() == QEvent::MouseButtonPress
                   || event->type() == QEvent::Leave) {
            m_hoverTimer->stop();
            m_hoverWord.clear();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CodeEditorWidget::completeBracketPair(const QChar open)
{
    QChar close;
    if (open == QLatin1Char('{'))
        close = QLatin1Char('}');
    else if (open == QLatin1Char('('))
        close = QLatin1Char(')');
    else if (open == QLatin1Char('['))
        close = QLatin1Char(']');
    else
        return;

    int cursorLine, cursorIdx;
    m_editor->getCursorPosition(&cursorLine, &cursorIdx);

    m_editor->beginUndoAction();

    if (m_editor->hasSelectedText()) {
        // getSelection() so tem valores validos quando ha selecao.
        int selFromLine = 0, selFromIdx = 0, selToLine = 0, selToIdx = 0;
        m_editor->getSelection(&selFromLine, &selFromIdx, &selToLine, &selToIdx);

        QString sel = m_editor->selectedText();
        sel.replace(QChar(0x2029), QLatin1Char('\n'));
        sel.replace(QChar(0x2028), QLatin1Char('\n'));

        m_editor->replaceSelectedText(QString(open) + sel + QString(close));
        // Cursor apos o caractere de abertura.
        m_editor->setCursorPosition(selFromLine, selFromIdx + 1);
        m_editor->ensureLineVisible(selFromLine + 1);
    } else {
        // ATENCAO: QsciScintilla::insert() NAO desloca o cursor. Insere o
        // par de uma so vez (evita o cursor ficar antes do texto e o par
        // sair invertido: "}{") e posiciona entre os dois.
        m_editor->insert(QString(open) + QString(close));
        m_editor->setCursorPosition(cursorLine, cursorIdx + 1);
        m_editor->ensureLineVisible(cursorLine + 1);
    }

    m_editor->endUndoAction();
}

void CodeEditorWidget::triggerNamespaceCompletion()
{
    // So C/C++ usa "::".
    if (m_language != QLatin1String("cpp") || m_apiWords.isEmpty())
        return;
    int line = 0, index = 0;
    m_editor->getCursorPosition(&line, &index);
    if (index < 3)
        return;
    // SCI_GETCHARAT usa offset FLAT do documento; converte (linha, coluna).
    auto charAt = [this](int pos) {
        return QChar(static_cast<ushort>(m_editor->SendScintilla(
            QsciScintillaBase::SCI_GETCHARAT, (unsigned long)pos)));
    };
    const int pos = m_editor->positionFromLineIndex(line, index);
    if (charAt(pos - 1) != QLatin1Char(':')
        || charAt(pos - 2) != QLatin1Char(':'))
        return;
    const QChar before = charAt(pos - 3);
    if (!before.isLetterOrNumber() && before != QLatin1Char('_')
        && before != QLatin1Char('~'))
        return;
    // Mostra a lista completa de simbolos conhecidos via SCI_AUTOCSHOW
    // (independe de foco/tecla-de-palavra, que o "std::" nao dispara).
    QStringList words = m_apiWords;
    words.sort();
    const QByteArray list = words.join(QLatin1Char(' ')).toUtf8();
    m_editor->SendScintilla(QsciScintillaBase::SCI_AUTOCSHOW, 0UL,
                            list.constData());
}

// ---------------------------------------------------------------------------
// Breakpoints / execucao do depurador (Parte 5)
// ---------------------------------------------------------------------------

void CodeEditorWidget::setBreakpoints(const QList<int>& lines,
                                      const QSet<int>& disabled)
{
    m_editor->markerDeleteAll(kBreakpointMarker);
    m_editor->markerDeleteAll(kBreakpointDisabledMarker);
    QList<int> sorted = lines;
    std::sort(sorted.begin(), sorted.end());
    int last = -1;
    for (int line : sorted) {
        if (line < 0 || line == last)
            continue;
        if (disabled.contains(line))
            m_editor->markerAdd(line, kBreakpointDisabledMarker);
        else
            m_editor->markerAdd(line, kBreakpointMarker);
        last = line;
    }
}

void CodeEditorWidget::clearAllBreakpoints()
{
    m_editor->markerDeleteAll(kBreakpointMarker);
    m_editor->markerDeleteAll(kBreakpointDisabledMarker);
}

void CodeEditorWidget::setExecutionLine(int line)
{
    m_editor->markerDeleteAll(kExecutionMarker);
    m_editor->markerDeleteAll(kExecutionLineMarker);
    if (line >= 0) {
        m_editor->markerAdd(line, kExecutionLineMarker);
        m_editor->markerAdd(line, kExecutionMarker);
    }
}

// ---------------------------------------------------------------------------
// Erros do compilador (indicador + annotation)
// ---------------------------------------------------------------------------

void CodeEditorWidget::setCompilerErrors(
    const QVector<std::pair<int, QString>>& errors)
{
    clearCompilerErrors();
    for (const auto& [line, msg] : errors) {
        m_editor->fillIndicatorRange(line, 0, 3, 1, kErrorIndicator);
        m_editor->annotate(line, msg, 0);
    }
}

void CodeEditorWidget::clearCompilerErrors()
{
    m_editor->clearIndicatorRange(0, 0, -1, -1, kErrorIndicator);
    m_editor->clearAnnotations();
}

QsciScintilla* CodeEditorWidget::editor() const
{
    return m_editor;
}

}  // namespace ui