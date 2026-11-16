#include "ResultsPanel.hpp"

#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Format.hpp"
#include "PlotWidget.hpp"
#include "Theme.hpp"

namespace {

enum Role { EntryRole = Qt::UserRole, FileRole, KeyRole, NameRole };

QString typeName(const QString& type) {
    static const QHash<QString, QString> names = {
        {"op", QObject::tr("Arbeitspunkt")}, {"dc", QObject::tr("DC-Sweep")},
        {"ac", QObject::tr("Frequenzgang")}, {"tran", QObject::tr("Transient")},
        {"pss", QObject::tr("Periodischer Zustand")}, {"noise", QObject::tr("Rauschen")},
        {"sens", QObject::tr("Empfindlichkeit")}, {"fit", QObject::tr("Anpassung")}};
    return names.value(type, type);
}

QString unitOf(const QString& name) {
    if (name.startsWith("v(")) return "V";
    if (name.startsWith("i(") || name.startsWith("-i(")) return "A";
    return QString();
}

// Scalar values of a summary entry as (name, text) rows; nested objects
// become "group: name".
void flatten(const QJsonObject& o, const QString& prefix, QVector<QPair<QString, QString>>& rows) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = it.key();
        if (prefix.isEmpty() && (key == "type" || key == "name" || key == "file" || key == "measure_file" ||
                                 key == "sweep"))
            continue;
        const QString name = prefix.isEmpty() ? key : prefix + ": " + key;
        if (it->isDouble()) {
            rows.push_back({name, formatSI(it->toDouble(), unitOf(key), 6)});
        } else if (it->isNull()) {
            rows.push_back({name, QObject::tr("nicht bestimmt")});
        } else if (it->isString()) {
            rows.push_back({name, it->toString()});
        } else if (it->isObject()) {
            if (key == "fourier" && it->toObject().contains("file")) {
                QJsonObject f = it->toObject();
                f.remove("file");
                flatten(f, name, rows);
            } else {
                flatten(it->toObject(), prefix.isEmpty() && key == "values" ? QString() : name, rows);
            }
        } else if (it->isBool()) {
            rows.push_back({name, it->toBool() ? QObject::tr("ja") : QObject::tr("nein")});
        }
    }
}

void fillTable(QTableWidget* t, const QVector<QPair<QString, QString>>& rows) {
    t->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        auto* name = new QTableWidgetItem(rows[r].first);
        auto* value = new QTableWidgetItem(rows[r].second);
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        value->setFlags(value->flags() & ~Qt::ItemIsEditable);
        t->setItem(r, 0, name);
        t->setItem(r, 1, value);
    }
}

QTableWidget* makeTable(QWidget* parent) {
    auto* t = new QTableWidget(0, 2, parent);
    t->setHorizontalHeaderLabels({QObject::tr("Größe"), QObject::tr("Wert")});
    t->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    t->verticalHeader()->hide();
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->setShowGrid(false);
    return t;
}

// A short line in the colour of a quantity, as icon of its list entry.
QIcon swatch(const QColor& color, int dash) {
    QPixmap pm(QSize(22, 12) * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    Qt::PenStyle style = dash == 1 ? Qt::DashLine : dash == 2 ? Qt::DotLine : dash == 3 ? Qt::DashDotLine : Qt::SolidLine;
    p.setPen(QPen(color, 2.0, style, Qt::RoundCap));
    p.drawLine(QPointF(2, 6), QPointF(20, 6));
    return QIcon(pm);
}

}  // namespace

ResultsPanel::ResultsPanel(QWidget* parent) : QWidget(parent) {
    analyses_ = new QListWidget;
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Größen filtern"));
    filter_->setClearButtonEnabled(true);
    quantities_ = new QListWidget;
    quantities_->setUniformItemSizes(true);
    values_ = makeTable(this);
    plot_ = new PlotWidget;
    bigValues_ = makeTable(this);
    stack_ = new QStackedWidget;
    stack_->addWidget(plot_);
    stack_->addWidget(bigValues_);

    auto* quantityBox = new QWidget;
    auto* ql = new QVBoxLayout(quantityBox);
    ql->setContentsMargins(0, 0, 0, 0);
    ql->setSpacing(4);
    ql->addWidget(filter_);
    ql->addWidget(quantities_);

    auto* left = new QSplitter(Qt::Vertical);
    left->addWidget(analyses_);
    left->addWidget(quantityBox);
    left->addWidget(values_);
    left->setStretchFactor(0, 1);
    left->setStretchFactor(1, 3);
    left->setStretchFactor(2, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(stack_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 800});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(split);

    connect(analyses_, &QListWidget::currentRowChanged, this, &ResultsPanel::showAnalysis);
    connect(quantities_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        if (filling_) return;
        QStringList on;
        for (int k = 0; k < quantities_->count(); ++k)
            if (quantities_->item(k)->checkState() == Qt::Checked) on << quantities_->item(k)->text();
        checked_[currentKey_] = on;
        updatePlot();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int k = 0; k < quantities_->count(); ++k)
            quantities_->item(k)->setHidden(!quantities_->item(k)->text().contains(text, Qt::CaseInsensitive));
    });
    clear();
}

