# shellcheck shell=bash disable=SC2034
# Wiring a node: open node search, add two nodes, connect two ports. A clip of the canvas.
TOUR_OUT=docs/images/tour-wire.avif
TOUR_SIZE=1280x800
TOUR_CROP=1040x500+160+260
TOUR_MAX_SECONDS=240

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  click 300 330 0.5          # a click on the empty canvas puts the keys there
  pressed Tab                # Tab opens node search
  sleep 2
  typed "number"
  sleep 1.5
  pressed Return             # adds the best match where the search opened
  sleep 2
  click 680 330 0.5
  pressed Tab
  sleep 2
  typed "preview"
  sleep 1.5
  pressed Return
  sleep 2
  glide 541 374
  drag 139 4                 # from Number's output port to Preview's input
  sleep 3
}
