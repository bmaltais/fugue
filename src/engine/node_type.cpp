#include "node_type.h"

#include "engine/canonical.h"
#include "engine/ids.h"
#include "engine/suggest.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace fugue {

namespace {

const QString invalidDocument = QStringLiteral("invalid_document");
constexpr double maxSafeInteger = 9007199254740992.0; // 2^53 (spec 2.1)

// `<item>_<row>`, the row a whole number from 1 with no leading zero: the item and the row, or nothing.
std::optional<std::pair<QString, int>> splitRowName(const QString &name) {
    static const QRegularExpression form(QStringLiteral("^(.+)_([1-9][0-9]{0,8})$"));
    const QRegularExpressionMatch match = form.match(name);
    if (!match.hasMatch())
        return std::nullopt;
    return std::pair<QString, int>{match.captured(1), match.captured(2).toInt()};
}


// A literal as JSON, for messages.
QString shown(const QJsonValue &value) {
    if (value.isUndefined())
        return QStringLiteral("nothing");
    return QString::fromUtf8(canonicalJson(value).value_or("?"));
}

Error literalError(const QString &code, const InputSpec &input, const QString &message) {
    Error error = makeError(code, message);
    error.input = input.name;
    return error;
}

// Collects manifest errors, every message starting with the type id.
class Reader {
public:
    Reader(const QString &id, Errors &errors) : m_id(id), m_errors(errors) {}

    void fail(const QString &message, const QString &input = {}, const QString &output = {}) {
        add(invalidDocument, Error::Severity::Error, message, input, output);
    }

    void warn(const QString &message, const QString &input = {}, const QString &output = {}) {
        add(invalidDocument, Error::Severity::Warning, message, input, output);
    }

    void add(const QString &code, Error::Severity severity, const QString &message,
             const QString &input = {}, const QString &output = {}) {
        Error error = makeError(code, m_id + QStringLiteral(": ") + message);
        error.severity = severity;
        error.input = input;
        error.output = output;
        m_errors << error;
        if (severity == Error::Severity::Error)
            ++m_errorCount;
    }

    void addPrefixed(const Errors &errors) {
        for (Error error : errors) {
            error.message = m_id + QStringLiteral(": the default of input ") + error.input +
                            QStringLiteral(" is invalid: ") + error.message;
            m_errors << error;
            ++m_errorCount;
        }
    }

    bool failed() const { return m_errorCount > 0; }
    // Errors added so far, not counting warnings.
    int count() const { return m_errorCount; }

    // Field readers: absent leaves `out` unchanged; a wrong JSON type fails.
    void string(const QJsonObject &o, const char *key, QString &out, const QString &where,
                const QString &input = {}, const QString &output = {}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined())
            return;
        if (!v.isString())
            fail(QStringLiteral("%1 \"%2\" must be a string.").arg(where, QLatin1String(key)), input, output);
        else
            out = v.toString();
    }

    void boolean(const QJsonObject &o, const char *key, bool &out, const QString &where,
                 const QString &input = {}, const QString &output = {}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined())
            return;
        if (!v.isBool())
            fail(QStringLiteral("%1 \"%2\" must be true or false.").arg(where, QLatin1String(key)), input,
                 output);
        else
            out = v.toBool();
    }

    void number(const QJsonObject &o, const char *key, std::optional<double> &out, const QString &where,
                const QString &input) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined())
            return;
        if (!v.isDouble())
            fail(QStringLiteral("%1 \"%2\" must be a number.").arg(where, QLatin1String(key)), input);
        else
            out = v.toDouble();
    }

    void count(const QJsonObject &o, const char *key, std::optional<int> &out, const QString &where,
               const QString &input) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined())
            return;
        const double d = v.toDouble(-1);
        if (!v.isDouble() || d < 0 || d != std::trunc(d) || d > 1e9)
            fail(QStringLiteral("%1 \"%2\" must be a whole number of at least 0.").arg(where, QLatin1String(key)),
                 input);
        else
            out = int(d);
    }

    void strings(const QJsonObject &o, const char *key, QStringList &out, const QString &where,
                 const QString &input = {}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (v.isUndefined())
            return;
        bool ok = v.isArray();
        QStringList list;
        for (const QJsonValue item : v.toArray()) {
            ok &= item.isString();
            list << item.toString();
        }
        if (!ok)
            fail(QStringLiteral("%1 \"%2\" must be a list of strings.").arg(where, QLatin1String(key)), input);
        else
            out = list;
    }

    void unknownFields(const QJsonObject &o, const QStringList &known, const QString &where,
                       const QString &input = {}, const QString &output = {}) {
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!known.contains(it.key()))
                warn(QStringLiteral("%1 has an unknown field \"%2\", which is ignored.").arg(where, it.key()),
                     input, output);
    }

private:
    QString m_id;
    Errors &m_errors;
    int m_errorCount = 0;
};

