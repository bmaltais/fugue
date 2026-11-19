# Spike 05 canvas prototype (throwaway)

Results and the decision: [docs/spikes/05-canvas.md](../../docs/spikes/05-canvas.md). Not production
code; the real canvas is built in its own ticket.

Run in a real session (offscreen numbers mean nothing):

```
/usr/lib/qt6/bin/qml canvas.qml                          # demo graph, toolbar to add nodes and switch wire modes
/usr/lib/qt6/bin/qml canvas.qml -- --grid 300            # start with 300 stress nodes
# Benchmarks, then quit. Mode 0 one Shape, 1 Shape per wire, 2 one Shape with CurveRenderer.
QT_FORCE_STDERR_LOGGING=1 QSG_RENDER_TIMING=1 /usr/lib/qt6/bin/qml canvas.qml -- --grid 300 --mode 1 --bench 2> timing.log
python3 render-timing.py timing.log
```

`QT_FORCE_STDERR_LOGGING=1` is needed when stderr is not a terminal: Qt otherwise sends
`console.log` (the `RESULT` lines) to the journal.

Use: wheel zooms at the cursor; right or middle drag pans; left drag moves a node; drag from an
output dot to an input dot to connect (invalid targets dim and the reason shows at the bottom).
