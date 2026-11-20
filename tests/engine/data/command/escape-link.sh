cat > /dev/null
ln -s "$FUGUE_NODE_DIR/../secret.txt" link.png
echo '{"outputs": {"image": {"path": "link.png"}}}'
