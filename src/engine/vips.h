#pragma once

// libvips set up the way spike 03 decided (spec 9.2): concurrency 1, because
// the engine's executor already runs CPU nodes in parallel, and an operation
// cache of 0, because the engine has its own result cache.
//
// Every engine function that touches libvips calls initVips() itself; call it
// early anyway to learn whether libvips works. A lone CPU node may raise its
// own concurrency later (the scheduler's choice, not measured yet).

#include <QString>

#include <vips/vips8>

#include "engine/result.h"

namespace fugue {

// Starts libvips once per process; later calls only return the first result.
bool initVips();

// The error when initVips() failed (internal).
Error vipsUnavailable();

// The libvips loader that would open `path` (sniffed from its content), when it is one libvips counts as safe for files from
// anywhere; else why not: no loader, or an untrusted one (magick, svg, pdf, dcraw, openexr, jxl, jp2k, fits, mat, openslide,
// ppm, rad, csv, raw, vips, analyze; VIPS_OPERATION_UNTRUSTED). For what a pack's sandboxed program wrote (6.7.1): the block is
// per file, so the person's own RAW, EXR or SVG files still open.
QString untrustedLoaderProblem(const QString &path);

// A libvips failure as an error, with libvips' own message. The code follows one rule:
// media_unreadable when libvips was reading an input's pixels (a file it cannot decode),
// internal for anything else (making, converting or writing an image).
Error vipsError(const vips::VError &error, const QString &code = QStringLiteral("internal"));

} // namespace fugue