bool widgetFits(const QString &widget, const InputSpec &input) {
    const QString type = input.type.type.id;
    if (widget == "spin" || widget == "slider" || widget == "seed")
        return type == "number";
    if (widget == "line" || widget == "multiline" || widget == "file" || widget == "folder" ||
        widget == "color" || widget == "mask" || widget == "paint" || widget == "canvas")
        return type == "text";
    if (widget == "toggle")
        return type == "boolean";
    if (widget == "choice")
        return type == "text" && input.choices.has_value();
    return false;
}

// Reads a `shown_when` or `carries_when` condition (6.1).
void readCondition(Reader &r, const QJsonObject &o, const char *key, ShownWhen &out, const QString &where,
                   const QString &input, const QString &output) {
    const QJsonValue value = o.value(QLatin1String(key));
    if (value.isUndefined())
        return;
    if (!value.isObject()) {
        r.fail(QStringLiteral("%1 \"%2\" must be an object.").arg(where, QLatin1String(key)), input, output);
        return;
    }
    QStringList problems;
    out = ShownWhen::parse(value.toObject(), problems);
    for (const QString &problem : std::as_const(problems))
        r.fail(QStringLiteral("%1 \"%2\": %3").arg(where, QLatin1String(key), problem), input, output);
}

std::optional<InputSpec> readInput(Reader &r, const QJsonValue &value, int index) {
    InputSpec input;
    const QString at = QStringLiteral("input %1").arg(index + 1);
    if (!value.isObject()) {
        r.fail(at + QStringLiteral(" must be an object."));
        return std::nullopt;
    }
    const QJsonObject o = value.toObject();
    const int before = r.count();
    bool bad = false;

    const QJsonValue name = o.value("name");
    input.name = name.toString();
    if (!isPortName(input.name)) {
        r.fail(at + QStringLiteral(" needs a \"name\" matching [a-z][a-z0-9_]*, not %1.").arg(shown(name)),
               input.name);
        bad = true;
    }
    const QString where = bad ? at : QStringLiteral("input ") + input.name;
    const QString &n = input.name;

    const QJsonValue type = o.value("type");
    const std::optional<PortType> parsed = PortType::parse(type.toString());
    if (!parsed) {
        r.fail(where + QStringLiteral(" has %1 as its \"type\", which is not a port type.").arg(shown(type)), n);
        bad = true;
    } else {
        input.type.type = *parsed;
    }

    r.boolean(o, "list", input.type.list, where, n);
    r.string(o, "label", input.label, where, n);
    r.string(o, "description", input.description, where, n);
    r.boolean(o, "optional", input.optional, where, n);
    r.boolean(o, "lazy", input.lazy, where, n);
    r.boolean(o, "takes_list", input.type.takesList, where, n);
    r.count(o, "min_items", input.minItems, where, n);
    r.count(o, "max_items", input.maxItems, where, n);
    r.number(o, "min", input.min, where, n);
    r.number(o, "max", input.max, where, n);
    r.number(o, "step", input.step, where, n);
    r.boolean(o, "integer", input.integer, where, n);
    r.string(o, "widget", input.widgetName, where, n);
    r.string(o, "syntax", input.syntax, where, n);
    r.string(o, "section", input.section, where, n);
    r.boolean(o, "advanced", input.advanced, where, n);
    r.string(o, "role", input.role, where, n);
    r.string(o, "over", input.over, where, n);
    readCondition(r, o, "shown_when", input.shownWhen, where, n, {});
    if (o.contains("choices")) {
        QStringList choices;
        r.strings(o, "choices", choices, where, n);
        if (choices.isEmpty())
            r.fail(where + QStringLiteral(" has empty \"choices\"."), n);
        input.choices = choices;
    }
    input.defaultValue = o.value("default");
    r.unknownFields(o,
                    {"name", "type", "label", "description", "default", "optional", "lazy", "takes_list", "list", "min_items",
                     "max_items", "min", "max", "step", "integer", "choices", "widget", "syntax", "section",
                     "advanced", "role", "over", "shown_when"},
                    where, n);

    if (parsed) {
        const QString id = parsed->id;
        const bool isNumber = id == "number";
        const bool scalar = isNumber || id == "text" || id == "boolean";
        if (!isNumber)
            for (const char *key : {"min", "max", "step", "integer"})
                if (o.contains(QLatin1String(key)))
                    r.fail(where + QStringLiteral(" is %1, but \"%2\" only applies to number inputs.")
                                       .arg(id, QLatin1String(key)),
                           n);
        if (!input.role.isEmpty() && (input.role != QLatin1String(kMaskRole) || id != "image"))
            r.fail(where + QStringLiteral(" has the role \"%1\"; the only role is \"mask\", on an image port.")
                               .arg(input.role),
                   n);
        if (input.choices && id != "text")
            r.fail(where + QStringLiteral(" is %1, but \"choices\" only applies to text inputs.").arg(id), n);
        if (input.type.takesList && (id != "any" || input.type.list))
            r.fail(where + QStringLiteral(" has \"takes_list\" but is not a single-value \"any\" input."), n);
        if (!input.type.list && (input.minItems || input.maxItems))
            r.fail(where + QStringLiteral(" has \"min_items\" or \"max_items\" but is not a list."), n);
        if (input.min && input.max && *input.min > *input.max)
            r.fail(where + QStringLiteral(" has \"min\" above \"max\"."), n);
        if (input.step && *input.step <= 0)
            r.fail(where + QStringLiteral(" has a \"step\" that is not above 0."), n);
        if (input.minItems && input.maxItems && *input.minItems > *input.maxItems)
            r.fail(where + QStringLiteral(" has \"min_items\" above \"max_items\"."), n);
        if (!input.over.isEmpty() && input.widgetName != QLatin1String("mask") && input.widgetName != QLatin1String("paint"))
            r.fail(where + QStringLiteral(" has \"over\", which only applies to the widgets \"mask\" and \"paint\"."), n);
        if (!input.syntax.isEmpty()) {
            if (input.syntax != QLatin1String("dynamic-prompt") && input.syntax != QLatin1String("template"))
                r.fail(where + QStringLiteral(" has the syntax \"%1\"; the syntaxes are \"dynamic-prompt\" and \"template\".").arg(input.syntax), n);
            else if (id != "text" || (input.widget() != QLatin1String("multiline") && input.widget() != QLatin1String("line")))
                r.fail(where + QStringLiteral(" has \"syntax\", which only applies to text inputs with the widget \"line\" or \"multiline\"."), n);
        }
        if (!input.widgetName.isEmpty() && !widgetFits(input.widgetName, input))
            r.fail(where + QStringLiteral(" is %1 and cannot use the widget \"%2\".")
                               .arg(input.type.toString(), input.widgetName),
                   n);
        if (!input.defaultValue.isUndefined()) {
            if (!scalar) {
                r.fail(where + QStringLiteral(" is %1 and cannot have a \"default\"; only text, number "
                                              "and boolean inputs can.")
                                   .arg(id),
                       n);
            } else if (input.type.list && input.defaultValue.isArray()) {
                for (const QJsonValue item : input.defaultValue.toArray())
                    r.addPrefixed(checkLiteral(input, item, PortKind::Builtin));
            } else {
                r.addPrefixed(checkLiteral(input, input.defaultValue, PortKind::Builtin));
            }
        }
    }

    if (bad || r.count() != before)
        return std::nullopt;
    return input;
}

