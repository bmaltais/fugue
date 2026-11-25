#pragma once

// Dynamic prompts (spec 14.1, Dynamic Prompt): the template language of adieyal's `dynamicprompts`
// (A1111's sd-dynamic-prompts) in native code: {a|b} variants with weights, counts and separators,
// __wildcard__ files (txt, yaml, json, globs, parameters), ${variables}, %{wrap ... $$inner} and the
// ~ ! @ sampler marks. Templates come from shared workflows, so they are untrusted: wildcard names
// only reach files inside the wildcard folders, and every kind of growth has a cap that ends in a
// clear error (limits below).
//
// Same template + seed + wildcard files give the same text on every machine (a standard
// std::mt19937_64 and our own draws; Python's random sequence is not reproduced).

#include <QString>
#include <QStringList>

#include "engine/result.h"

namespace fugue::dynprompt {

struct Limits {
    qint64 maxFileBytes = 4 * 1024 * 1024; // one wildcard file
    int maxDepth = 32;                     // nested braces, and wildcard/variable expansions inside each other
    int maxOutput = 64 * 1024;             // characters of one prompt
    int maxFiles = 5000;                   // wildcard files looked at in one run
    qint64 maxSteps = 200000;              // template parts evaluated for one prompt (a loop that adds nothing)
};

struct Options {
    QStringList folders; // the wildcard folders, in order: the first that has a name wins
    Limits limits;
};

// The folders wildcards are read from, in order: the session's own `wildcards/` in the data folder,
// the Default's (a project that inherits `prompts`, spec 1.4.2), then every pack's `wildcards/`.
QStringList wildcardFolders();

// A text that changes when a wildcard file is added, removed or edited under `folders`
// (path, size, modification time): the Dynamic Prompt node's part of the cache key.
QString wildcardSignature(const QStringList &folders);

// One random prompt. Errors are `template_invalid` on the `template` input and name the place.
Result<QString> generate(const QString &templ, quint64 seed, const Options &options);

struct Expansion {
    QStringList prompts;
    bool capped = false; // there were more than `cap`
};

// Every combination of the template's variants and wildcards, in order (the last choice changes
// fastest), at most `cap`. `{2$$a|b|c}` gives its combinations (a, b), (a, c), (b, c); `~` parts
// stay random and `@` parts cycle, with `seed` as the start.
Result<Expansion> expandAll(const QString &templ, const Options &options, int cap, quint64 seed = 0);

// What can be told without running: the syntax, wildcard names that cannot be allowed, wildcards
// that no file has, variables that are never set. Each finding is a template_invalid Error; the
// scheduler shows every NodeKernel::check finding under validate as a warning on the node (as for
// Text Template), and the run itself fails with the same error.
Errors check(const QString &templ, const Options &options);

} // namespace fugue::dynprompt
