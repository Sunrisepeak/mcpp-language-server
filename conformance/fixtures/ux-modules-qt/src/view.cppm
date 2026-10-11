module;
#include <QtWidgets/QtWidgets>
export module views.view;
import views.model;

export namespace views {

class InventoryWindow : public QMainWindow {
public:
    explicit InventoryWindow(Inventory* inventory) {
        auto* list = new QListView(this);
        list->setModel(inventory);
        setCentralWidget(list);
        setWindowTitle(QStringLiteral("Inventory"));
    }
    void showCount(int count) { statusBar()->showMessage(QString::number(count)); }
};

} // namespace views
