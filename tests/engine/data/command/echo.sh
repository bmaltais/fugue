cat > request.json
env | grep '^FUGUE_' | sort > env.txt
echo '{"op": "progress", "value": 0.4, "message": "pass 2 of 5"}'
echo '{"outputs": {"text": "hello"}}'
