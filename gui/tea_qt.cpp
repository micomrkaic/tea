/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gui/tea_qt.cpp — the Qt desktop shell (tea-qt).  The only C++ TU in
 * the tree: a thin shell over src/tea_embed.h; the core stays C17.
 *
 *   - Console: terminal-style REPL — the prompt lives at the tail of
 *     the results document and input interleaves with output, exactly
 *     like the CLI.  History (Up/Down), completion (Tab), and the
 *     document before the prompt is read-only.
 *   - Do-file editor dock: open/save/run file/run selection; running
 *     writes the buffer to a temp file and feeds it through `do`, so
 *     what you see is what runs — no save required.
 *   - Data browser + variables panes: QAbstractTableModels reading
 *     the frame in place via the embed accessors, refreshed between
 *     commands only.
 *   - Plots dock: watches tea_graph.svg, BASELINED AT STARTUP — a
 *     stale SVG from an earlier session in the same directory must
 *     never surface (it did; that was a bug).  Only graphs written
 *     after launch show.
 *   - Worker QThread runs every embed call; Break is the API's
 *     designated any-thread interrupt.
 *   - OutputPump captures stdout/stderr at the fd level; the smoke
 *     verdict writes to a dup of the REAL stderr taken before the
 *     pump replaces fd 2.
 *
 * --smoke <dofile>: headless gate (QT_QPA_PLATFORM=offscreen); runs
 * the do-file through the worker and asserts output reached both the
 * capture and the console, the models see the frame, and no stale
 * plot surfaced.  Wired as `make gui-test`.
 */
#include <QtWidgets>
#include <QtSvgWidgets/QSvgWidget>
#include <unistd.h>
#include <cstdio>
#include <functional>
#include "../src/tea_embed.h"

/* a dup of the real stderr, taken before the pump steals fd 2 */
static int g_real_err = 2;

/* ================= output pump: fd-level capture ================= */

class OutputPump : public QObject {
    Q_OBJECT
public:
    void start() {
        g_real_err = dup(2);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        startOne(1, false);
        startOne(2, true);
    }
signals:
    void textReady(const QString &text, bool isErr);
private:
    void startOne(int fd, bool isErr) {
        int p[2];
        if (pipe(p) != 0) return;
        dup2(p[1], fd);
        ::close(p[1]);
        int rd = p[0];
        auto *t = QThread::create([this, rd, isErr]() {
            char buf[8192];
            ssize_t n;
            while ((n = ::read(rd, buf, sizeof buf)) > 0)
                emit textReady(QString::fromUtf8(buf, int(n)), isErr);
        });
        t->start();
    }
};

/* ================= worker: the core's single thread ============== */

class Worker : public QObject {
    Q_OBJECT
    /* end-of-output sentinel: one \x01 byte down each captured fd.
     * lineDone and the pump's textReady are queued from DIFFERENT
     * threads, so their relative delivery order is unguaranteed — the
     * v1.6.52 console showed the prompt before help's final chunks
     * arrived, the chunks landed after the prompt, and the polluted
     * tail got submitted as commands (the "doom loop").  The window
     * now waits for lineDone AND both sentinels before prompting. */
    static void eol() { const char b = 1; (void)!write(1, &b, 1); (void)!write(2, &b, 1); }
public slots:
    void init()                     { tea_embed_init(); emit ready(); }
    void execLine(const QString &s) {
        int more = tea_embed_exec(s.toUtf8().constData());
        eol();
        emit lineDone(tea_embed_last_rc(), more == 1);
    }
    void runDofile(const QString &p) {
        int rc = tea_embed_run_dofile(p.toUtf8().constData());
        eol();
        emit lineDone(rc, false);
    }
signals:
    void ready();
    void lineDone(int rc, bool needMore);
};

/* ================= console: terminal-style REPL ================== */

class Console : public QPlainTextEdit {
    Q_OBJECT
public:
    Console() {
        setUndoRedoEnabled(false);
        setMaximumBlockCount(200000);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
    }

    /* MainWindow supplies completion (empty result while busy) */
    std::function<QString(const QString &, int)> completer;

