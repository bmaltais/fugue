# shellcheck shell=bash
# Shared by the tours (source "$TOUR_DIR/lib.sh"). A tour finds what it clicks by objectName, not by pixel: the app
# answers "where is this item" through its test-only probe (src/tour_probe.h), which is on only when FUGUE_TOUR_PROBE names a
# folder. docs/recording-screenshots.md says more.

# In tour_prepare: make the probe's folder and hand its name to the app (the sandbox session inherits the environment).
probe_prepare() {
  FUGUE_TOUR_PROBE=$(mktemp -d)
  export FUGUE_TOUR_PROBE
}

# probe_all QUERY: one line per visible match, "objectName x y width height". QUERY is objectNames joined by "/" (each
# searched below the one before); a name with * in it is a wildcard pattern. Prints nothing when there is none.
probe_all() {
  local query=$1
  rm -f "$FUGUE_TOUR_PROBE/reply"
  printf '# %s\n%s\n' "$RANDOM$RANDOM" "$query" >"$FUGUE_TOUR_PROBE/ask.new" && mv "$FUGUE_TOUR_PROBE/ask.new" "$FUGUE_TOUR_PROBE/ask"
  local i
  for ((i = 0; i < 40; i++)); do
    if [[ -f $FUGUE_TOUR_PROBE/reply ]] && [[ $(tail -n 1 "$FUGUE_TOUR_PROBE/reply") == . ]]; then
      sed -e '1d' -e '$d' "$FUGUE_TOUR_PROBE/reply"
      return 0
    fi
    sleep 0.15
  done
  return 1
}

# probe_wait QUERY [seconds] [which]: wait for the QUERY to match (the which-th match, from 1), print "x y width height".
# A target that never shows fails the tour at once with its name, instead of clicking into the void.
probe_wait() {
  local query=$1 limit=${2:-30} which=${3:-1} line end=$((SECONDS + ${2:-30}))
  while ((SECONDS < end)); do
    line=$(probe_all "$query" | sed -n "${which}p")
    if [[ -n $line ]]; then
      read -r _ x y w h <<<"$line"
      echo "$x $y $w $h"
      return 0
    fi
    sleep 0.3
  done
  echo "tour: no item matches '$query' after ${limit}s" >&2
  return 1
}

# center_of QUERY [which]: "x y", the middle of the item once it has stopped moving (a popup or dialog slides in, so a
# position read mid-way would be a click that misses).
center_of() {
  local now before="" x y w h tries=0
  while ((tries++ < 20)); do
    now=$(probe_wait "$1" "${PROBE_WAIT:-30}" "${2:-1}") || return 1
    if [[ $now == "$before" ]]; then
      read -r x y w h <<<"$now"
      echo "$((x + w / 2)) $((y + h / 2))"
      return 0
    fi
    before=$now
    sleep 0.3
  done
  tour_fail "$1 never stopped moving"
}

# click_on QUERY [pause] [which]: glide to the middle of the item and click it.
click_on() {
  local at
  at=$(center_of "$1" "${3:-1}") || tour_fail "cannot find $1"
  # shellcheck disable=SC2086
  click $at "${2:-0.9}"
}

# tour_fail MESSAGE: stop the tour, with the reason on the terminal.
tour_fail() {
  echo "tour: $1" >&2
  exit 1
}

# menu_command COMMAND [pause] [TITLE]: open the menu that holds the command and click it. Menu titles are found by
# objectName (the window's bar is menuTitle-N, a view's own bar, as the Graph's View, Select, Add and Node, is
# <view>MenuTitle-<name>). TITLE is an objectName pattern of the title to try first (menuTitle-0, *MenuTitle-View), so the pointer goes straight there; when the
# command is not in it, every title is opened in turn until one holds it. The devkit sends no key chords.
menu_command() {
  local command=$1 pause=${2:-1.2} first="" title end=$((SECONDS + 20))
  [[ -n ${3:-} ]] && first=$(probe_all "$3" | head -n 1 | cut -d' ' -f1)
  while ((SECONDS < end)); do
    for title in $first $({ probe_all "menuTitle-*"; probe_all "*MenuTitle-*"; } | cut -d' ' -f1); do
      PROBE_WAIT=5 click_on "$title" 0.6 || return 1
      if probe_all "menuItem-$command" | grep -q .; then
        click_on "menuItem-$command" "$pause"
        return
      fi
    done
    sleep 0.5
  done
  tour_fail "no menu holds the command $command"
}

