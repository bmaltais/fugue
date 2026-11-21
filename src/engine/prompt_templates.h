#pragma once

// Prompt templates (spec 14.2, #140): the instructions a text node gives a language model to write
// a prompt for some other model, kept as data. Enhance Prompt's Style list is this set, so a new
// target (a video model, a music model) is a file, not a code change.
//
// Template files are JSON, {"prompt_templates": 1, "templates": [{...}]}; see src/prompts/*.json.
// Fugue's own are compiled in (`:/fugue/prompts/*.json`); the person's, in dataDir()/prompts/*.json,
// are read after them, and a template there with the id of an earlier one replaces it, as for the
// model catalog (engine/model_catalog.h). A template that is not valid (no id, label or system
// prompt, an id that is not lower case with hyphens, an unknown family, a repeated id) is left
// out and said in problems(). The list is in the order image, video, audio, and within a family
// as the files list it.
//
// A template holds what to ask (`system`, one string or a list of lines joined with a newline),
// what it is for (`family` image, video or audio; `target`, the models it suits), what a person
// should read (`notes`), and optionally worked `examples` ({idea, prompt}), which are shown to the
// language model after the system prompt. `duration_note` and `language_note` are what the
// node's Length and Language options add to the instructions, with {seconds} and {language} in
// them; a template without `duration_note` ignores the length.

#include <QList>
#include <QString>
#include <QStringList>

namespace fugue {

struct PromptExample {
    QString idea;
    QString prompt;
};

struct PromptTemplate {
    QString id;           // lower case letters, digits and hyphens: what the node's Style holds
    QString label;        // "MiniMax video"
    QString family;       // "image", "video" or "audio"
    QString target;       // the models it suits: "MiniMax H3, Hailuo"
    QString system;       // the instructions
    QString notes;        // a line or two for a person: when to use it, where it comes from
    QString durationNote; // with {seconds}; empty: this template does not use a length
    QString languageNote; // with {language}; empty: the general sentence
    QList<PromptExample> examples;
};

// What a node's other inputs add to a template's instructions. Empty or zero: nothing is added.
struct PromptOptions {
    QString language;     // the language of the prompt: "English", "français"
    int durationSeconds = 0;
    QString instructions; // free words from the person ("Also: ...")
};

class PromptTemplates {
public:
    // Fugue's own templates, then the files of `userFolder` (default dataDir()/prompts); read each
    // time, so a file dropped in is there at once.
    static PromptTemplates load(const QString &userFolder = {});
    // One file's text; what it says wrong goes to problems().
    static PromptTemplates parse(const QByteArray &json, const QString &what);

    const QList<PromptTemplate> &all() const { return m_templates; }
    const PromptTemplate *find(const QString &id) const;
    QStringList ids() const;
    // What was left out, one sentence each, naming the file and the template.
    const QStringList &problems() const { return m_problems; }

    // Adds `other`'s templates: one with the id of a template here replaces it, in its place.
    void merge(const PromptTemplates &other);

private:
    QList<PromptTemplate> m_templates;
    QStringList m_problems;
};

// The system prompt a template and the options make: the template's instructions, its examples,
// the length and language sentences, then the person's own words ("Also: ...").
QString systemPrompt(const PromptTemplate &prompt, const PromptOptions &options);

// "id: label, for target" lines: what the Style input says in its description.
QString styleSummary(const PromptTemplates &templates);

} // namespace fugue