    void appendOutput(const QString &t, bool isErr) {
        QTextCharFormat f;
        if (isErr) f.setForeground(QColor(200, 60, 40));
        QTextCursor c(document());
        if (m_hasPrompt) {
            /* terminal semantics: output goes ABOVE the prompt line,
             * prompt and typed tail shift down intact — late chunks
             * can never pollute the editable tail */
            c.setPosition(m_promptStart);
            c.setCharFormat(f);
            c.insertText(t);
            int d = int(t.size());
            m_promptStart += d;
            m_promptPos   += d;
        } else {
            c.movePosition(QTextCursor::End);
            c.setCharFormat(f);
            c.insertText(t);
            setTextCursor(c);
        }
        ensureCursorVisible();
    }

    void showPrompt(bool continuation) {
        QTextCursor c(document());
        c.movePosition(QTextCursor::End);
        if (c.columnNumber() != 0) c.insertText(QStringLiteral("\n"));
        c.setCharFormat(QTextCharFormat());
        m_promptStart = c.position();
        c.insertText(continuation ? QStringLiteral("> ") : QStringLiteral(". "));
        m_promptPos = c.position();
        m_hasPrompt = true;
        setTextCursor(c);
        ensureCursorVisible();
    }

    void promptConsumed() { m_hasPrompt = false; }

    /* History-dock rerun / menu-driven commands land here */
    void injectAndSubmit(const QString &line) {
        if (!m_hasPrompt) return;
        setTail(line);
        submitTail();
    }

    bool hasPrompt() const { return m_hasPrompt; }

signals:
    void command(const QString &line);

protected:
    void keyPressEvent(QKeyEvent *e) override {
        const bool modifies =
            !e->text().isEmpty() || e->key() == Qt::Key_Backspace ||
            e->key() == Qt::Key_Delete || e->matches(QKeySequence::Paste) ||
            e->matches(QKeySequence::Cut);

        if (!m_hasPrompt) {                 /* busy: read-only console */
            if (!modifies) QPlainTextEdit::keyPressEvent(e);
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            submitTail();
            return;
        }
        if (e->key() == Qt::Key_Up)   { histMove(-1); return; }
        if (e->key() == Qt::Key_Down) { histMove(+1); return; }
        if (e->key() == Qt::Key_Tab)  { complete();   return; }
        if (e->key() == Qt::Key_Home) {
            QTextCursor c = textCursor();
            if (c.position() >= m_promptPos) {
                c.setPosition(m_promptPos,
                              e->modifiers() & Qt::ShiftModifier
                                  ? QTextCursor::KeepAnchor
                                  : QTextCursor::MoveAnchor);
                setTextCursor(c);
                return;
            }
        }
        if (modifies) {
            QTextCursor c = textCursor();
            /* edits happen in the tail only; jump there if elsewhere */
            if (c.position() < m_promptPos ||
                (c.hasSelection() && c.selectionStart() < m_promptPos)) {
                c.movePosition(QTextCursor::End);
                setTextCursor(c);
            }
            if (e->key() == Qt::Key_Backspace &&
                textCursor().position() <= m_promptPos &&
                !textCursor().hasSelection())
                return;                      /* don't eat the prompt */
        }
        QPlainTextEdit::keyPressEvent(e);
    }

    void insertFromMimeData(const QMimeData *src) override {
        if (!m_hasPrompt) return;
        QTextCursor c = textCursor();
        if (c.position() < m_promptPos) {
            c.movePosition(QTextCursor::End);
            setTextCursor(c);
        }
        /* multi-line paste: first line into the tail; queue the rest
         * would need submit plumbing — v1 keeps the first line only
         * and appends the remainder flattened with spaces removed of
         * newlines, which matches pasting into a one-line prompt */
        QString t = src->text();
        t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        t.replace(QChar('\n'), QChar(' '));
        insertPlainText(t);
    }

private:
    QString tailText() const {
        QTextCursor c(document());
        c.setPosition(m_promptPos);
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        return c.selectedText();
    }