void ResultsPanel::clear() {
    entries_ = {};
    analyses_->clear();
    quantities_->clear();
    values_->setRowCount(0);
    bigValues_->setRowCount(0);
    plot_->clear();
    stack_->setCurrentWidget(plot_);
}

bool ResultsPanel::load(const QString& summaryPath, QString& error) {
    QFile f(summaryPath);
    if (!f.open(QIODevice::ReadOnly)) {
        error = tr("Die Zusammenfassung %1 fehlt").arg(summaryPath);
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) {
        error = tr("Die Zusammenfassung ist kein gültiges JSON: %1").arg(pe.errorString());
        return false;
    }
    const QString previous = currentKey_;
    entries_ = doc.object().value("analyses").toArray();
    analyses_->blockSignals(true);
    analyses_->clear();
    int select = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        const QJsonObject e = entries_[i].toObject();
        const QString type = e.value("type").toString(), name = e.value("name").toString();
        QString label = typeName(type);
        if (!name.isEmpty() && name != type) label += QString::fromUtf8(" „%1“").arg(name);
        if (e.contains("sweep")) label += tr(" (Sweep)");
        const auto add = [&](const QString& text, const QString& file, const QString& kind) {
            auto* item = new QListWidgetItem(text, analyses_);
            item->setData(EntryRole, i);
            item->setData(FileRole, file);
            item->setData(KeyRole, type + "/" + name + "/" + kind);
            if (item->data(KeyRole).toString() == previous) select = analyses_->count() - 1;
        };
        const QString file = e.value("file").toString();
        const QString measures = e.value("measure_file").toString();
        if (!file.isEmpty() || measures.isEmpty()) add(label, file, "curves");
        if (!measures.isEmpty()) add(label + tr(" – Messgrößen"), measures, "measures");
        const QString fourier = e.value("fourier").toObject().value("file").toString();
        if (!fourier.isEmpty()) add(label + tr(" – Fourier"), fourier, "fourier");
    }
    analyses_->blockSignals(false);
    if (select < 0) {
        // the first analysis with curves, else the first one
        for (int k = 0; k < analyses_->count() && select < 0; ++k)
            if (!analyses_->item(k)->data(FileRole).toString().isEmpty()) select = k;
        if (select < 0 && analyses_->count() > 0) select = 0;
    }
    analyses_->setCurrentRow(select);
    if (select >= 0) showAnalysis(select);
    return true;
}

void ResultsPanel::showAnalysis(int row) {
    if (row < 0 || row >= analyses_->count()) return;
    const QListWidgetItem* item = analyses_->item(row);
    currentKey_ = item->data(KeyRole).toString();
    const QJsonObject entry = entries_[item->data(EntryRole).toInt()].toObject();
    fillValues(entry);

    const QString file = item->data(FileRole).toString();
    filling_ = true;
    quantities_->clear();
    table_ = {};
    if (file.isEmpty()) {
        filling_ = false;
        // values only (operating point and the like): the large table
        QVector<QPair<QString, QString>> rows;
        flatten(entry, QString(), rows);
        fillTable(bigValues_, rows);
        stack_->setCurrentWidget(bigValues_);
        return;
    }
    stack_->setCurrentWidget(plot_);
    QString error;
    if (!readCsv(file, table_, error)) {
        filling_ = false;
        plot_->clear();
        return;
    }
    const QStringList names = quantities(table_);
    // a sweep: the colour stands for the run, the line style for the quantity
    const bool sweep = table_.header.size() > 2 && (table_.header[1] == "time" || table_.header[1] == "freq");
    // ticked before: keep; new analysis: the first eight
    const bool known = checked_.contains(currentKey_);
    const QStringList before = checked_.value(currentKey_);
    QStringList on;
    for (int k = 0; k < names.size(); ++k) {
        auto* q = new QListWidgetItem(names[k], quantities_);
        q->setFlags(q->flags() | Qt::ItemIsUserCheckable);
        const bool tick = known ? before.contains(names[k]) : k < Theme::kSeries;
        q->setCheckState(tick ? Qt::Checked : Qt::Unchecked);
        q->setIcon(sweep ? swatch(Theme::textSecondary(), k % 4)
                         : swatch(Theme::series(k % Theme::kSeries), (k / Theme::kSeries) % 4));
        q->setHidden(!filter_->text().isEmpty() && !names[k].contains(filter_->text(), Qt::CaseInsensitive));
        if (tick) on << names[k];
    }
    checked_[currentKey_] = on;
    filling_ = false;
    updatePlot();
}

void ResultsPanel::updatePlot() {
    if (table_.columns.isEmpty()) return;
    const QStringList on = checked_.value(currentKey_);
    if (on.isEmpty()) {
        plot_->clear();
        return;
    }
    plot_->setData(plotData(table_, on));
}

void ResultsPanel::fillValues(const QJsonObject& entry) {
    QVector<QPair<QString, QString>> rows;
    flatten(entry, QString(), rows);
    fillTable(values_, rows);
    values_->setVisible(!rows.isEmpty());
}
