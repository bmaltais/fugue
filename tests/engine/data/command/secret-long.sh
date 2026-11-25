k=$(cat | sed 's/.*"value":"\([^"]*\)".*/\1/')
pad=$(printf 'x%.0s' $(seq 1 493))
echo "$pad$k tail" >&2
exit 3
