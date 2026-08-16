/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gui/tea_qt.cpp — the Qt desktop shell (tea-qt).  The only C++ TU in
 * the tree: a thin shell over src/tea_embed.h; the core stays C17.
 * Worker thread runs all embed calls; models refresh between
 * commands; stdout/stderr captured at fd level by OutputPump.
 * --smoke <dofile> is the headless gate (make gui-test).
 */
#include <QtWidgets>
#include <QtSvgWidgets/QSvgWidget>
#include <unistd.h>
#include <cstdio>
#include "../src/tea_embed.h"

/* the real stderr, saved before OutputPump redirects fd 2 — smoke
 * diagnostics must reach the invoking shell, not the results widget */
static int g_real_err = 2;

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

class Worker : public QObject {
    Q_OBJECT
public slots:
    void init()                     { tea_embed_init(); emit ready(); }
    void execLine(const QString &s) {
        int more = tea_embed_exec(s.toUtf8().constData());
        emit lineDone(tea_embed_last_rc(), more == 1);
    }
    void runDofile(const QString &p) {
        int rc = tea_embed_run_dofile(p.toUtf8().constData());
        emit lineDone(rc, false);
    }
signals:
    void ready();
    void lineDone(int rc, bool needMore);
};

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
        resize(1200, 800);

        m_results = new QPlainTextEdit;
        m_results->setReadOnly(true);
        m_results->setMaximumBlockCount(200000);
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::TypeWriter);
        m_results->setFont(mono);

        m_cmd = new QLineEdit;
        m_cmd->setFont(mono);
        m_cmd->setPlaceholderText(QStringLiteral("type a command — Tab completes, Up/Down history"));
        m_cmd->installEventFilter(this);

        m_break = new QToolButton;
        m_break->setText(QStringLiteral("Break"));
        m_break->setEnabled(false);

        auto *cmdRow = new QWidget;
        auto *h = new QHBoxLayout(cmdRow);
        h->setContentsMargins(4, 2, 4, 4);
        auto *dot = new QLabel(QStringLiteral("."));
        dot->setFont(mono);
        h->addWidget(dot);
        h->addWidget(m_cmd, 1);
        h->addWidget(m_break);

        auto *center = new QWidget;
        auto *v = new QVBoxLayout(center);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(m_results, 1);
        v->addWidget(cmdRow);
        setCentralWidget(center);

        m_varsModel = new VarsModel;
        auto *varsView = new QTableView;
        varsView->setModel(m_varsModel);
        varsView->horizontalHeader()->setStretchLastSection(true);
        varsView->verticalHeader()->setVisible(false);
        varsView->setSelectionBehavior(QAbstractItemView::SelectRows);
        varsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        connect(varsView, &QTableView::doubleClicked, this, [this](const QModelIndex &ix){
            m_cmd->insert(QString::fromUtf8(tea_embed_var_name(ix.row())) + QChar(' '));
            m_cmd->setFocus();
        });
        addDockWidget(Qt::LeftDockWidgetArea, makeDock(QStringLiteral("Variables"), varsView, "vars"));

        m_history = new QListWidget;
        m_history->setFont(mono);
        connect(m_history, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem *it){ submit(it->text()); });
        addDockWidget(Qt::LeftDockWidgetArea, makeDock(QStringLiteral("History"), m_history, "hist"));

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
        dataDock->raise();

        m_worker = new Worker;
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(this, &MainWindow::reqInit,    m_worker, &Worker::init);
        connect(this, &MainWindow::reqExec,    m_worker, &Worker::execLine);
        connect(this, &MainWindow::reqDofile,  m_worker, &Worker::runDofile);
        connect(m_worker, &Worker::ready,    this, &MainWindow::onReady);
        connect(m_worker, &Worker::lineDone, this, &MainWindow::onLineDone);
        connect(m_break, &QToolButton::clicked, this, []{ tea_embed_interrupt(); });
        m_thread.start();
        emit reqInit();

        auto *file = menuBar()->addMenu(QStringLiteral("&File"));
        QAction *aDo = file->addAction(QStringLiteral("&Run do-file..."));
        aDo->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        connect(aDo, &QAction::triggered, this, [this]{
            QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Run do-file"),
                        QString(), QStringLiteral("do-files (*.do);;all files (*)"));
            if (!p.isEmpty()) submit(QStringLiteral("do \"%1\"").arg(p));
        });
        QAction *aOpen = file->addAction(QStringLiteral("&Open data..."));
        aOpen->setShortcut(QKeySequence::Open);
        connect(aOpen, &QAction::triggered, this, [this]{
            QString p = QFileDialog::getOpenFileName(this, QStringLiteral("Open data"),
                        QString(), QStringLiteral("data (*.dta *.tea *.csv *.tsv *.xlsx);;all files (*)"));
            if (!p.isEmpty()) submit(QStringLiteral("use \"%1\", clear").arg(p));
        });
        file->addSeparator();
        QAction *aQuit = file->addAction(QStringLiteral("&Quit"));
        aQuit->setShortcut(QKeySequence::Quit);
        connect(aQuit, &QAction::triggered, qApp, &QApplication::quit);
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
        appendText(QStringLiteral("%1 %2 — ready\n")
                   .arg(QString::fromUtf8(
#ifdef TEA_DECAF
                       "decaf tea"
#else
                       "tea"
#endif
                   ), QString::fromUtf8(tea_embed_version())), false);
        if (m_smoke) { m_busy = true; emit reqDofile(m_smokeFile); }
        else m_cmd->setFocus();
    }

    void onText(const QString &t, bool isErr) {
        appendText(t, isErr);
        if (m_smoke) m_smokeOut += t;
    }

    void onLineDone(int rc, bool needMore) {
        m_busy = false;
        m_break->setEnabled(false);
        m_cmd->setEnabled(true);
        m_needMore = needMore;
        m_dataModel->refresh();
        m_varsModel->refresh();
        refreshPlot();
        if (m_smoke) {
            /* defer so the pump's queued output signals drain first */
            QTimer::singleShot(300, this, [this, rc]{ verdict(rc); });
            return;
        }
        m_cmd->setFocus();
    }