// A repeating group of inputs (6.1, #714): {"repeat": <name>, "label", "max", "inputs": [<item inputs>]}. An item is an
// input as any other, except that it is not a list or lazy, holds no input file (a file, mask or paint widget) and is no
// seed (the code that reads those walks the declared inputs only), has no
// `shown_when` and no `over`, and its name does not end in `_<number>`; the first item, the row's key, has no default
// (a row is there when its key has a value).
std::optional<RepeatSpec> readRepeat(Reader &r, const QJsonObject &o, int position) {
    RepeatSpec repeat;
    repeat.position = position;
    const int before = r.count();
    repeat.name = o.value("repeat").toString();
    if (!isPortName(repeat.name)) {
        r.fail(QStringLiteral("a repeating input needs a \"repeat\" name matching [a-z][a-z0-9_]*, not %1.").arg(shown(o.value("repeat"))));
        return std::nullopt;
    }
    const QString where = QStringLiteral("repeating input ") + repeat.name;
    r.string(o, "label", repeat.label, where);
    std::optional<int> max;
    r.count(o, "max", max, where, {});
    if (max && (*max < 1 || *max > kMaxRepeatRows))
        r.fail(where + QStringLiteral(" has a \"max\" outside 1 to %1.").arg(kMaxRepeatRows));
    else if (max)
        repeat.max = *max;
    r.unknownFields(o, {"repeat", "label", "max", "inputs"}, where);
    const QJsonArray items = o.value("inputs").toArray();
    if (!o.value("inputs").isArray() || items.isEmpty())
        r.fail(where + QStringLiteral(" needs \"inputs\": the inputs of one row, at least one."));
    for (qsizetype i = 0; i < items.size(); ++i) {
        const std::optional<InputSpec> item = readInput(r, items.at(i), int(i));
        if (!item)
            continue;
        const QString at = where + QStringLiteral(" has the input ") + item->name + QStringLiteral(", which ");
        if (splitRowName(item->name))
            r.fail(at + QStringLiteral("ends in _ and a number, as the inputs of its rows do."), item->name);
        if (item->type.list || item->lazy)
            r.fail(at + QStringLiteral("is a list or lazy; a row's input takes one value, always read."), item->name);
        if (item->holdsInputFile() || item->widget() == QLatin1String("seed"))
            r.fail(at + QStringLiteral("holds an input file or a seed; a row's input cannot."), item->name);
        if (!item->shownWhen.isEmpty() || !item->over.isEmpty())
            r.fail(at + QStringLiteral("has \"shown_when\" or \"over\"; a row's input cannot."), item->name);
        if (i == 0 && !item->defaultValue.isUndefined())
            r.fail(at + QStringLiteral("is the row's key and cannot have a default: a row is there when its key has a value."),
                   item->name);
        repeat.items << *item;
    }
    if (r.count() != before)
        return std::nullopt;
    return repeat;
}

