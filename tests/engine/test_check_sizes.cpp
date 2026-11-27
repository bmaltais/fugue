// Sizes known before a run (#601): the dry check asks each node's kernel for the size of its pictures
// (NodeKernel::outputSizes), hands a node the sizes of what is linked to it (NodeContext::inputSizes), and returns them
// in GraphCheck::sizes. The same sizes reach a kernel's check(), which is where a size out of range is an early error.
//
// Seam: checkGraph() on fake kernels (scheduler_support.h) with a size rule of their own; the real nodes' rules are in
// tests/nodes/test_predicted_sizes.cpp. Nothing runs and nothing sleeps.

#include <QtTest>

#include <vips/vips8>

#include "engine/media.h"
#include "engine/vips.h"
#include "scheduler_support.h"

using namespace fugue;
using namespace fugue::test;

namespace {

QJsonObject node(const QString &type, const QJsonObject &inputs) { return QJsonObject{{"type", type}, {"inputs", inputs}}; }
QJsonObject from(const QString &port) { return QJsonObject{{"from", port}}; }

// A source of a size written in the node (w and h; none when either is left out), and a node that halves its input's size.
class SizeKernel : public NodeKernel {
public:
    explicit SizeKernel(bool halves) : m_halves(halves) {}

    Result<Values> run(const NodeContext &) override { return Values{{"pic", Value(QStringLiteral("made"))}}; }

    PictureSizes outputSizes(const NodeContext &context) const override {
        if (!m_halves) {
            if (context.input("w").isEmpty() || context.input("h").isEmpty())
                return {};
            return {{"pic", QSize(int(context.input("w").number()), int(context.input("h").number()))}};
        }
        const auto in = context.inputSizes.constFind("in");
        return in == context.inputSizes.constEnd() ? PictureSizes() : PictureSizes{{"pic", QSize(in->width() / 2, in->height() / 2)}};
    }

    // A picture wider than 100 is out of range, as a node's own limit is: said before a run, naming the input.
    Errors check(const NodeContext &context) const override {
        const auto in = context.inputSizes.constFind("in");
        if (m_halves && in != context.inputSizes.constEnd() && in->width() > 100)
            return {makeInputError("value_out_of_range", "in", QStringLiteral("%1 is wider than 100.").arg(in->width()))};
        return {};
    }

private:
    const bool m_halves;
};

// Makes a real picture of 10 x 6 and says it will be 99 x 99: once it is cached, what is downstream reads the real size.
class PhotoKernel : public NodeKernel {
public:
    explicit PhotoKernel(World *world) : m_world(world) {}
    Result<Values> run(const NodeContext &context) override {
        const std::vector<unsigned char> pixels(10 * 6 * 3, 128);
        const vips::VImage image = vips::VImage::new_from_memory_copy(pixels.data(), pixels.size(), 10, 6, 3, VIPS_FORMAT_UCHAR);
        const Result<MediaHandle> handle = MediaHandle::fromImage(image);
        if (!handle)
            return handle.errors();
        const Result<MediaHandle> saved = handle->saved(context.workDir, "photo", *m_world->hashes);
        if (!saved)
            return saved.errors();
        return Values{{"pic", Value(saved.value())}};
    }
    PictureSizes outputSizes(const NodeContext &) const override { return {{"pic", QSize(99, 99)}}; }

private:
    World *m_world;
};

void addTypes(World &world) {
    world.type("fugue/photo", "'inputs': [], 'outputs': [{'name': 'pic', 'type': 'image'}]", nullptr);
    world.kernels.add("fugue/photo", std::make_shared<PhotoKernel>(&world));
    world.type("fugue/pic",
               "'inputs': [{'name': 'w', 'type': 'number', 'optional': true}, {'name': 'h', 'type': 'number', 'optional': true}],"
               "'outputs': [{'name': 'pic', 'type': 'text'}]",
               nullptr);
    world.type("fugue/halve", "'inputs': [{'name': 'in', 'type': 'any'}], 'outputs': [{'name': 'pic', 'type': 'text'}]", nullptr);
    world.kernels.add("fugue/pic", std::make_shared<SizeKernel>(false));
    world.kernels.add("fugue/halve", std::make_shared<SizeKernel>(true));
}

QJsonObject chain(int width, int height) {
    return QJsonObject{{"a", node("fugue/pic", {{"w", width}, {"h", height}})},
                       {"b", node("fugue/halve", {{"in", from("a.pic")}})},
                       {"c", node("fugue/halve", {{"in", from("b.pic")}})},
                       {"s", node("fugue/save", {{"value", from("c.pic")}})}};
}

} // namespace

