// The engine's reason for a component that cannot load (helper/src/engine_error.h, #542): the helper
// appends the last error line of the engine's log to the generic message, so a missing tokenizer is
// not shown as "cannot load component" alone.

#include <QtTest>

#include <string>

#include "../../helper/src/engine_error.h"

using namespace fugue_helper;

class TestHelperEngineError : public QObject {
    Q_OBJECT

private slots:
    void aLogLineIsReducedToItsReason() {
        QCOMPARE(engineLogReason("failed to build model runners: MOSS-Image requires a tokenizer --- component_builders.cpp:60\n"),
                 std::string("failed to build model runners: MOSS-Image requires a tokenizer"));
        QCOMPARE(engineLogReason("no suffix at all\r\n"), std::string("no suffix at all"));
        QVERIFY(engineLogReason("\n").empty());
    }

    void aGenericLoadFailureGetsTheEngineReason() {
        const std::string generic = "line 38, column 17: cannot load component from '/m/qwen.safetensors'";
        QCOMPARE(withEngineReason(generic, "MOSS-Image requires the official Qwen3.5 tokenizer.json"),
                 generic + " (the engine said: MOSS-Image requires the official Qwen3.5 tokenizer.json)");
    }

    void otherMessagesAndRepeatsAreLeftAlone() {
        QCOMPARE(withEngineReason("out of memory", "some earlier error"), std::string("out of memory"));
        QCOMPARE(withEngineReason("cannot load component from 'x'", ""), std::string("cannot load component from 'x'"));
        const std::string already = "cannot load component from 'x': tokenizer is missing";
        QCOMPARE(withEngineReason(already, "tokenizer is missing"), already);
    }
};

QTEST_GUILESS_MAIN(TestHelperEngineError)
#include "test_helper_engine_error.moc"