std::optional<OutputSpec> readOutput(Reader &r, const QJsonValue &value, int index) {
    OutputSpec output;
    const QString at = QStringLiteral("output %1").arg(index + 1);
    if (!value.isObject()) {
        r.fail(at + QStringLiteral(" must be an object."));
        return std::nullopt;
    }
    const QJsonObject o = value.toObject();
    const int before = r.count();
    bool bad = false;

    const QJsonValue name = o.value("name");
    output.name = name.toString();
    if (!isPortName(output.name)) {
        r.fail(at + QStringLiteral(" needs a \"name\" matching [a-z][a-z0-9_]*, not %1.").arg(shown(name)), {},
               output.name);
        bad = true;
    }
    const QString where = bad ? at : QStringLiteral("output ") + output.name;
    const QString &n = output.name;

    const QJsonValue type = o.value("type");
    const std::optional<PortType> parsed = PortType::parse(type.toString());
    if (!parsed) {
        r.fail(where + QStringLiteral(" has %1 as its \"type\", which is not a port type.").arg(shown(type)), {},
               n);
        bad = true;
    } else if (parsed->isAny() && !o.contains("follows")) {
        r.fail(where + QStringLiteral(" cannot be of type \"any\" unless it has \"follows\"; only built-in nodes can."), {}, n);
        bad = true;
    } else {
        output.type.type = *parsed;
    }
    r.boolean(o, "list", output.type.list, where, {}, n);
    r.string(o, "label", output.label, where, {}, n);
    r.string(o, "description", output.description, where, {}, n);
    r.boolean(o, "optional", output.optional, where, {}, n);
    r.string(o, "role", output.role, where, {}, n);
    r.strings(o, "carries", output.carries, where);
    r.strings(o, "follows", output.follows, where);
    if (o.contains("follows") && (!parsed || !parsed->isAny() || output.follows.isEmpty()))
        r.fail(where + QStringLiteral(" has \"follows\" but is not of type \"any\", or names no input."), {}, n);
    readCondition(r, o, "carries_when", output.carriesWhen, where, {}, n);
    if (o.contains("carries_when") && output.carries.isEmpty())
        r.fail(where + QStringLiteral(" has \"carries_when\" but carries nothing."), {}, n);
    r.unknownFields(o, {"name", "type", "label", "description", "list", "optional", "role", "carries", "follows", "carries_when"},
                    where, {}, n);
    if (!output.role.isEmpty() && (output.role != QLatin1String(kMaskRole) || !parsed || parsed->id != "image"))
        r.fail(where + QStringLiteral(" has the role \"%1\"; the only role is \"mask\", on an image port.")
                           .arg(output.role),
               {}, n);

    if (bad || r.count() != before)
        return std::nullopt;
    return output;
}

void putIf(QJsonObject &o, const char *key, const QString &value) {
    if (!value.isEmpty())
        o.insert(QLatin1String(key), value);
}

void putIf(QJsonObject &o, const char *key, const std::optional<double> &value) {
    if (value)
        o.insert(QLatin1String(key), *value);
}

void putIf(QJsonObject &o, const char *key, const std::optional<int> &value) {
    if (value)
        o.insert(QLatin1String(key), *value);
}

void putIfTrue(QJsonObject &o, const char *key, bool value) {
    if (value)
        o.insert(QLatin1String(key), true);
}

QJsonObject inputJson(const InputSpec &input) {
    QJsonObject i{{"name", input.name}, {"type", input.type.type.toString()}};
    putIfTrue(i, "list", input.type.list);
    putIf(i, "label", input.label);
    putIf(i, "description", input.description);
    if (!input.defaultValue.isUndefined())
        i.insert("default", input.defaultValue);
    putIfTrue(i, "optional", input.optional);
    putIfTrue(i, "lazy", input.lazy);
    putIfTrue(i, "takes_list", input.type.takesList);
    putIf(i, "min_items", input.minItems);
    putIf(i, "max_items", input.maxItems);
    putIf(i, "min", input.min);
    putIf(i, "max", input.max);
    putIf(i, "step", input.step);
    putIfTrue(i, "integer", input.integer);
    if (input.choices)
        i.insert("choices", QJsonArray::fromStringList(*input.choices));
    putIf(i, "widget", input.widgetName);
    putIf(i, "syntax", input.syntax);
    putIf(i, "section", input.section);
    putIfTrue(i, "advanced", input.advanced);
    putIf(i, "role", input.role);
    putIf(i, "over", input.over);
    if (!input.shownWhen.isEmpty())
        i.insert("shown_when", input.shownWhen.raw);
    return i;
}

} // namespace

