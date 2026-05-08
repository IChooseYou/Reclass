#include "import_pdb_dialog.h"
#include "import_pdb.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QApplication>

namespace rcx {

PdbImportDialog::PdbImportDialog(QWidget* parent)
    : ThemedDialog(parent)
{
    setWindowTitle(QStringLiteral("Import from PDB"));
    resize(520, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    // PDB path row
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(QStringLiteral("PDB file:")));
    m_pathEdit = new QLineEdit;
    m_pathEdit->setPlaceholderText(QStringLiteral("Select a PDB file..."));
    pathRow->addWidget(m_pathEdit);
    m_browseBtn = new DialogButton(QStringLiteral("..."),
        DialogButton::Secondary, this);
    m_browseBtn->setFixedWidth(40);
    pathRow->addWidget(m_browseBtn);
    layout->addLayout(pathRow);

    // Filter row
    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(QStringLiteral("Filter:")));
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(QStringLiteral("Type name filter..."));
    m_filterEdit->setEnabled(false);
    filterRow->addWidget(m_filterEdit);
    layout->addLayout(filterRow);

    // Select all checkbox
    m_selectAll = new QCheckBox(QStringLiteral("Select all"));
    m_selectAll->setEnabled(false);
    layout->addWidget(m_selectAll);

    // Type list
    m_typeList = new QListWidget;
    m_typeList->setEnabled(false);
    layout->addWidget(m_typeList);

    // Count label
    m_countLabel = new QLabel(QStringLiteral("No PDB loaded."));
    layout->addWidget(m_countLabel);

    // Buttons
    auto* cancelBtn = new DialogButton(QStringLiteral("Cancel"),
        DialogButton::Secondary, this);
    m_importBtn = new DialogButton(QStringLiteral("Import"),
        DialogButton::Primary, this);
    m_importBtn->setEnabled(false);
    layout->addLayout(makeButtonRow({ cancelBtn, m_importBtn }));

    connect(m_browseBtn,  &QPushButton::clicked, this, &PdbImportDialog::browsePdb);
    connect(m_pathEdit,   &QLineEdit::returnPressed, this, &PdbImportDialog::loadPdb);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &PdbImportDialog::filterChanged);
    connect(m_selectAll,  &QCheckBox::toggled, this, &PdbImportDialog::selectAllToggled);
    connect(m_typeList,   &QListWidget::itemChanged, this, &PdbImportDialog::updateSelectionCount);
    connect(m_importBtn,  &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn,    &QPushButton::clicked, this, &QDialog::reject);
}

QString PdbImportDialog::pdbPath() const {
    return m_pathEdit->text();
}

QVector<uint32_t> PdbImportDialog::selectedTypeIndices() const {
    QVector<uint32_t> result;
    for (int i = 0; i < m_typeList->count(); i++) {
        auto* item = m_typeList->item(i);
        if (item->checkState() == Qt::Checked) {
            uint32_t typeIndex = item->data(Qt::UserRole).toUInt();
            result.append(typeIndex);
        }
    }
    return result;
}

void PdbImportDialog::browsePdb() {
    QString path = QFileDialog::getOpenFileName(this,
        "Select PDB File", {},
        "PDB Files (*.pdb);;All Files (*)");
    if (path.isEmpty()) return;
    m_pathEdit->setText(path);
    loadPdb();
}

void PdbImportDialog::loadPdb() {
    QString path = m_pathEdit->text();
    if (path.isEmpty()) return;

    m_typeList->clear();
    m_allTypes.clear();
    m_countLabel->setText(QStringLiteral("Loading..."));
    m_typeList->setEnabled(false);
    m_filterEdit->setEnabled(false);
    m_selectAll->setEnabled(false);
    m_importBtn->setEnabled(false);
    QApplication::processEvents();

    QString error;
    QVector<PdbTypeInfo> types = enumeratePdbTypes(path, &error);

    if (types.isEmpty()) {
        m_countLabel->setText(error.isEmpty()
            ? QStringLiteral("No types found in this PDB.")
            : error);
        return;
    }

    m_allTypes.reserve(types.size());
    for (const auto& t : types) {
        TypeItem item;
        item.typeIndex = t.typeIndex;
        item.name = t.name;
        item.childCount = t.childCount;
        item.isUnion = t.isUnion;
        m_allTypes.append(item);
    }

    // Sort by name
    std::sort(m_allTypes.begin(), m_allTypes.end(),
              [](const TypeItem& a, const TypeItem& b) { return a.name < b.name; });

    m_filterEdit->setEnabled(true);
    m_selectAll->setEnabled(true);
    m_typeList->setEnabled(true);
    populateList();
}

void PdbImportDialog::populateList() {
    m_typeList->blockSignals(true);
    m_typeList->clear();

    QString filter = m_filterEdit->text();
    bool selectAll = m_selectAll->isChecked();

    for (const auto& t : m_allTypes) {
        if (!filter.isEmpty() && !t.name.contains(filter, Qt::CaseInsensitive))
            continue;

        QString label = QStringLiteral("%1  (%2 fields)")
            .arg(t.name).arg(t.childCount);
        auto* item = new QListWidgetItem(label, m_typeList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(selectAll ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, t.typeIndex);
    }

    m_typeList->blockSignals(false);
    updateSelectionCount();
}

void PdbImportDialog::filterChanged(const QString&) {
    populateList();
}

void PdbImportDialog::selectAllToggled(bool) {
    populateList();
}

void PdbImportDialog::updateSelectionCount() {
    int checked = 0;
    int total = m_typeList->count();
    for (int i = 0; i < total; i++) {
        if (m_typeList->item(i)->checkState() == Qt::Checked)
            checked++;
    }
    m_countLabel->setText(QStringLiteral("%1 of %2 types selected.")
        .arg(checked).arg(m_allTypes.size()));
    m_importBtn->setEnabled(checked > 0);
}

} // namespace rcx