    void setTail(const QString &t) {
        QTextCursor c(document());
        c.setPosition(m_promptPos);
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        c.insertText(t);
        c.movePosition(QTextCursor::End);
        setTextCursor(c);
    }

    void submitTail() {
        QString line = tailText();
        QTextCursor c(document());
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n"));
        setTextCursor(c);
        m_hasPrompt = false;
        if (!line.trimmed().isEmpty()) {
            m_hist.append(line);
        }
        m_histPos = m_hist.size();
        emit command(line);
    }

    void histMove(int d) {
        if (m_hist.isEmpty()) return;
        m_histPos = qBound(0, m_histPos + d, int(m_hist.size()));
        setTail(m_histPos < m_hist.size() ? m_hist.at(m_histPos) : QString());
    }

    void complete() {
        if (!completer) return;
        QString tail = tailText();
        QTextCursor c = textCursor();
        int point = qBound(0, c.position() - m_promptPos, int(tail.size()));
        QString joined = completer(tail, point);
        if (joined.isEmpty()) return;
        QStringList cands = joined.split(QChar('\n'), Qt::SkipEmptyParts);
        if (cands.size() == 1) {
            /* replace the word being completed, readline-style */
            int s = point;
            while (s > 0 && !tail.at(s - 1).isSpace()) --s;
            QString nt = tail;
            nt.replace(s, point - s, cands.first());
            setTail(nt);
        } else {
            /* print candidates above, then restore prompt + tail */
            QString saved = tail;
            appendOutput(QStringLiteral("\n") +
                         cands.join(QStringLiteral("  ")) +
                         QStringLiteral("\n"), false);
            showPrompt(false);
            setTail(saved);
        }
    }

    int m_promptPos = 0;     /* after the prompt text: tail starts here */
    int m_promptStart = 0;   /* before the prompt text: output inserts here */
    bool m_hasPrompt = false;
    QStringList m_hist;
    int m_histPos = 0;
};

/* ================= do-file editor ================================ */

class DoEditor : public QWidget {
    Q_OBJECT
public:
    DoEditor() {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        auto *bar = new QToolBar;
        bar->setIconSize(QSize(16, 16));
        auto addBtn = [&](const QString &text, const QKeySequence &ks,
                          auto fn) {
            QAction *a = bar->addAction(text);
            if (!ks.isEmpty()) {
                a->setShortcut(ks);
                a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            }
            connect(a, &QAction::triggered, this, fn);
            return a;
        };
        addBtn(QStringLiteral("New"),  QKeySequence(), [this]{ newFile(); });
        addBtn(QStringLiteral("Open"), QKeySequence(), [this]{ open(); });
        addBtn(QStringLiteral("Save"), QKeySequence::Save, [this]{ save(); });
        bar->addSeparator();
        addBtn(QStringLiteral("Run"),
               QKeySequence(QStringLiteral("Ctrl+R")), [this]{ runAll(); });
        addBtn(QStringLiteral("Run selection"),
               QKeySequence(QStringLiteral("Ctrl+Shift+R")),
               [this]{ runSelection(); });
        v->addWidget(bar);

        m_ed = new QPlainTextEdit;
        m_ed->setLineWrapMode(QPlainTextEdit::NoWrap);
        v->addWidget(m_ed, 1);

        m_name = new QLabel(QStringLiteral("untitled.do"));
        m_name->setContentsMargins(6, 2, 6, 2);
        v->addWidget(m_name);
    }

    void setFontAll(const QFont &f) { m_ed->setFont(f); }

signals:
    /* MainWindow feeds this through the worker's `do` path */
    void runRequest(const QString &path, const QString &what);

private:
    void newFile() { m_ed->clear(); m_path.clear(); m_name->setText(QStringLiteral("untitled.do")); }

