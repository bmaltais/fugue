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

// "line 2, column 5 of the template", as the Dynamic Prompt's problems name a place.
QString placeOf(const QString &text, qsizetype pos) {
    const qsizetype line = text.left(pos).count(QLatin1Char('\n'));
    const qsizetype start = text.lastIndexOf(QLatin1Char('\n'), pos - 1) + 1;
    return QStringLiteral("line %1, column %2 of the template").arg(line + 1).arg(pos - start + 1);
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
            return Result<QString>(templateError(QStringLiteral("A single } closes nothing (%1); write }} for a literal one.").arg(placeOf(text, i))));
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
            return Result<QString>(templateError(QStringLiteral("The { is never closed (%1); write {{ for a literal one.").arg(placeOf(text, i))));
        const QString name = text.mid(i + 1, close - i - 1);
        if (!slotNames().contains(name))
            return Result<QString>(templateError(QStringLiteral("The template has an unknown placeholder {%1} (%2); the inputs are %3.")
                                                            .arg(name, placeOf(text, i), slotNames().join(", "))));
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

Errors checkTextTemplate(const QString &text) {
    const Result<QString> checked = fill(text, nullptr);
    return checked.ok() ? Errors{} : checked.errors();
}

void addTextTemplateKernels(Kernels &kernels) {
    kernels.add("fugue/text-template", std::make_shared<TextTemplateKernel>());
}

} // namespace fugue::nodes
