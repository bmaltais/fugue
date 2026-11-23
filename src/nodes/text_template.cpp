// Text Template (spec 14.1): a template text filled with the text of inputs a to f.
//
// {name} is the input's text (empty when it has none), {{ and }} are a literal { and }, and any other
// brace use is an error that names it, so a typo never silently leaves a hole in a prompt.

#include <QHash>
#include <QStringList>

#include "engine/node_kernel.h"
#include "nodes/kernels.h"

namespace fugue::nodes {

namespace {

const QStringList &slotNames() {
    static const QStringList names{"a", "b", "c", "d", "e", "f"};
    return names;
}

Error templateError(const QString &message) {
    return makeInputError(QStringLiteral("template_invalid"), QStringLiteral("template"), message,
                          QStringLiteral("Placeholders are {a} to {f}; write {{ or }} for a literal brace."));
}

// The filled template; `values` maps a slot name to its text (null: only check the template).
Result<QString> fill(const QString &text, const QHash<QString, QString> *values) {
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == '}') {
            if (i + 1 < text.size() && text.at(i + 1) == '}') {
                out += '}';
                ++i;
                continue;
            }
            return Result<QString>(templateError(QStringLiteral("A single } at position %1 closes nothing; write }} for a literal one.").arg(i + 1)));
        }
        if (c != '{') {
            out += c;
            continue;
        }
        if (i + 1 < text.size() && text.at(i + 1) == '{') {
            out += '{';
            ++i;
            continue;
        }
        const qsizetype close = text.indexOf('}', i + 1);
        if (close < 0)
            return Result<QString>(templateError(QStringLiteral("The { at position %1 is never closed; write {{ for a literal one.").arg(i + 1)));
        const QString name = text.mid(i + 1, close - i - 1);
        if (!slotNames().contains(name))
            return Result<QString>(templateError(QStringLiteral("The template has an unknown placeholder {%1}; the inputs are %2.")
                                                            .arg(name, slotNames().join(", "))));
        if (values)
            out += values->value(name);
        i = close;
    }
    return out;
}

class TextTemplateKernel : public NodeKernel {
public:
    Result<Values> run(const NodeContext &context) override {
        QHash<QString, QString> values;
        for (const QString &name : slotNames())
            values.insert(name, context.input(name).text());
        const Result<QString> filled = fill(context.input("template").text(), &values);
        if (!filled.ok())
            return Result<Values>(filled.errors());
        QString text = filled.value();
        if (context.input("trim").json().toBool())
            text = text.trimmed();
        return Values{{"text", Value(text)}};
    }

    Errors check(const NodeContext &context) const override {
        if (!context.inputs.contains("template"))
            return {};
        const Result<QString> checked = fill(context.input("template").text(), nullptr);
        return checked.ok() ? Errors{} : checked.errors();
    }
};

} // namespace

void addTextTemplateKernels(Kernels &kernels) {
    kernels.add("fugue/text-template", std::make_shared<TextTemplateKernel>());
}

} // namespace fugue::nodes
