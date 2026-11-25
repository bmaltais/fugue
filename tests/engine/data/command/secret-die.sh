k=$(cat | sed 's/.*"value":"\([^"]*\)".*/\1/')
echo "fatal: $k" >&2
exit 3
