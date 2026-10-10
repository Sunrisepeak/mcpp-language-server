module;
#include <QtCore/QAbstractListModel>
#include <QtCore/QStringList>
export module views.model;

export namespace views {

class Inventory : public QAbstractListModel {
public:
    explicit Inventory(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : items_.size(); }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole) return {};
        return items_.at(index.row());
    }
    void addItem(const QString& item) {
        beginInsertRows({}, items_.size(), items_.size());
        items_.append(item);
        endInsertRows();
    }
    QStringList itemNames() const { return items_; }

private:
    QStringList items_;
};

} // namespace views
