#include "gui_resources.hpp"

#include <QResource>

void initialize_mint_gui_qt_resources() {
    Q_INIT_RESOURCE(mint_gui);
}

namespace mint::ui {

void initialize_gui_resources() {
    initialize_mint_gui_qt_resources();
}

} // namespace mint::ui
