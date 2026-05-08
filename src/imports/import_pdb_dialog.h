#pragma once

#include "widgets/themed_dialog.h"
#include "widgets/dialog_button.h"
#include <QVector>
#include <cstdint>

class QLineEdit;
class QCheckBox;
class QListWidget;
class QLabel;
class QPushButton;

namespace rcx {

struct PdbTypeInfo;

class PdbImportDialog : public ThemedDialog {
    Q_OBJECT
public:
    explicit PdbImportDialog(QWidget* parent = nullptr);

    QString pdbPath() const;
    QVector<uint32_t> selectedTypeIndices() const;

private slots:
    void browsePdb();
    void loadPdb();
    void filterChanged(const QString& text);
    void selectAllToggled(bool checked);
    void updateSelectionCount();

private:
    QLineEdit*    m_pathEdit;
    QPushButton*  m_browseBtn;
    QLineEdit*    m_filterEdit;
    QCheckBox*    m_selectAll;
    QListWidget*  m_typeList;
    QLabel*       m_countLabel;
    DialogButton* m_importBtn;

    struct TypeItem {
        uint32_t typeIndex;
        QString  name;
        int      childCount;
        bool     isUnion;
    };
    QVector<TypeItem> m_allTypes;

    void populateList();
};

} // namespace rcx
