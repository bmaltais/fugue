#!/bin/bash
# One-shot command node: one JSON request on stdin, one JSON result line on stdout.
request=$(cat)
text=$(jq -r '.inputs.text' <<<"$request")
mark=$(jq -r '.inputs.mark' <<<"$request")
times=$(jq -r '.inputs.times' <<<"$request")
marks=$(printf "%${times}s" | sed "s/ /$mark/g")
jq -cn --arg text "${text^^}$marks" '{outputs: {text: $text}}'
