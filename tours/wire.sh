# shellcheck shell=bash disable=SC2034
# Wiring a node: open node search, add two nodes, connect two ports. A clip of the canvas. Targets are found by objectName
# (tours/lib.sh); the two nodes go where the canvas was clicked, so those two points are the only fixed ones, and they sit
# inside the crop and well inside the canvas.
TOUR_OUT=docs/images/tour-wire.avif
TOUR_SIZE=1280x800
TOUR_CROP=1040x500+160+260
TOUR_MAX_SECONDS=300

# shellcheck source=lib.sh
source "$TOUR_DIR/lib.sh"
tour_prepare() { probe_prepare; }

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

# add_node X Y WORD: a click on the empty canvas puts the keys there, Tab opens node search, Return adds the best match.
add_node() {
  click "$1" "$2" 0.5
  pressed Tab
  sleep 2
  typed "$3"
  sleep 1.5
  pressed Return
  sleep 2
}

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  show_graph
  add_node 300 330 number
  add_node 680 330 preview
  local nodes
  nodes=$(probe_all "node-*" | cut -d' ' -f1)
  [[ $(wc -w <<<"$nodes") -eq 2 ]] || tour_fail "expected two nodes, found: ${nodes:-none}"
  local first second out in ox oy ix iy w h
  first=$(head -n 1 <<<"$nodes") second=$(tail -n 1 <<<"$nodes")
  # Number's output to Preview's input: the first node's first output dot, the second's first input dot.
  read -r ox oy w h < <(probe_wait "$first/outputDot-*" 20) || tour_fail "no output port on $first"
  read -r ix iy w h < <(probe_wait "$second/inputDot-*" 20) || tour_fail "no input port on $second"
  read -r out in <<<"$((ox + w / 2)) $((ix + w / 2))"
  glide "$out" $((oy + h / 2))
  drag $((in - out)) $((iy - oy))
  sleep 3
}