bool InputSpec::required() const {
    return !optional && defaultValue.isUndefined();
}

QString InputSpec::widget() const {
    if (!widgetName.isEmpty())
        return widgetName;
    const QString &id = type.type.id;
    if (id == "number")
        return QStringLiteral("spin");
    if (id == "text")
        return choices ? QStringLiteral("choice") : QStringLiteral("line");
    if (id == "boolean")
        return QStringLiteral("toggle");
    return {};
}

bool InputSpec::holdsInputFile() const {
    const QString w = widget();
    return w == QLatin1String("file") || w == QLatin1String("mask") || w == QLatin1String("paint");
}

const InputSpec *NodeType::input(const QString &name) const {
    for (const InputSpec &each : inputs)
        if (each.name == name)
            return &each;
    if (repeats.isEmpty())
        return nullptr;
    const std::optional<RowInput> at = rowInput(name);
    return at ? rowSpec(at->repeat, at->row, at->item) : nullptr;
}

QString NodeType::rowInputName(const QString &item, int row) { return item + QLatin1Char('_') + QString::number(row); }

std::optional<RowInput> NodeType::rowInput(const QString &name) const {
    const std::optional<std::pair<QString, int>> split = splitRowName(name);
    if (!split)
        return std::nullopt;
    for (int r = 0; r < repeats.size(); ++r) {
        const RepeatSpec &repeat = repeats.at(r);
        if (split->second > repeat.max)
            continue;
        for (int i = 0; i < repeat.items.size(); ++i)
            if (repeat.items.at(i).name == split->first)
                return RowInput{r, split->second, i};
    }
    return std::nullopt;
}

const InputSpec *NodeType::rowSpec(int repeat, int row, int item) const {
    if (repeat < 0 || repeat >= repeats.size())
        return nullptr;
    qsizetype offset = 0;
    for (int r = 0; r < repeat; ++r)
        offset += qsizetype(repeats.at(r).max) * repeats.at(r).items.size();
    const RepeatSpec &spec = repeats.at(repeat);
    if (row < 1 || row > spec.max || item < 0 || item >= spec.items.size())
        return nullptr;
    const qsizetype at = offset + qsizetype(row - 1) * spec.items.size() + item;
    return at < rowInputs.size() ? &rowInputs.at(at) : nullptr;
}

QList<int> NodeType::rowsOf(int repeat, const std::function<bool(const QString &)> &hasValue) const {
    QList<int> rows;
    if (repeat < 0 || repeat >= repeats.size())
        return rows;
    const RepeatSpec &spec = repeats.at(repeat);
    for (int row = 1; row <= spec.max; ++row)
        if (hasValue(rowInputName(spec.items.first().name, row)))
            rows << row;
    return rows;
}

QList<const InputSpec *> NodeType::inputsWhere(const std::function<bool(const QString &)> &hasValue) const {
    QList<const InputSpec *> list;
    list.reserve(inputs.size());
    const auto addRows = [&](int position) {
        for (int r = 0; r < repeats.size(); ++r) {
            if (repeats.at(r).position != position)
                continue;
            for (const int row : rowsOf(r, hasValue))
                for (int item = 0; item < repeats.at(r).items.size(); ++item)
                    if (const InputSpec *spec = rowSpec(r, row, item))
                        list << spec;
        }
    };
    for (int i = 0; i < inputs.size(); ++i) {
        if (!repeats.isEmpty())
            addRows(i);
        list << &inputs.at(i);
    }
    if (!repeats.isEmpty())
        addRows(int(inputs.size()));
    return list;
}

const OutputSpec *NodeType::output(const QString &name) const {
    for (const OutputSpec &each : outputs)
        if (each.name == name)
            return &each;
    return nullptr;
}

