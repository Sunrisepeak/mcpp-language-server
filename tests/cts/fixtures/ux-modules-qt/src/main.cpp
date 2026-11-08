#include <QtWidgets/QApplication>
#include <QtCore/QTimer>
import views.model;
import views.view;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    views::Inventory inventory;
    inventory.addItem(QStringLiteral("bolt"));
    views::InventoryWindow window(&inventory);
    window.showCount(inventory.rowCount());
    QTimer::singleShot(0, &app, &QApplication::quit);
    return app.exec();
}
