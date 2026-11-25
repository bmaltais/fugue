k=$(cat | sed 's/.*"value":"\([^"]*\)".*/\1/')
echo "log line with $k" >&2
echo "{\"op\": \"error\", \"code\": \"bad\", \"message\": \"the server refused $k\", \"hint\": \"try $k\"}"
exit 1