namespace {

std::optional<NodeType> readManifest(const QJsonObject &manifest, Errors &errors) {
    NodeType type;
    const QJsonValue id = manifest.value("id");
    Reader r(id.isString() && !id.toString().isEmpty() ? id.toString() : QStringLiteral("manifest"), errors);

    const QJsonValue format = manifest.value("manifest");
    if (format.isDouble() && format.toDouble() > 1 && format.toDouble() == std::trunc(format.toDouble())) {
        r.add(QStringLiteral("unsupported_format_version"), Error::Severity::Error,
              QStringLiteral("manifest format %1 is newer than this Fugue supports (1).").arg(shown(format)));
        return std::nullopt;
    }
    if (format != QJsonValue(1))
        r.fail(QStringLiteral("\"manifest\" must be 1, not %1.").arg(shown(format)));

    type.id = id.toString();
    if (!isTypeId(type.id))
        r.fail(QStringLiteral("\"id\" must be \"<pack id>/<name>\", not %1.").arg(shown(id)));

    const QJsonValue version = manifest.value("version");
    if (const std::optional<Version> parsed = Version::parse(version.toString()))
        type.version = *parsed;
    else
        r.fail(QStringLiteral("\"version\" must be a semver version such as \"1.0.0\", not %1.").arg(shown(version)));

    const QJsonValue name = manifest.value("name");
    if (!name.isString() || name.toString().isEmpty())
        r.fail(QStringLiteral("\"name\" must be a display name."));
    type.name = name.toString();

    const QString where = QStringLiteral("the manifest");
    r.string(manifest, "description", type.description, where);
    r.string(manifest, "category", type.category, where);
    r.strings(manifest, "keywords", type.keywords, where);
    r.strings(manifest, "aliases", type.aliases, where);
    for (const QString &alias : std::as_const(type.aliases))
        if (!isTypeId(alias))
            r.fail(QStringLiteral("alias \"%1\" is not a node type id.").arg(alias));
    r.strings(manifest, "retired_inputs", type.retiredInputs, where);
    r.boolean(manifest, "sink", type.sink, where);
    r.boolean(manifest, "view_only", type.viewOnly, where);
    if (type.viewOnly && !type.sink)
        r.fail(QStringLiteral("\"view_only\" needs \"sink\": true."));
    r.boolean(manifest, "deterministic", type.deterministic, where);
    r.string(manifest, "picture", type.picture, where);
    if (!type.picture.isEmpty() && type.picture != QLatin1String("always") && type.picture != QLatin1String("result")
        && type.picture != QLatin1String("never"))
        r.fail(QStringLiteral("\"picture\" must be \"always\", \"result\" or \"never\", not %1.").arg(shown(manifest.value("picture"))));

    static const QStringList kinds{"command", "group", "native", "script", "builtin"};
    const QJsonValue runtime = manifest.value("runtime");
    type.runtime = runtime.toObject();
    if (!runtime.isObject() || !kinds.contains(type.runtime.value("kind").toString()))
        r.fail(QStringLiteral("\"runtime\" must be an object whose \"kind\" is one of %1.")
                   .arg(kinds.join(QStringLiteral(", "))));

    const QJsonValue needs = manifest.value("needs");
    if (!needs.isUndefined() && !needs.isObject())
        r.fail(QStringLiteral("\"needs\" must be an object."));
    type.needs = needs.toObject();
    type.capabilities = manifest.value("capabilities");
    if (manifest.contains("ui"))
        r.fail(QStringLiteral("\"ui\" is reserved and must be absent in this version."));

    r.unknownFields(manifest,
                    {"manifest", "id", "version", "name", "description", "category", "keywords", "aliases", "retired_inputs",
                     "sink", "view_only", "deterministic", "picture", "runtime", "inputs", "outputs", "needs", "capabilities", "ui"},
                    where);

    QSet<QString> seen;
    const QJsonValue inputs = manifest.value("inputs");
    if (!inputs.isArray())
        r.fail(QStringLiteral("\"inputs\" must be a list (it may be empty)."));
    const QJsonArray inputList = inputs.toArray();
    const auto declare = [&](const QString &inputName) {
        if (isPortName(inputName) && seen.contains(inputName))
            r.fail(QStringLiteral("input %1 is declared more than once.").arg(inputName), inputName);
        seen.insert(inputName);
    };
    for (qsizetype i = 0; i < inputList.size(); ++i) {
        if (inputList.at(i).isObject() && inputList.at(i).toObject().contains("repeat")) {
            const std::optional<RepeatSpec> repeat = readRepeat(r, inputList.at(i).toObject(), int(type.inputs.size()));
            if (!repeat)
                continue;
            declare(repeat->name);
            for (const InputSpec &item : repeat->items)
                declare(item.name);
            type.repeats << *repeat;
            continue;
        }
        const std::optional<InputSpec> input = readInput(r, inputList.at(i), int(i));
        declare(inputList.at(i).toObject().value("name").toString());
        if (input)
            type.inputs << *input;
    }
    // A declared input named like a row's input (`lora_2`) would be two inputs at once.
    for (const RepeatSpec &repeat : std::as_const(type.repeats))
        for (const InputSpec &item : repeat.items)
            for (const InputSpec &input : std::as_const(type.inputs))
                if (const auto split = splitRowName(input.name); split && split->first == item.name)
                    r.fail(QStringLiteral("input %1 has the name of a row of the repeating input %2.").arg(input.name, item.name),
                           input.name);
    for (int i = 0; i < type.repeats.size(); ++i) {
        const RepeatSpec &repeat = type.repeats.at(i);
        for (int row = 1; row <= repeat.max; ++row)
            for (int item = 0; item < repeat.items.size(); ++item) {
                InputSpec spec = repeat.items.at(item);
                spec.name = NodeType::rowInputName(spec.name, row);
                const QString label = spec.label.isEmpty() ? repeat.items.at(item).name : spec.label;
                spec.label = item == 0 ? QStringLiteral("%1 %2").arg(label).arg(row) : label;
                type.rowInputs << spec;
            }
    }

    // A mask is painted over an image input of the same node.
    for (const InputSpec &input : std::as_const(type.inputs)) {
        if (input.over.isEmpty())
            continue;
        const InputSpec *over = nullptr;
        for (const InputSpec &other : std::as_const(type.inputs))
            if (other.name == input.over)
                over = &other;
        if (!over || over->type.type.id != QLatin1String("image") || over->type.list)
            r.fail(QStringLiteral("input %1 is painted \"over\" %2, which is not an image input of this node.")
                       .arg(input.name, input.over),
                   input.name);
    }

    // A condition reads other inputs of the same node, never its own row.
    for (const InputSpec &input : std::as_const(type.inputs))
        for (const QString &name : input.shownWhen.inputs()) {
            const InputSpec *other = nullptr;
            for (const InputSpec &candidate : std::as_const(type.inputs))
                if (candidate.name == name)
                    other = &candidate;
            if (!other || other == &input)
                r.fail(QStringLiteral("input %1 has a \"shown_when\" that reads %2, which is not another input of this node.")
                           .arg(input.name, name),
                       input.name);
        }

    seen.clear();
    const QJsonValue outputs = manifest.value("outputs");
    if (!outputs.isArray())
        r.fail(QStringLiteral("\"outputs\" must be a list (it may be empty)."));
    const QJsonArray outputList = outputs.toArray();
    for (qsizetype i = 0; i < outputList.size(); ++i) {
        const std::optional<OutputSpec> output = readOutput(r, outputList.at(i), int(i));
        const QString outputName = outputList.at(i).toObject().value("name").toString();
        if (isPortName(outputName) && seen.contains(outputName))
            r.fail(QStringLiteral("output %1 is declared more than once.").arg(outputName), {}, outputName);
        seen.insert(outputName);
        if (output)
            type.outputs << *output;
    }

    for (const OutputSpec &output : std::as_const(type.outputs))
        for (const QString &name : output.carriesWhen.inputs())
            if (!type.input(name))
                r.fail(QStringLiteral("output %1 has a \"carries_when\" that reads %2, which is not an input of this node.")
                           .arg(output.name, name),
                       {}, output.name);

    if (r.failed())
        return std::nullopt;
    return type;
}

} // namespace

