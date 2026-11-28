#pragma once

// Parameter sweep (#552, spec 13.1 `fugue run --sweep`): one workflow run once per value of one or two inputs, the pictures laid
// out as one labelled sheet. Two small pieces, so the command line and a later window menu share them:
//   - planSweep(): the workflow with each cell's values bound, plus the labels, or a refusal before anything runs;
//   - composeDocument(): the workflow of the sheet itself (Load Image per cell picture, Image Grid, Save Image), which the caller
//     runs like any other.
// A cell is an ordinary run of a bound copy; the document is never changed (the cache reuses the work cells share). The first
// axis runs left to right (the columns), the second top to bottom (the rows).

#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include "engine/document.h"
#include "engine/result.h"

namespace fugue {

class Registry;

// `--set` and `--sweep` read a value the same way: JSON when it parses as one (a number, true, "text"), else plain text.
QJsonValue parseSetValue(const QString &raw);

// Every `path` of a media handle in a node.done event's `outputs` or `shown`, in order: the files a node made.
void collectMediaPaths(const QJsonValue &value, QStringList &paths);

struct SweepAxis {
    QString node;
    QString input;
    QList<QJsonValue> values;
    QStringList texts; // each value as written, for labels
};

// `<node>.<input>=v1,v2,...`: the values are split at commas outside quotes, brackets and braces, each read by parseSetValue.
// `usage` for a form that is not that, or an empty value.
Result<SweepAxis> parseSweepAxis(const QString &text);

// The most cells a sweep makes (the Image Grid's own limit).
inline constexpr int kMaxSweepCells = 64;

struct SweepLabels {
    int columns = 1;
    QStringList cells;   // one per cell, row by row: what is written under the picture
    QStringList columnLabels; // two axes: the first axis's values
    QStringList rowLabels;    // two axes: the second axis's values
};

struct SweepCell {
    Document bound; // the workflow with this cell's values in
    QString label;  // the cell in words, "steps=20" or "steps=20, cfg=7"
};

struct SweepPlan {
    QList<SweepCell> cells; // row by row
    SweepLabels labels;
};

// One or two axes over `document`, run up to the node `to`. Refused with nothing run: `usage` (no or over two axes, an axis twice,
// a node the workflow does not have: `valid` lists them), `unknown_node_type`, `unknown_input`, `type_mismatch` (a list input, or
// one that holds a picture, is not an axis), `value_out_of_range` or `value_not_in_choices` (a value the input cannot take: the
// compiler's own checks, one error per value), and `value_out_of_range` for over kMaxSweepCells cells.
Result<SweepPlan> planSweep(const Document &document, const QList<SweepAxis> &axes, const QString &to, const Registry &registry);

// The sheet's workflow: `cellFiles` (one picture file per cell, row by row) into an Image Grid laid out and labelled by `labels`,
// then Save Image into `folder` (empty: the output folder). `unknown_node_type` when `registry` lacks one of those three.
Result<Document> composeDocument(const QStringList &cellFiles, const SweepLabels &labels, const Registry &registry,
                                 const QString &folder = {});

} // namespace fugue
