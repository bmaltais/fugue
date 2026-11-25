#!/bin/bash
# One-shot command node: the protocol on stdin and stdout, the work in request.py (Python 3 standard library only).
exec python3 -I "$(dirname "$0")/request.py"