Result<NodeType> NodeType::fromJson(const QJsonObject &manifest) {
    Errors problems;
    std::optional<NodeType> type = readManifest(manifest, problems);
    return collected(std::move(type), std::move(problems));
}

QJsonObject NodeType::toJson() const {
    QJsonObject o{{"manifest", 1}, {"id", id}, {"version", version.toString()}, {"name", name},
                  {"runtime", runtime}};
    putIf(o, "description", description);
    putIf(o, "category", category);
    if (!keywords.isEmpty())
        o.insert("keywords", QJsonArray::fromStringList(keywords));
    if (!aliases.isEmpty())
        o.insert("aliases", QJsonArray::fromStringList(aliases));
    if (!retiredInputs.isEmpty())
        o.insert("retired_inputs", QJsonArray::fromStringList(retiredInputs));
    putIfTrue(o, "sink", sink);
    putIfTrue(o, "view_only", viewOnly);
    putIf(o, "picture", picture);
    if (!deterministic)
        o.insert("deterministic", false);
    if (!needs.isEmpty())
        o.insert("needs", needs);
    if (!capabilities.isUndefined())
        o.insert("capabilities", capabilities);

    QJsonArray inputList;
    const auto addRepeats = [&](qsizetype position) {
        for (const RepeatSpec &repeat : repeats) {
            if (repeat.position != position)
                continue;
            QJsonObject block{{"repeat", repeat.name}};
            putIf(block, "label", repeat.label);
            if (repeat.max != kMaxRepeatRows)
                block.insert("max", repeat.max);
            QJsonArray items;
            for (const InputSpec &item : repeat.items)
                items << inputJson(item);
            block.insert("inputs", items);
            inputList << block;
        }
    };
    for (qsizetype at = 0; at < inputs.size(); ++at) {
        addRepeats(at);
        inputList << inputJson(inputs.at(at));
    }
    addRepeats(inputs.size());
    o.insert("inputs", inputList);

    QJsonArray outputList;
    for (const OutputSpec &output : outputs) {
        QJsonObject out{{"name", output.name}, {"type", output.type.type.toString()}};
        putIfTrue(out, "list", output.type.list);
        putIf(out, "label", output.label);
        putIf(out, "description", output.description);
        putIfTrue(out, "optional", output.optional);
        putIf(out, "role", output.role);
        if (!output.follows.isEmpty())
            out.insert("follows", QJsonArray::fromStringList(output.follows));
        if (!output.carries.isEmpty())
            out.insert("carries", QJsonArray::fromStringList(output.carries));
        if (!output.carriesWhen.isEmpty())
            out.insert("carries_when", output.carriesWhen.raw);
        outputList << out;
    }
    o.insert("outputs", outputList);
    return o;
}