    void open() {
        QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open do-file"),
                    QString(), QStringLiteral("do-files (*.do);;all files (*)"));
        if (p.isEmpty()) return;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        m_ed->setPlainText(QString::fromUtf8(f.readAll()));
        m_path = p;
        m_name->setText(p);
    }

    void save() {
        if (m_path.isEmpty()) {
            QString p = QFileDialog::getSaveFileName(this, QStringLiteral("Save do-file"),
                        QStringLiteral("untitled.do"), QStringLiteral("do-files (*.do)"));
            if (p.isEmpty()) return;
            m_path = p;
            m_name->setText(p);
        }
        QFile f(m_path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(m_ed->toPlainText().toUtf8());
    }

    void runText(const QString &text, const QString &what) {
        /* run what you see: the buffer goes to a temp file and through
         * `do` — no save required, loops and continuations intact */
        QString p = QDir::temp().filePath(QStringLiteral("tea_qt_editor_run.do"));
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QByteArray b = text.toUtf8();
        f.write(b);
        if (!b.endsWith('\n')) f.write("\n");
        f.close();
        emit runRequest(p, what);
    }

    void runAll()       { runText(m_ed->toPlainText(), QStringLiteral("editor buffer")); }
    void runSelection() {
        QString s = m_ed->textCursor().selectedText();
        s.replace(QChar(0x2029), QChar('\n'));   /* paragraph seps -> newlines */
        if (s.isEmpty()) return;
        runText(s, QStringLiteral("selection"));
    }

    QPlainTextEdit *m_ed{};
    QLabel *m_name{};
    QString m_path;
};

/* ================= models over the embed accessors =============== */

class DataModel : public QAbstractTableModel {
    Q_OBJECT
public:
    void refresh() { beginResetModel(); m_rows = tea_embed_nobs();
                     m_cols = tea_embed_nvar(); endResetModel(); }
    int rowCount(const QModelIndex & = {}) const override { return int(qMin<long>(m_rows, INT_MAX)); }
    int columnCount(const QModelIndex & = {}) const override { return m_cols; }
    QVariant data(const QModelIndex &ix, int role) const override {
        if (!ix.isValid()) return {};
        if (role == Qt::DisplayRole) {
            char buf[512];
            tea_embed_cell(ix.row(), ix.column(), buf, sizeof buf);
            return QString::fromUtf8(buf);
        }
        if (role == Qt::TextAlignmentRole)
            return tea_embed_var_is_str(ix.column())
                 ? QVariant(Qt::AlignLeft  | Qt::AlignVCenter)
                 : QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    QVariant headerData(int s, Qt::Orientation o, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (o == Qt::Horizontal) return QString::fromUtf8(tea_embed_var_name(s));
        return s + 1;
    }
private:
    long m_rows = 0; int m_cols = 0;
};

class VarsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    void refresh() { beginResetModel(); m_n = tea_embed_nvar(); endResetModel(); }
    int rowCount(const QModelIndex & = {}) const override { return m_n; }
    int columnCount(const QModelIndex & = {}) const override { return 4; }
    QVariant data(const QModelIndex &ix, int role) const override {
        if (!ix.isValid() || role != Qt::DisplayRole) return {};
        int j = ix.row();
        switch (ix.column()) {
        case 0: return QString::fromUtf8(tea_embed_var_name(j));
        case 1: return tea_embed_var_is_str(j) ? QStringLiteral("str")
                                               : QStringLiteral("double");
        case 2: return QString::fromUtf8(tea_embed_var_format(j));
        case 3: return QString::fromUtf8(tea_embed_var_label(j));
        }
        return {};
    }
    QVariant headerData(int s, Qt::Orientation o, int role) const override {
        if (role != Qt::DisplayRole || o != Qt::Horizontal) return {};
        static const char *H[4] = {"Variable", "Type", "Format", "Label"};
        return QString::fromUtf8(H[s]);
    }
private:
    int m_n = 0;
};

