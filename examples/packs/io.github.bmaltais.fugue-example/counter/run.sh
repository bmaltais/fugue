#!/bin/bash
# Persistent command node: started once, says ready, then answers one run per line until shutdown.
count=0
printf '{"op": "ready"}\n'
while IFS= read -r line; do
  case $(jq -r '.op' <<<"$line") in
    run)
      id=$(jq -r '.id' <<<"$line")
      count=$((count + $(jq -r '.inputs.step' <<<"$line")))
      jq -cn --arg id "$id" --argjson count "$count" '{op: "result", id: $id, outputs: {count: $count}}'
      ;;
    cancel) jq -cn --arg id "$(jq -r '.id' <<<"$line")" '{op: "error", id: $id, code: "cancelled", message: "cancelled"}' ;;
    shutdown) exit 0 ;;
  esac
done
