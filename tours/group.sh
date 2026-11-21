# shellcheck shell=bash disable=SC2034
# Making a group: open a template, select three nodes with a rubber band, Node > Make a group, then
# collapse the group to one node.
TOUR_OUT=docs/images/tour-group.avif
TOUR_SIZE=1280x800
TOUR_MAX_SECONDS=300

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

# Left-drag on the empty canvas selects (the "standard" navigation preset) instead of panning; the
# sandbox's copy of the settings is the only one changed.
tour_prepare() {
  local f=$HOME/.config/fugue/settings.json
  jq '.navigation = "standard"' "$f" >"$f.new" && mv "$f.new" "$f"
}

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  click 25 13 0.8            # File
  click 87 133 1.5           # Browse templates
  sleep 3
  typed "klein 4b"
  sleep 2
  pressed Return
  sleep 6
  click 109 13 0.8           # View
  click 151 48 1.5           # Fit the view
  glide 350 225
  drag 380 280               # a rubber band round Text Encode, Empty Latent and Sampler
  sleep 1.5
  click 157 13 0.8           # Node
  click 203 384 2            # Make a group
  sleep 3
}
