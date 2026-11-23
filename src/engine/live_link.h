#pragma once

// The canvas live link's decisions (#315, docs/canvas-actions-design.md 16): when to re-run the chosen canvas action as a preview
// because the canvas, the selection or the prompt changed. No Qt GUI, no timer, no clock of its own: the caller says what the
// state is (note) and what time it is (every call takes `now`, in milliseconds), and asks what to do (poll), so a test drives it with
// a fake clock and the window only wires it to a QTimer and RunService.
//
// The rules:
//  - Debounce: a run starts only after the state has been still for `quiet()`. That is the base (600 ms), or half the last run's time when
//    that is longer (a slow model waits longer), at most `ceiling`.
//  - One run at a time (one queue): a change that makes the run in flight stale cancels it at once; the next starts after the
//    quiet time and only once the cancelled one has ended (ended()).
//  - A state whose preview is already shown, or whose run is in flight, is never run again.
//  - Not ready (no selection, a blocker, another canvas busy): nothing starts and a run in flight is cancelled.
//  - A failed run switches the link off: a broken action must not retry on every change.

#include <QString>

namespace fugue {

class LiveLink {
public:
    enum class Do { Nothing, Start, Cancel };
    struct Step {
        Do what = Do::Nothing;
        qint64 waitMs = -1; // Nothing: poll again after this long (-1: only after the next note or ended)
    };
    enum class End { Ok, Cancelled, Failed };

    static constexpr qint64 defaultBase = 600;
    static constexpr qint64 ceiling = 5000;
    // The shortest quiet time (tests make it short).
    void setBase(qint64 ms) { m_base = ms; }

    bool enabled() const { return m_enabled; }
    // On: the current state is run once it has been still (a start is a change). Off: the shown preview is forgotten.
    void setEnabled(bool on, qint64 now);

    // The state as it is now: `key` names everything a run reads (canvas, head, selection, prompt, action, seed, ...), `ready`
    // says a run may start. Calling it again with the same key changes nothing, so it is safe on every signal.
    void note(const QString &key, bool ready, qint64 now);
    // The state was changed by the person's own keep of the preview (the layer landed): take it as shown, run nothing until the next change.
    void adopt(const QString &key);
    // What to do now. Start marks a run in flight: the caller starts it, and calls ended() when it ends (or fails to start).
    Step poll(qint64 now);
    void ended(End how, qint64 now);

    bool running() const { return m_running; }
    qint64 quiet() const;

private:
    bool m_enabled = false;
    bool m_ready = false;
    bool m_running = false;
    bool m_cancelling = false;
    QString m_key, m_runKey, m_shownKey;
    qint64 m_base = defaultBase;
    qint64 m_changedAt = 0, m_startedAt = 0, m_lastRun = 0;
};

} // namespace fugue
