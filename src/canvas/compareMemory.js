.pragma library

// The compare mode last picked in a canvas surface (#228, the cohesion audit's C4): the Canvas view and Compare Images
// open their compare in it, so a person who compares by Difference keeps Difference from one to the other. One for the
// whole window (a JS library is shared by everything that imports it); a session's choice, not a setting.

var last = "slider"

function remember(mode) { last = mode }
function remembered() { return last }