/* ================= main window =================================== */

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        setWindowTitle(QStringLiteral("%1 %2")
            .arg(QString::fromUtf8(
#ifdef TEA_DECAF
                "decaf tea"
#else
                "tea"
#endif
            ), QString::fromUtf8(tea_embed_version())));
        resize(1280, 840);

        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::TypeWriter);

        /* center: the console IS the command line */
        m_console = new Console;
        m_console->setFont(mono);
        m_console->completer = [this](const QString &line, int point) -> QString {
            if (m_busy) return {};
            char out[8192];
            int n = tea_embed_complete(line.toUtf8().constData(), point,
                                       out, sizeof out);
            return n > 0 ? QString::fromUtf8(out) : QString();
        };
        connect(m_console, &Console::command, this, &MainWindow::onCommand);
        setCentralWidget(m_console);

        /* left: variables + history docks */
        m_varsModel = new VarsModel;
        auto *varsView = new QTableView;
        varsView->setModel(m_varsModel);
        varsView->horizontalHeader()->setStretchLastSection(true);
        varsView->verticalHeader()->setVisible(false);
        varsView->setSelectionBehavior(QAbstractItemView::SelectRows);
        varsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        connect(varsView, &QTableView::doubleClicked, this, [this](const QModelIndex &ix){
            /* insert the variable name at the console tail */
            m_console->insertPlainText(QString::fromUtf8(tea_embed_var_name(ix.row())) + QChar(' '));
            m_console->setFocus();
        });
        addDockWidget(Qt::LeftDockWidgetArea, makeDock(QStringLiteral("Variables"), varsView, "vars"));

        m_history = new QListWidget;
        m_history->setFont(mono);
        connect(m_history, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem *it){
                    if (!m_busy) m_console->injectAndSubmit(it->text());
                });
        addDockWidget(Qt::LeftDockWidgetArea, makeDock(QStringLiteral("History"), m_history, "hist"));

        /* right: data browser, plots, editor docks (tabbed) */
        m_dataModel = new DataModel;
        auto *dataView = new QTableView;
        dataView->setModel(m_dataModel);
        dataView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dataView->horizontalHeader()->setDefaultSectionSize(90);
        dataView->verticalHeader()->setDefaultSectionSize(
            dataView->fontMetrics().height() + 6);
        auto *dataDock = makeDock(QStringLiteral("Data"), dataView, "data");
        addDockWidget(Qt::RightDockWidgetArea, dataDock);

        m_svg = new QSvgWidget;
        auto *scroll = new QScrollArea;
        scroll->setWidget(m_svg);
        scroll->setWidgetResizable(true);
        m_plotDock = makeDock(QStringLiteral("Plots"), scroll, "plots");
        addDockWidget(Qt::RightDockWidgetArea, m_plotDock);
        tabifyDockWidget(dataDock, m_plotDock);

        m_editor = new DoEditor;
        m_editor->setFontAll(mono);
        connect(m_editor, &DoEditor::runRequest, this, &MainWindow::onEditorRun);
        auto *edDock = makeDock(QStringLiteral("Do-file editor"), m_editor, "editor");
        addDockWidget(Qt::RightDockWidgetArea, edDock);
        tabifyDockWidget(m_plotDock, edDock);
        dataDock->raise();

        /* the stale-plot fix: baseline tea_graph.svg's mtime at launch
         * so a leftover graph from an earlier session never surfaces —
         * only graphs written AFTER startup show the dock */
        {
            QFileInfo fi(QDir::current().filePath(QStringLiteral("tea_graph.svg")));
            if (fi.exists()) m_plotStamp = fi.lastModified();
            m_plotDock->hide();
        }

        /* worker thread: the core's home */
        m_worker = new Worker;
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(this, &MainWindow::reqInit,    m_worker, &Worker::init);
        connect(this, &MainWindow::reqExec,    m_worker, &Worker::execLine);
        connect(this, &MainWindow::reqDofile,  m_worker, &Worker::runDofile);
        connect(m_worker, &Worker::ready,    this, &MainWindow::onReady);
        connect(m_worker, &Worker::lineDone, this, &MainWindow::onLineDone);
        m_thread.start();
        emit reqInit();

        /* toolbar: run/open/break */
        auto *tb = addToolBar(QStringLiteral("Run"));
        tb->setObjectName(QStringLiteral("toolbar_run"));
        tb->setMovable(false);
        QAction *aDo = tb->addAction(QStringLiteral("Run do-file…"));
        aDo->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        connect(aDo, &QAction::triggered, this, [this]{
            QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Run do-file"),
                        QString(), QStringLiteral("do-files (*.do);;all files (*)"));
            if (!p.isEmpty() && !m_busy)
                m_console->injectAndSubmit(QStringLiteral("do \"%1\"").arg(p));
        });
        QAction *aOpen = tb->addAction(QStringLiteral("Open data…"));
        aOpen->setShortcut(QKeySequence::Open);
        connect(aOpen, &QAction::triggered, this, [this]{
            QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open data"),
                        QString(), QStringLiteral("data (*.dta *.tea *.csv *.tsv *.xlsx);;all files (*)"));
            if (!p.isEmpty() && !m_busy)
                m_console->injectAndSubmit(QStringLiteral("use \"%1\", clear").arg(p));
        });
        tb->addSeparator();
        m_breakAct = tb->addAction(QStringLiteral("Break"));
        m_breakAct->setEnabled(false);
        connect(m_breakAct, &QAction::triggered, this, []{ tea_embed_interrupt(); });

        /* menus: File mirrors the toolbar; View toggles the docks */
        auto *file = menuBar()->addMenu(QStringLiteral("&File"));
        file->addAction(aDo);
        file->addAction(aOpen);
        file->addSeparator();
        QAction *aQuit = file->addAction(QStringLiteral("&Quit"));
        aQuit->setShortcut(QKeySequence::Quit);
        connect(aQuit, &QAction::triggered, qApp, &QApplication::quit);

        auto *view = menuBar()->addMenu(QStringLiteral("&View"));
        const auto docks = findChildren<QDockWidget *>();
        for (QDockWidget *d : docks) view->addAction(d->toggleViewAction());
    }

    ~MainWindow() override { m_thread.quit(); m_thread.wait(2000); }

    void attachPump(OutputPump *pump) {
        connect(pump, &OutputPump::textReady, this, &MainWindow::onText,
                Qt::QueuedConnection);
    }

    void smokeRun(const QString &dofile) {
        m_smoke = true;
        m_smokeFile = dofile;
    }

