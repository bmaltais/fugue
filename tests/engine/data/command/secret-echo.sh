cat > request.json
env > env.txt
k=$(sed 's/.*"value":"\([^"]*\)".*/\1/' request.json)
echo "{\"op\": \"progress\", \"value\": 0.5, \"message\": \"using $k\"}"
echo "{\"outputs\": {\"text\": \"echo $k\"}}"