# probe_gone QUERY [seconds]: wait until nothing matches any more (false after the limit).
probe_gone() {
  local end=$((SECONDS + ${2:-60}))
  while ((SECONDS < end)); do
    probe_all "$1" | grep -q . || return 0
    sleep 0.5
  done
  return 1
}

# template_open ID: File > Browse templates, type the template's id in the search (it matches ids, so one card is left),
# check that this card is the only match, and open it with Return. (A click on the card also lands its release on whatever
# the closing dialog uncovers, so the keyboard opens it.)
template_open() {
  local id=$1 cards
  menu_command file.templates 1.5 menuTitle-0
  click_on templatesSearch 0.6
  typed "$id"
  probe_wait "templateCard-$id" 30 >/dev/null || tour_fail "the templates dialog has no card for $id"
  sleep 2                                      # the grid narrows to the matches
  cards=$(probe_all "templateCard-*" | cut -d' ' -f1 | tr '\n' ' ')
  [[ $cards == "templateCard-$id " ]] || tour_fail "searching for $id left these cards: ${cards:-none}"
  pressed Return
  sleep 4
}

# tour_pick_template: sets TOUR_TEMPLATE to the first of TOUR_TEMPLATE_CHOICES (or the one named in the environment's
# TOUR_TEMPLATE) whose models this machine has, as `fugue template list` says. Fails with the models that are missing when
# none is ready, so a tour never opens a template that stops at the Get ready sheet. Run it from tour_requires.
tour_pick_template() {
  local bin=${FUGUE_BIN:-build/fugue} list id missing report=""
  list=$("$bin" template list --json) || { echo "tour: '$bin template list' failed" >&2; return 1; }
  local choices=("${TOUR_TEMPLATE_CHOICES[@]}")
  [[ -n ${TOUR_TEMPLATE:-} ]] && choices=("$TOUR_TEMPLATE")
  for id in "${choices[@]}"; do
    missing=$(jq -r --arg id "$id" '.data.templates[] | select(.id == $id) | [.models[] | select(.here | not) | .name] | join(", ")' <<<"$list")
    if jq -e --arg id "$id" '.data.templates[] | select(.id == $id)' >/dev/null <<<"$list"; then
      if [[ -z $missing ]]; then TOUR_TEMPLATE=$id; echo "tour: using the template $id" >&2; return 0; fi
      report+=$'\n  '"$id: missing $missing"
    else
      report+=$'\n  '"$id: not among the templates"
    fi
  done
  echo "tour: none of these templates has its models on this machine:$report" >&2
  echo "tour: put the models in a model folder of the settings, or name another template (TOUR_TEMPLATE=<id>)." >&2
  return 1
}

# tour_find_helper: a worktree has no build-helper-<backend>/ of its own. Without FUGUE_HELPER, use the one in the main
# checkout of this repo (the first of `git worktree list`), so a GPU run finds the helper. Read in place, never written.
tour_find_helper() {
  [[ -n ${FUGUE_HELPER+x} ]] && return 0
  local main program
  main=$(git worktree list --porcelain | sed -n '1s/^worktree //p')
  for program in "$main"/build-helper-cuda/fugue-helper-cuda "$main"/build-helper-vulkan/fugue-helper-vulkan; do
    if [[ -x $program ]]; then export FUGUE_HELPER=$program; echo "tour: helper $program" >&2; return 0; fi
  done
  return 0
}

# show_graph: the window opens on the Apps shelf (a fresh one) or, after a template that is an app, on its simple screen; the
# canvas is what the clips show. One click on the right half of the App/Graph switch, found by name.
show_graph() {
  if ! probe_all canvas | grep -q .; then
    local x y w h
    read -r x y w h < <(probe_wait appViewSwitch 20) || tour_fail "no App/Graph switch"
    click $((x + w * 3 / 4)) $((y + h / 2)) 1.5
  fi
  probe_wait canvas 20 >/dev/null || tour_fail "the canvas never showed"
}
