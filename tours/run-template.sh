# shellcheck shell=bash disable=SC2034
# Running a template: File > Browse templates, open FLUX.2 Klein 4B, fit the view and press Run. The
# generation really finishes in the sandbox (the models are read from the real model folders).
TOUR_OUT=docs/images/tour-run-template.avif
TOUR_SIZE=1280x800
TOUR_MAX_SECONDS=300

# The first frames of a software-GL window are slow: let it settle before filming starts.
tour_setup() { sleep 10; }

tour_play() {
  wait_until "app_window_up" 60
  sleep 1
  click 25 13 0.8            # File
  click 87 133 1.5           # Browse templates
  sleep 3
  typed "klein 4b"
  sleep 2
  pressed Return             # opens the first match
  sleep 6
  click 109 13 0.8           # View
  click 151 48 1.5           # Fit the view
  click 621 61 1             # Run
  sleep 22                   # the model loads, the sampler runs, the picture shows on the nodes
  sleep 4                    # hold the finished graph
}