class TestCheckSizes : public QObject {
    Q_OBJECT

private slots:
    // A size flows through the links of nodes that have not run: each kernel is given the size before it.
    void aSizeFlowsThroughStaleNodes() {
        World world;
        addTypes(world);
        const GraphCheck check = checkGraph(world.graph(chain(80, 60)), world.environment());
        QCOMPARE(check.states.value("c"), NodeState::Stale);
        QCOMPARE(check.sizes.value("a").value("pic"), QSize(80, 60));
        QCOMPARE(check.sizes.value("b").value("pic"), QSize(40, 30));
        QCOMPARE(check.sizes.value("c").value("pic"), QSize(20, 15));
        QVERIFY(!check.sizes.contains("s")); // the default kernel knows none
        QCOMPARE(world.calls.total(), 0);    // asking ran nothing
    }

    // A source with no size written has none, and nothing downstream is guessed.
    void anUnknownSizeStaysUnknownDownstream() {
        World world;
        addTypes(world);
        QJsonObject nodes = chain(80, 60);
        nodes.insert("a", node("fugue/pic", {{"w", 80}}));
        const GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QVERIFY(check.sizes.isEmpty());
    }

    // The size a kernel is told is also in its check(): a size out of range is the node's error, naming its input, with no run.
    void aSizeOutOfRangeIsAnEarlyErrorOnTheNodeThatMeetsIt() {
        World world;
        addTypes(world);
        const GraphCheck check = checkGraph(world.graph(chain(300, 60)), world.environment());
        QCOMPARE(check.checkErrors.size(), 2); // b meets 300; c meets 150, also over 100
        QStringList where;
        for (const Error &error : check.checkErrors)
            where << error.node + ":" + error.input + ":" + error.code;
        QCOMPARE(where, (QStringList{"b:in:value_out_of_range", "c:in:value_out_of_range"}));
    }

    // What a cached node made is the truth: the size downstream is worked from the real picture, not from the prediction.
    void aCachedPicturesRealSizeBeatsItsPrediction() {
        World world;
        initVips();
        addTypes(world);
        const QJsonObject nodes{{"p", node("fugue/photo", {})},
                                {"h", node("fugue/halve", {{"in", from("p.pic")}})},
                                {"s", node("fugue/save", {{"value", from("h.pic")}})}};
        GraphCheck check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.sizes.value("h").value("pic"), QSize(49, 49)); // from the prediction while nothing is cached
        world.run(world.graph(nodes));
        check = checkGraph(world.graph(nodes), world.environment());
        QCOMPARE(check.states.value("p"), NodeState::Cached);
        QCOMPARE(check.sizes.value("h").value("pic"), QSize(5, 3)); // 10 x 6 halved
    }

    // After a run the nodes are cached; the sizes are still there, and a change of the source's size moves every one after it.
    void sizesFollowAnEditAndSurviveACachedRun() {
        World world;
        addTypes(world);
        world.run(world.graph(chain(80, 60)));
        GraphCheck check = checkGraph(world.graph(chain(80, 60)), world.environment());
        QCOMPARE(check.states.value("b"), NodeState::Cached);
        QCOMPARE(check.sizes.value("c").value("pic"), QSize(20, 15));
        check = checkGraph(world.graph(chain(40, 20)), world.environment());
        QCOMPARE(check.states.value("a"), NodeState::Stale);
        QCOMPARE(check.sizes.value("c").value("pic"), QSize(10, 5));
    }
};

QTEST_GUILESS_MAIN(TestCheckSizes)
#include "test_check_sizes.moc"
