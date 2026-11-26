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
  local before after pill seen=0 end
  before=$(probe_count thumbnail) || tour_fail "the app does not answer"
  menu_command run.start 1 menuTitle-2
  # Finished means the run pill showed and went, or, for a run so quick the pill was never caught, a new picture is on a
  # node. Never "no answer": a probe that fails fails the tour.
  end=$((SECONDS + 60))
  while ((SECONDS < end)); do
    pill=$(probe_all runPillBox) || tour_fail "the app stopped answering"
    [[ -n $pill ]] && { seen=1; break; }
    after=$(probe_count thumbnail) || tour_fail "the app stopped answering"
    ((after > before)) && break
    sleep 0.5
  done
  if ((seen)); then
    probe_gone runPillBox 400
    case $? in
      0) ;;
      1) tour_fail "the run did not finish in time" ;;
      *) tour_fail "the app stopped answering during the run" ;;
    esac
  elif ((SECONDS >= end)); then
    tour_fail "the run never started"
  fi
  sleep 3                      # the finished graph, with the picture on the nodes
  local left
  left=$(probe_all runPillBox) || tour_fail "the app stopped answering"
  [[ -z $left ]] || tour_fail "the run ended with errors"
  sleep 3
}
