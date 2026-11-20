#include "node_type_reloads.h"

namespace fugue {

void followNodeTypeReloads(RegistryReloader &reloader, const AppObjects &objects) {
    QObject::connect(&reloader, &RegistryReloader::reloaded, &reloader, [objects] {
        if (objects.canvasModel)
            objects.canvasModel->refreshNodeTypes();
        if (objects.workflows)
            objects.workflows->refreshNodeTypes();
        if (objects.runs)
            objects.runs->refreshNodeTypes();
        if (objects.settings)
            objects.settings->packs()->refresh();
    });
    // Queued: the save's own call (a menu, a button) is over before every row is made again.
    if (objects.canvasModel)
        QObject::connect(objects.canvasModel, &CanvasModel::librarySaved, &reloader, &RegistryReloader::reload,
                         Qt::QueuedConnection);
    if (objects.settings) {
        QObject::connect(objects.settings->packs(), &PacksModel::packsModified, &reloader, &RegistryReloader::reload);
        QObject::connect(objects.settings, &SettingsModel::changed, &reloader, &RegistryReloader::reloadSoon);
    }
}

} // namespace fugue
