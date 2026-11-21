#!/bin/bash
# Smoke test of `fugue-helper --serve-http` (helper/README.md), with curl; needs no model:
# auth, describe, the key file's mode, bad input, a missing model, a path outside the roots.
#
#   helper/smoke-http.sh build-helper-cpu/fugue-helper-cpu
set -euo pipefail

helper="${1:?usage: smoke-http.sh <fugue-helper binary>}"
work="$(mktemp -d)"
pid=
cleanup() { [[ -n $pid ]] && kill "$pid" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
mkdir -p "$work/models/vae"
head -c 1000 /dev/zero > "$work/models/vae/small.safetensors"
echo "secret-key" > "$work/key"
export XDG_STATE_HOME="$work/state" # where the helper keeps its hashes of model files
chmod 600 "$work/key"
port=$((20000 + RANDOM % 20000))
url="http://127.0.0.1:$port/fugue/v1"
failures=0

check() { # name, expected, actual
  if [[ $2 == "$3" ]]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; failures=$((failures + 1)); fi
}
code() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
auth=(-H "Authorization: Bearer secret-key")

chmod 644 "$work/key"
if "$helper" --serve-http --listen 127.0.0.1 --port "$port" --key-file "$work/key" --models "$work/models" 2>/dev/null; then
  check "a world-readable key file is refused" refused started
else
  check "a world-readable key file is refused" refused refused
fi
chmod 600 "$work/key"
if "$helper" --serve-http --listen 127.0.0.1 --port "$port" --models "$work/models" 2>/dev/null; then
  check "no key file is refused" refused started
else
  check "no key file is refused" refused refused
fi

"$helper" --serve-http --listen 127.0.0.1 --port "$port" --key-file "$work/key" --models "$work/models" 2>"$work/log" &
pid=$!
for _ in $(seq 50); do curl -s -o /dev/null "$url/describe" && break; sleep 0.1; done

check "no key: 401" 401 "$(code -X POST -d '{}' "$url/describe")"
check "wrong key: 401" 401 "$(code -X POST -H 'Authorization: Bearer nope' -d '{}' "$url/describe")"
check "the 401 body" unauthorized "$(curl -s -X POST -d '{}' "$url/describe" | python3 -c 'import json,sys; print(json.load(sys.stdin)["error"]["code"])')"
describe="$(curl -s "${auth[@]}" -X POST -d '{}' "$url/describe")"
check "describe" "1 sdcpp False True" "$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["protocol"], d["engine"], d["shares_filesystem"], d["free_memory_bytes"] > 0)' "$describe")"
check "describe capabilities" "text.encode latent.sample latent.decode latent.encode" "$(python3 -c 'import json,sys; print(" ".join(json.loads(sys.argv[1])["capabilities"]))' "$describe")"
check "bad JSON: 400" 400 "$(code "${auth[@]}" -X POST -d '{nope' "$url/describe")"
check "unknown capability: 400" 400 "$(code "${auth[@]}" -X POST -d '{"capability":"x.y"}' "$url/submit")"
miss='{"capability":"latent.decode","params":{"context":{"vae_path":{"name":"vae/absent.safetensors","size":5}}},"inputs":{"latent":{"data":"AAAA","format":"latent"}}}'
check "missing model: 400" 400 "$(code "${auth[@]}" -X POST -d "$miss" "$url/submit")"
check "missing model code" model_missing "$(curl -s "${auth[@]}" -X POST -d "$miss" "$url/submit" | python3 -c 'import json,sys; print(json.load(sys.stdin)["error"]["code"])')"
check "a path outside the roots: 400" 400 "$(code "${auth[@]}" -X POST -d '{"capability":"latent.decode","params":{"context":{"vae_path":"/etc/passwd"}}}' "$url/submit")"
check "a name with ..: 400" 400 "$(code "${auth[@]}" -X POST -d '{"capability":"latent.decode","params":{"context":{"vae_path":{"name":"../key"}}}}' "$url/submit")"
check "unknown job: 404" 404 "$(code "${auth[@]}" -X POST -d '{"job":"99"}' "$url/status")"
check "a result file of no job: 404" 404 "$(code "${auth[@]}" "$url/result/99/0")"
check "shutdown does nothing" "{}" "$(curl -s "${auth[@]}" -X POST -d '{}' "$url/shutdown")"
check "still serving" 200 "$(code "${auth[@]}" -X POST -d '{}' "$url/describe")"

# A handle that is found (the model is not loaded: the job fails, but is accepted).
ok='{"capability":"latent.decode","params":{"context":{"vae_path":{"name":"small.safetensors","size":1000}}},"inputs":{"latent":{"data":"AAAA","format":"latent"}}}'
check "a found handle is accepted" 200 "$(code "${auth[@]}" -X POST -d "$ok" "$url/submit")"

# A handle with a sha256: the file is taken only if its content hashes to it.
good="$(sha256sum "$work/models/vae/small.safetensors" | cut -d' ' -f1)"
bad="$(printf '%064d' 0)"
withsha() { echo "{\"capability\":\"latent.decode\",\"params\":{\"context\":{\"vae_path\":{\"name\":\"small.safetensors\",\"size\":1000,\"sha256\":\"$1\"}}},\"inputs\":{\"latent\":{\"data\":\"AAAA\",\"format\":\"latent\"}}}"; }
job="$(curl -s "${auth[@]}" -X POST -d "$(withsha "sha256:$bad")" "$url/submit" | python3 -c 'import json,sys; print(json.load(sys.stdin)["job"])')"
for _ in $(seq 50); do
  status="$(curl -s "${auth[@]}" -X POST -d "{\"job\":\"$job\"}" "$url/status")"
  [[ $(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["state"])' "$status") == error ]] && break; sleep 0.1
done
check "same name, wrong sha256: model_missing" model_missing "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["error"]["code"])' "$status")"
check "... saying the sha256 differs" yes "$(python3 -c 'import json,sys; print("yes" if "the sha256 differs" in json.loads(sys.argv[1])["error"]["message"] else "no")' "$status")"
check "a malformed sha256: 400" 400 "$(code "${auth[@]}" -X POST -d "$(withsha zz)" "$url/submit")"
check "the right sha256 is accepted (bare hex)" 200 "$(code "${auth[@]}" -X POST -d "$(withsha "$good")" "$url/submit")"
job="$(curl -s "${auth[@]}" -X POST -d "$(withsha "sha256:${good^^}")" "$url/submit" | python3 -c 'import json,sys; print(json.load(sys.stdin)["job"])')"
sleep 0.5
status="$(curl -s "${auth[@]}" -X POST -d "{\"job\":\"$job\"}" "$url/status")"
check "the right sha256 passes the models stage" no "$(python3 -c 'import json,sys; e=json.loads(sys.argv[1]).get("error"); print("yes" if e and e["code"]=="model_missing" else "no")' "$status")"
check "the hash is kept for a restart" "$good" "$(python3 -c 'import json,sys; print(next(iter(json.load(open(sys.argv[1])).values()))["sha256"])' "$work/state/fugue/model-hashes.json")"

((failures == 0)) && echo "all passed" || { echo "$failures failed"; exit 1; }
