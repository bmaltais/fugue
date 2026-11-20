# A persistent node that takes `run` lines. mode "slow" waits for a cancel message, "deaf"
# ignores every message, "die" exits, "preview" shows step.png in the run's work_dir (and sends
# a preview for another run, which must not show), anything else answers at once. It records
# its pid in pids.txt (in the node's folder) and counts the runs.
echo $$ >> "$FUGUE_NODE_DIR/pids.txt"
echo '{"op": "ready"}'
n=0
while read -r line; do
  case "$line" in
    *'"op":"shutdown"'*) exit 0 ;;
    *'"op":"run"'*)
      n=$((n+1))
      id=$(echo "$line" | sed 's/.*"id":"\([^"]*\)".*/\1/')
      case "$line" in
        *'"mode":"die"'*) exit 3 ;;
        *'"mode":"preview"'*)
          work=$(echo "$line" | sed 's/.*"work_dir":"\([^"]*\)".*/\1/')
          echo "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==" | base64 -d > "$work/step.png"
          echo "{\"op\":\"preview\",\"id\":\"other\",\"image\":{\"path\":\"$work/step.png\"}}"
          echo "{\"op\":\"preview\",\"id\":\"$id\",\"image\":{\"path\":\"$work/step.png\"}}" ;;
        *'"mode":"deaf"'*) touch "$FUGUE_NODE_DIR/slow.started"; sleep 60 ;;
        *'"mode":"slow"'*)
          echo "{\"op\":\"progress\",\"id\":\"$id\",\"value\":0.5,\"message\":\"working\"}"
          touch "$FUGUE_NODE_DIR/slow.started"
          for i in $(seq 1 100); do
            if read -r -t 0.1 msg && [[ "$msg" == *'"op":"cancel"'* ]]; then
              echo "{\"op\":\"error\",\"id\":\"$id\",\"code\":\"cancelled\",\"message\":\"stopped\"}"
              continue 2
            fi
          done ;;
      esac
      echo "{\"op\":\"result\",\"id\":\"$id\",\"outputs\":{\"text\":\"run $n\"}}"
      ;;
  esac
done
