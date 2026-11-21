#pragma once

// The media a workflow made (#133), as a list a viewer can show: plain C++ with no QML and no
// canvas, so it is tested alone and any host (the workflows panel, a node's face, the large view)
// can feed the same MediaBrowser with it.
//
//   mediaOfRun        the images, videos and sounds one run folder (spec 9.7) holds as results: the
//                     outputs of the nodes nothing else reads (Save Image, Preview, Compare...),
//                     not the loaded inputs or the steps in between. A node that was cached counts
//                     as well: a run that changed nothing still shows what it showed.
//   mediaOfWorkflow   the same over the newest runs of one workflow file, newest first; a file
//                     that came back in a later run stays with the newest, and one that is gone
//                     from the disk is left out.
//   groupByRun        the items in groups of one run each, in the order given.
//   embeddedSummary   what a PNG or WebP file's embedded workflow says (11.7): the prompt, the
//                     seed and the model that made it.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace fugue {

struct MediaItem {
    QString path;     // the file
    QString name;     // its file name
    QString kind;     // "image", "video" or "audio"
    QString run;      // the run id, or "" for a file that came from elsewhere
    QString node;     // the path id of the node that made it
    QString workflow; // the workflow file the run started from, or ""
    QDateTime time;   // when the run started (UTC)
};

struct MediaGroup {
    QString run;
    QDateTime time;
    QList<MediaItem> items;
};

// When a run id ("20261121-174052-080-k3f9") says the run started; invalid for any other text.
QDateTime runStartOf(const QString &runId);

QList<MediaItem> mediaOfRun(const QString &runsRoot, const QString &runId);
QList<MediaItem> mediaOfWorkflow(const QString &runsRoot, const QString &workflow, int maxRuns = 40);
QList<MediaGroup> groupByRun(const QList<MediaItem> &items);

// {"embedded": bool, "prompt", "seed", "model" (each only when found), "nodes": count}. The prompt is
// the first Text node's text, the seed and model the first the run section records. A file that is
// no PNG or WebP, or holds no workflow, gives {"embedded": false}.
QJsonObject embeddedSummary(const QString &path);

} // namespace fugue