private:
    QDockWidget *makeDock(const QString &title, QWidget *w, const char *key) {
        auto *d = new QDockWidget(title);
        d->setObjectName(QStringLiteral("dock_%1").arg(QString::fromUtf8(key)));
        d->setWidget(w);
        return d;
    }

    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == m_cmd && e->type() == QEvent::KeyPress) {
            auto *k = static_cast<QKeyEvent *>(e);
            if (k->key() == Qt::Key_Up)   { histMove(-1); return true; }
            if (k->key() == Qt::Key_Down) { histMove(+1); return true; }
            if (k->key() == Qt::Key_Tab)  { complete();   return true; }
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
                submit(m_cmd->text()); return true;
            }
        }
        return QMainWindow::eventFilter(o, e);
    }

    void submit(const QString &line) {
        if (m_busy) return;
        appendText((m_needMore ? QStringLiteral("> %1\n") : QStringLiteral(". %1\n"))
                   .arg(line), false);
        if (!line.trimmed().isEmpty()) {
            m_history->addItem(line);
            m_histPos = m_history->count();
        }
        m_cmd->clear();
        m_busy = true;
        m_break->setEnabled(true);
        m_cmd->setEnabled(false);
        emit reqExec(line);
    }

    void histMove(int d) {
        int n = m_history->count();
        if (!n) return;
        m_histPos = qBound(0, m_histPos + d, n);
        m_cmd->setText(m_histPos < n ? m_history->item(m_histPos)->text()
                                     : QString());
    }

    void complete() {
        if (m_busy) return;
        char out[8192];
        QByteArray line = m_cmd->text().toUtf8();
        int n = tea_embed_complete(line.constData(), m_cmd->cursorPosition(),
                                   out, sizeof out);
        if (n <= 0) return;
        QStringList cands = QString::fromUtf8(out).split(QChar('\n'),
                                                         Qt::SkipEmptyParts);
        if (cands.size() == 1) {
            QString t = m_cmd->text();
            int p = m_cmd->cursorPosition(), s = p;
            while (s > 0 && !t.at(s - 1).isSpace()) --s;
            t.replace(s, p - s, cands.first());
            m_cmd->setText(t);
        } else {
            appendText(cands.join(QStringLiteral("  ")) + QStringLiteral("\n"),
                       false);
        }
    }

    void appendText(const QString &t, bool isErr) {
        QTextCharFormat f;
        if (isErr) f.setForeground(QColor(200, 60, 40));
        QTextCursor c = m_results->textCursor();
        c.movePosition(QTextCursor::End);
        c.setCharFormat(f);
        c.insertText(t);
        m_results->setTextCursor(c);
    }

    void refreshPlot() {
        QString p = QDir::current().filePath(QStringLiteral("tea_graph.svg"));
        QFileInfo fi(p);
        if (!fi.exists()) return;
        if (fi.lastModified() == m_plotStamp) return;
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
        dprintf(g_real_err, "gui smoke: %s (rows=%d cols=%d)\n",
                ok ? "PASS" : "FAIL",
                m_dataModel->rowCount(), m_dataModel->columnCount());
        QCoreApplication::exit(ok ? 0 : 1);
    }

    QPlainTextEdit *m_results{};
    QLineEdit      *m_cmd{};
    QToolButton    *m_break{};
    QListWidget    *m_history{};
    QSvgWidget     *m_svg{};
    QDockWidget    *m_plotDock{};
    DataModel      *m_dataModel{};
    VarsModel      *m_varsModel{};
    Worker         *m_worker{};
    QThread         m_thread;
    QDateTime       m_plotStamp;
    int             m_histPos = 0;
    bool            m_busy = false, m_needMore = false, m_smoke = false;
    QString         m_smokeOut, m_smokeFile;
};

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
