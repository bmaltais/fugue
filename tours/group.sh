# shellcheck shell=bash disable=SC2034
# Making a group: open a template whose models this machine has, select two or three nodes with a rubber band, Node > Make a
# group. Targets are found by objectName (tours/lib.sh). The nodes to select are the ones this table names for the template
# (a template that is ready but not in it fails with a message); TOUR_TEMPLATE picks one.
TOUR_OUT=docs/images/tour-group.avif
TOUR_SIZE=1280x800
TOUR_MAX_SECONDS=300
TOUR_TEMPLATE_CHOICES=(flux2-klein-4b flux2-klein-9b z-image-turbo image-upscale)
# Template id -> the ids of the nodes the rubber band takes: a block of the graph with no other node inside its box.
declare -A TOUR_GROUP_NODES=(
  [flux2-klein-4b]="encode sample decode"
  [flux2-klein-9b]="encode sample decode"
  [z-image-turbo]="encode sample decode"
  [image-upscale]="plain model"
)

# shellcheck source=lib.sh
source "$TOUR_DIR/lib.sh"
tour_requires() {
  tour_pick_template || return 1
  [[ -n ${TOUR_GROUP_NODES[$TOUR_TEMPLATE]:-} ]] || { echo "tour: group.sh has no nodes listed for $TOUR_TEMPLATE" >&2; return 1; }
}

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

# Left-drag on the empty canvas selects (the "standard" navigation preset) instead of panning; the
# sandbox's copy of the settings is the only one changed.
tour_prepare() {
  probe_prepare
  local f=$HOME/.config/fugue/settings.json
  jq '.navigation = "standard"' "$f" >"$f.new" && mv "$f.new" "$f"
}

# band_around ID...: the rubber band's corners, "x0 y0 x1 y1", 14 px outside the nodes' joint box.
band_around() {
  local id x y w h x0=99999 y0=99999 x1=0 y1=0
  for id in "$@"; do
    read -r x y w h < <(probe_wait "node-$id" 20) || return 1
    ((x < x0)) && x0=$x
    ((y < y0)) && y0=$y
    ((x + w > x1)) && x1=$((x + w))
    ((y + h > y1)) && y1=$((y + h))
  done
  echo "$((x0 - 14)) $((y0 - 14)) $((x1 + 14)) $((y1 + 14))"
}

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  template_open "$TOUR_TEMPLATE"
  show_graph
  menu_command view.fit 1.5 '*MenuTitle-View'
  sleep 1
  local x0 y0 x1 y1
  # shellcheck disable=SC2086
  read -r x0 y0 x1 y1 < <(band_around ${TOUR_GROUP_NODES[$TOUR_TEMPLATE]}) || tour_fail "cannot find the nodes to group"
  glide "$x0" "$y0"
  drag $((x1 - x0)) $((y1 - y0))               # a rubber band round the nodes
  sleep 1.5
  menu_command group.make 2 '*MenuTitle-Node'
  sleep 1.5
  esc                                           # ends the name edit the new group starts
  sleep 2
  probe_all "node-*" | grep -q . || tour_fail "no node on the canvas after the group"
}