signals:
    void reqInit();
    void reqExec(const QString &line);
    void reqDofile(const QString &path);

private slots:
    void onReady() {
        m_console->appendOutput(QStringLiteral("%1 %2 — ready\n")
                   .arg(QString::fromUtf8(
#ifdef TEA_DECAF
                       "decaf tea"
#else
                       "tea"
#endif
                   ), QString::fromUtf8(tea_embed_version())), false);
        if (m_smoke) { m_busy = true; emit reqDofile(m_smokeFile); return; }
        m_console->showPrompt(false);
        m_console->setFocus();
    }

    void onCommand(const QString &line) {
        if (!line.trimmed().isEmpty()) {
            m_history->addItem(line);
            m_history->scrollToBottom();
        }
        m_busy = true;
        m_breakAct->setEnabled(true);
        emit reqExec(line);
    }

    void onEditorRun(const QString &path, const QString &what) {
        if (m_busy) return;
        m_console->promptConsumed();
        m_console->appendOutput(QStringLiteral("· running %1\n").arg(what), false);
        m_busy = true;
        m_breakAct->setEnabled(true);
        emit reqDofile(path);
    }

    void onText(const QString &t, bool isErr) {
        QString clean = t;
        int k = int(clean.count(QChar(1)));
        if (k) {
            clean.remove(QChar(1));
            (isErr ? m_eolErr : m_eolOut) += k;
        }
        if (!clean.isEmpty()) {
            m_console->appendOutput(clean, isErr);
            if (m_smoke) m_smokeOut += clean;
        }
        if (k) maybeFinishLine();
    }

    void onLineDone(int rc, bool needMore) {
        m_lineDone = true;
        m_doneRc = rc;
        m_doneMore = needMore;
        maybeFinishLine();
    }

    void maybeFinishLine() {
        /* the prompt appears only after the command reported done AND
         * every byte it wrote has been drained from both pipes */
        if (!m_lineDone || m_eolOut < 1 || m_eolErr < 1) return;
        m_lineDone = false;
        m_eolOut -= 1;
        m_eolErr -= 1;
        m_busy = false;
        m_breakAct->setEnabled(false);
        m_dataModel->refresh();
        m_varsModel->refresh();
        refreshPlot();
        m_console->showPrompt(m_doneMore);
        if (m_smoke) { verdict(m_doneRc); return; }
    }

