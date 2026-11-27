#pragma once

// The model families (#369, #370): the one table that names them, so the templates window's
// Model family filter and the model pickers' family filter say the same names. A family is read
// from a model's resource type and file name, never typed by hand: "flux-2-klein-9b-Q8_0.gguf" of
// a diffusion model is "FLUX.2 Klein 9B". A model that is no family's (a VAE, a text encoder, a
// file nobody listed) has none: the pickers file it under "Other", a template just shows no family
// for it.

#include <QJsonArray>
#include <QString>
#include <QStringList>

namespace fugue {

// The family of the model file `name` of resource type `resource` ("fugue/diffusion-model" or the
// bare "diffusion-model"), or an empty string when it is none's.
QString modelFamily(const QString &resource, const QString &name);

// The families of a workflow's models (Document::models(): {resource, hash, name, ...} handles):
// each once, in the order the models come in.
QStringList modelFamilies(const QJsonArray &models);

// The picker family for the engine's own version name (#703: "Flux", "SDXL Inpaint", "Flux.2 klein", what `fugue-helper --inspect`
// answers as `version`), in this table's style, so a file the table does not name lands in the same group as one it does where
// both are one family ("Flux", "Flux Fill" and "Flux Control" are "FLUX.1"; the three Qwen Image versions are "Qwen-Image").
// An inpaint, pix2pix or tiny variant is its base family's. A name with no entry is returned as the engine wrote it.
QString engineFamilyName(const QString &engineName);

// Every family name the table knows, in its order: what a filter may offer before any model is seen.
QStringList knownModelFamilies();

} // namespace fugue
