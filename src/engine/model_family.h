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

// Every family name the table knows, in its order: what a filter may offer before any model is seen.
QStringList knownModelFamilies();

} // namespace fugue
