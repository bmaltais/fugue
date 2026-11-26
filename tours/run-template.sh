# shellcheck shell=bash disable=SC2034
# Running a template: File > Browse templates, open a template whose models this machine has, show its graph and press
# Run. The generation really finishes in the sandbox (the models are read from the real model folders). Targets are found by
# objectName (tours/lib.sh), never by pixel. The template is the first of TOUR_TEMPLATE_CHOICES that is ready, or the one
# in TOUR_TEMPLATE; none ready fails before anything is recorded, naming the missing models.
TOUR_OUT=docs/images/tour-run-template.avif
TOUR_SIZE=1280x800
TOUR_MAX_SECONDS=420
TOUR_TEMPLATE_CHOICES=(flux2-klein-4b flux2-klein-9b z-image-turbo image-upscale)

# shellcheck source=lib.sh
source "$TOUR_DIR/lib.sh"
tour_requires() { tour_find_helper; tour_pick_template; }
tour_prepare() { probe_prepare; }

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  template_open "$TOUR_TEMPLATE"
  # An app opens as its simple screen; the graph is what the clip shows.
  show_graph
  menu_command view.fit 1.5 '*MenuTitle-View'
  menu_command run.start 1 menuTitle-2
  probe_wait runPillBox 60 >/dev/null || tour_fail "the run never started"
  probe_gone runPillBox 400 || tour_fail "the run did not finish in time"
  sleep 3                      # the finished graph, with the picture on the nodes
  probe_all runPillBox | grep -q . && tour_fail "the run ended with errors"
  sleep 3
}
