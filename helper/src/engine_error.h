// The engine's own reason for a failure (#542). A component that cannot be built fails with the one
// sentence "cannot load component from '<file>'": the reason (a missing tokenizer, a tensor that is
// not there) goes to the engine's log as an error line and is lost to the caller. The helper keeps
// the last error line of the log and appends it to such a message.
#pragma once

#include <mutex>
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

// The engine reads its `tokenizer` option as a list of slot settings ("main=FILE,clip-l=FILE"), so a path with a
// "=" or a "," in it would be taken apart. Empty when the path is safe to give it.
inline std::string tokenizerPathError(const std::string &path) {
    if (path.find_first_of("=,") == std::string::npos)
        return {};
    return "the tokenizer file " + path + " has a \"=\" or a \",\" in its path, which the engine reads as a setting; move or rename it";
}

// The longest reason appended: a file the engine failed to parse can put some of its content in the error, and the
// message goes to a remote client.
inline constexpr std::string::size_type kEngineReasonMax = 200;

inline std::string cappedReason(std::string reason) {
    if (reason.size() <= kEngineReasonMax)
        return reason;
    std::string::size_type cut = kEngineReasonMax;
    while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0) == 0x80) // not inside a UTF-8 character
        --cut;
    return reason.substr(0, cut) + "...";
}

// `message` (the engine's last error) with the reason from its log when the message says only that
// a component could not be loaded. A message that already holds the reason is left as it is.
inline std::string withEngineReason(const std::string &message, const std::string &reason) {
    if (reason.empty() || message.find("cannot load component") == std::string::npos || message.find(reason) != std::string::npos)
        return message;
    return message + " (the engine said: " + cappedReason(reason) + ")";
}

} // namespace fugue_helper

namespace fugue_helper {

// The last error line of the engine's log, for the job that is running: reset when a job starts, so a line
// from an earlier job (one that went on to succeed, or failed another way) is never the reason of this one.
class EngineReason {
public:
    void record(const std::string &logLine) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reason = engineLogReason(logLine);
    }
    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reason.clear();
    }
    // `message` with the reason when it applies; the reason is spent.
    std::string explain(const std::string &message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string out = withEngineReason(message, m_reason);
        m_reason.clear();
        return out;
    }

private:
    std::mutex m_mutex;
    std::string m_reason;
};

} // namespace fugue_helper
