# Persistent mode

Use it when starting the program is slow, for example when it loads a model. Set
`"runtime": {"kind": "command", "entry": "./run.sh", "mode": "persistent"}`.

The program is started once and kept. It writes `{"op": "ready"}` when it can take work. Fugue then
sends `run` messages, the same shape as in [one-shot mode](protocol.md), one per line, one at a
time. Your answers (`progress`, `preview`, `result`, `error`) must carry the request's `id`.

```bash
printf '{"op": "ready"}\n'
while IFS= read -r line; do
  case $(jq -r '.op' <<<"$line") in
    run)      # answer with {"op":"result","id":…,"outputs":{…}}
      ;;
    cancel)   # stop that run, answer {"op":"error","id":…,"code":"cancelled","message":"…"}
      ;;
    shutdown) exit 0 ;;
  esac
done
```

The example pack's `counter` node is a complete one.

## Messages from Fugue

| Message | Meaning |
| --- | --- |
| `run` | Do the work. |
| `cancel` with the run's `id` | Stop it. Answer with an `error` whose `code` is `cancelled`, or with the `result` if it finished first. No answer within 5 seconds and the process is killed. |
| `shutdown` | Exit now. Sent after the idle timeout (`idle_timeout_s`, default 300) or when memory is needed, then SIGTERM, then SIGKILL. |

## Things that bite

- **The working directory and `FUGUE_WORK_DIR` are those of the first run.** Later runs have other
  folders. Write each run's files under the `work_dir` of **its own request**, never under the
  environment's.
- One run at a time per process.
- If the process dies the running node fails with `node_failed`; the next run starts a new process.
- A node that is not a pure function of its inputs (a counter, a clock) should say
  `"deterministic": false`, or Fugue returns a cached result without calling it.
- A pack's `node.json` and entry script are sealed when loaded: editing them while Fugue is open
  stops the node until Fugue reads the pack again; a persistent program is then restarted.
- If the type declares `needs.gpu`, the running program counts as a resident model
  ([resources](resources.md)): Fugue may shut it down between runs to make room, and your program
  must expect to be started again.
