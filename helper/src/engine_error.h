// The engine's own reason for a failure (#542). A component that cannot be built fails with the one
// sentence "cannot load component from '<file>'": the reason (a missing tokenizer, a tensor that is
// not there) goes to the engine's log as an error line and is lost to the caller. The helper keeps
// the last error line of the log and appends it to such a message.
#pragma once

#include <string>

namespace fugue_helper {

// An engine log line as the reason it states: the text without the "--- file:line" suffix the
// engine adds and without the line end. Empty when there is nothing to say.
inline std::string engineLogReason(const std::string &line) {
    std::string text = line;
    const std::string::size_type where = text.rfind(" --- ");
    if (where != std::string::npos)
        text.erase(where);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

// `message` (the engine's last error) with the reason from its log when the message says only that
// a component could not be loaded. A message that already holds the reason is left as it is.
inline std::string withEngineReason(const std::string &message, const std::string &reason) {
    if (reason.empty() || message.find("cannot load component") == std::string::npos || message.find(reason) != std::string::npos)
        return message;
    return message + " (the engine said: " + reason + ")";
}

} // namespace fugue_helper