Errors checkLiteral(const InputSpec &input, const QJsonValue &value, PortKind kind) {
    const QString type = input.type.type.toString();
    const auto mismatch = [&](const QString &message) {
        return Errors{literalError(QStringLiteral("type_mismatch"), input, message)};
    };

    if (value.isObject() && value.toObject().contains("auto")) {
        if (value.toObject().value("auto") == QJsonValue("random") && input.widget() == "seed")
            return {};
        return mismatch(QStringLiteral("%1 is only for inputs with the seed widget, and %2 has none.")
                            .arg(shown(value), input.name));
    }

    switch (kind) {
    case PortKind::Any:
        return {};
    case PortKind::Resource: {
        const std::optional<PortType> handleType =
            PortType::parse(value.toObject().value("resource").toString());
        if (value.isObject() && handleType && *handleType == input.type.type)
            return {};
        return mismatch(QStringLiteral("Input %1 takes a link or a %2 resource handle, not %3.")
                            .arg(input.name, type, shown(value)));
    }
    case PortKind::Data:
        return mismatch(QStringLiteral("Input %1 is %2 and takes a link only, not a value.").arg(input.name, type));
    case PortKind::Unknown:
        return mismatch(QStringLiteral("Input %1 has the unknown type %2.").arg(input.name, type));
    case PortKind::Builtin:
        break;
    }

    const QString &id = input.type.type.id;
    if (id == "text") {
        if (!value.isString())
            return mismatch(QStringLiteral("Input %1 takes text, not %2.").arg(input.name, shown(value)));
        if (input.choices && !input.choices->contains(value.toString())) {
            Error error = literalError(QStringLiteral("value_not_in_choices"), input,
                                       QStringLiteral("%1 is not a valid %2.").arg(shown(value), input.name));
            error.valid = QJsonArray::fromStringList(*input.choices);
            const QString near = nearest(value.toString(), *input.choices);
            if (!near.isEmpty())
                error.hint = QStringLiteral("Did you mean \"%1\"?").arg(near);
            return {error};
        }
        static const QRegularExpression color(QStringLiteral("^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$"));
        if (input.widget() == QLatin1String("color") && !color.match(value.toString()).hasMatch())
            return mismatch(QStringLiteral("Input %1 takes a color as #rrggbb or #rrggbbaa, not %2.")
                                .arg(input.name, shown(value)));
        return {};
    }
    if (id == "boolean") {
        if (!value.isBool())
            return mismatch(QStringLiteral("Input %1 takes true or false, not %2.").arg(input.name, shown(value)));
        return {};
    }
    if (id == "number") {
        if (!value.isDouble())
            return mismatch(QStringLiteral("Input %1 takes a number, not %2.").arg(input.name, shown(value)));
        const double d = value.toDouble();
        const auto outOfRange = [&](const QString &message) {
            return Errors{literalError(QStringLiteral("value_out_of_range"), input, message)};
        };
        if (input.integer && d != std::trunc(d))
            return outOfRange(QStringLiteral("Input %1 takes a whole number, not %2.").arg(input.name, shown(value)));
        if (input.integer && std::abs(d) > maxSafeInteger)
            return outOfRange(QStringLiteral("%1 is too large for input %2; whole numbers must lie within "
                                             "plus or minus 2^53.")
                                  .arg(shown(value), input.name));
        if (input.min && d < *input.min)
            return outOfRange(QStringLiteral("%1 is below the minimum of %2 for input %3.")
                                  .arg(shown(value), shown(*input.min), input.name));
        if (input.max && d > *input.max)
            return outOfRange(QStringLiteral("%1 is above the maximum of %2 for input %3.")
                                  .arg(shown(value), shown(*input.max), input.name));
        return {};
    }
    return mismatch(QStringLiteral("Input %1 is %2 and takes a link only, not a value.").arg(input.name, type));
}

Errors checkItemCount(const InputSpec &input, qsizetype count) {
    Errors errors;
    if (input.minItems && count < *input.minItems)
        errors.append(literalError(QStringLiteral("value_out_of_range"), input,
                                   QStringLiteral("Input %1 takes at least %2 items, not %3.")
                                       .arg(input.name)
                                       .arg(*input.minItems)
                                       .arg(count)));
    if (input.maxItems && count > *input.maxItems)
        errors.append(literalError(QStringLiteral("value_out_of_range"), input,
                                   QStringLiteral("Input %1 takes at most %2 items, not %3.")
                                       .arg(input.name)
                                       .arg(*input.maxItems)
                                       .arg(count)));
    return errors;
}

} // namespace fugue