private:
    QDockWidget *makeDock(const QString &title, QWidget *w, const char *key) {
        auto *d = new QDockWidget(title);
        d->setObjectName(QStringLiteral("dock_%1").arg(QString::fromUtf8(key)));
        d->setWidget(w);
        return d;
    }

    void refreshPlot() {
        QString p = QDir::current().filePath(QStringLiteral("tea_graph.svg"));
        QFileInfo fi(p);
        if (!fi.exists()) return;
        if (m_plotStamp.isValid() && fi.lastModified() <= m_plotStamp) return;
        if (!m_plotStamp.isValid() && fi.lastModified() == m_plotStamp) return;
        m_plotStamp = fi.lastModified();
        m_svg->load(p);
        m_plotDock->show();
        m_plotDock->raise();
    }

    void verdict(int rc) {
        bool ok = true;
        if (rc != 0) { dprintf(g_real_err, "smoke: dofile rc=%d\n", rc); ok = false; }
        if (m_dataModel->rowCount() <= 0 || m_dataModel->columnCount() <= 0) {
            dprintf(g_real_err, "smoke: data model empty (%d x %d)\n",
                    m_dataModel->rowCount(), m_dataModel->columnCount());
            ok = false;
        }
        if (m_varsModel->rowCount() <= 0) {
            dprintf(g_real_err, "smoke: vars model empty\n"); ok = false;
        }
        if (!m_smokeOut.contains(QStringLiteral("SMOKE_MARK"))) {
            dprintf(g_real_err, "smoke: output marker missing (captured %d chars)\n",
                    int(m_smokeOut.size()));
            ok = false;
        }
        if (!m_console->toPlainText().contains(QStringLiteral("SMOKE_MARK"))) {
            dprintf(g_real_err, "smoke: marker did not reach the console widget\n");
            ok = false;
        }
        if (m_plotDock->isVisible()) {
            dprintf(g_real_err, "smoke: stale plot surfaced (dock visible with no graph command)\n");
            ok = false;
        }
        {
            QString all = m_console->toPlainText();
            if (all.contains(QStringLiteral("unrecognized command"))) {
                dprintf(g_real_err, "smoke: console polluted (output executed as commands)\n");
                ok = false;
            }
            /* prompt-after-output: with the sentinel protocol the
             * document must END at a fresh prompt */
            if (!all.endsWith(QStringLiteral("\n. ")) && !all.endsWith(QStringLiteral(". "))) {
                dprintf(g_real_err, "smoke: document does not end at a prompt\n");
                ok = false;
            }
        }
        dprintf(g_real_err, "gui smoke: %s (rows=%d cols=%d)\n",
                ok ? "PASS" : "FAIL",
                m_dataModel->rowCount(), m_dataModel->columnCount());
        QCoreApplication::exit(ok ? 0 : 1);
    }

    Console        *m_console{};
    QListWidget    *m_history{};
    QSvgWidget     *m_svg{};
    QDockWidget    *m_plotDock{};
    DoEditor       *m_editor{};
    DataModel      *m_dataModel{};
    VarsModel      *m_varsModel{};
    Worker         *m_worker{};
    QAction        *m_breakAct{};
    QThread         m_thread;
    QDateTime       m_plotStamp;
    bool            m_busy = false, m_smoke = false;
    bool            m_lineDone = false, m_doneMore = false;
    int             m_doneRc = 0, m_eolOut = 0, m_eolErr = 0;
    QString         m_smokeOut, m_smokeFile;
};

/* ================= entry ========================================= */

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    auto *pump = new OutputPump;
    pump->start();

    MainWindow w;
    w.attachPump(pump);

    QStringList args = app.arguments();
    int si = args.indexOf(QStringLiteral("--smoke"));
    if (si >= 0 && si + 1 < args.size()) {
        w.smokeRun(args.at(si + 1));
        w.show();
        return app.exec();
    }

    w.show();
    return app.exec();
}

#include "tea_qt.moc"
