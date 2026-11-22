#pragma once

// A node type with one input of every widget of spec 6.2 and a model input, for the widget
// tests. No built-in node has a toggle or a color input.

#include <QJsonDocument>
#include <QJsonObject>

#include "engine/model_offers.h"
#include "engine/registry.h"

namespace widget_test {

inline const char *manifest = R"({
  "manifest": 1, "id": "io.example.widgets/all", "version": "1.0.0", "name": "All widgets",
  "runtime": {"kind": "command", "entry": "./all.sh"},
  "inputs": [
    {"name": "count", "type": "number", "label": "Count", "default": 4, "min": 0, "max": 100, "step": 1, "integer": true},
    {"name": "amount", "type": "number", "label": "Amount", "default": 0.5, "min": 0, "max": 1, "step": 0.05, "widget": "slider"},
    {"name": "width", "type": "number", "label": "Width", "optional": true, "integer": true, "min": 64, "max": 8192, "step": 64},
    {"name": "name", "type": "text", "label": "Name", "default": "fugue"},
    {"name": "notes", "type": "text", "label": "Notes", "default": "", "widget": "multiline"},
    {"name": "flag", "type": "boolean", "label": "Flag", "default": false},
    {"name": "mode", "type": "text", "label": "Mode", "default": "a", "choices": ["a", "b", "c"]},
    {"name": "file", "type": "text", "label": "File", "optional": true, "widget": "file"},
    {"name": "folder", "type": "text", "label": "Folder", "optional": true, "widget": "folder"},
    {"name": "tint", "type": "text", "label": "Tint", "default": "#ff8800", "widget": "color"},
    {"name": "seed", "type": "number", "label": "Seed", "default": 0, "integer": true, "min": 0, "max": 9007199254740991, "widget": "seed"},
    {"name": "model", "type": "fugue/image-model", "label": "Model", "optional": true},
    {"name": "image", "type": "image", "label": "Image", "optional": true}
  ],
  "outputs": [{"name": "image", "type": "image", "label": "Image"}]
})";

// Call after the built-in node types are in (they declare fugue/image-model).
inline void addWidgetType(fugue::Registry &registry) {
    const fugue::Errors errors = registry.addManifest(QJsonDocument::fromJson(manifest).object());
    if (!errors.isEmpty())
        qFatal("widget fixture: %s", qPrintable(errors.first().message));
}

// A model a provider offers, as the model picker's source lists it.
inline fugue::ListedModel served(const QString &name, const QJsonObject &handle, const QString &status,
                                 const QString &provider, const QString &title = {}, const QString &note = {},
                                 const QString &host = {}) {
    fugue::ListedModel model;
    model.name = name;
    model.handle = handle;
    model.status = status;
    model.provider = provider;
    model.title = title;
    model.note = note;
    model.host = host;
    return model;
}

} // namespace widget_test
