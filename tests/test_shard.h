// FUGUE_TEST_MAIN(TestClass, QGuiApplication) is QTEST_MAIN(TestClass) that can run a part of the class.
// With FUGUE_TEST_SHARD=i/n in the environment (CMakeLists.txt sets it for the long tests, one ctest entry per
// i) it runs every n-th test function, counting from the i-th, in the order the class declares them, so the
// shards together run every function once. Without it, or when the command line names functions, it is the
// plain QTEST_MAIN. Each shard runs initTestCase and cleanupTestCase of its own.
#pragma once

#include <QMetaMethod>
#include <QObject>
#include <QStringList>
#include <QtTest>

#include <vector>

namespace fugue_test {

inline int shardedExec(QObject *test, int argc, char **argv) {
    const QByteArray spec = qgetenv("FUGUE_TEST_SHARD");
    if (spec.isEmpty() || argc > 1)
        return QTest::qExec(test, argc, argv);
    const QList<QByteArray> parts = spec.split('/');
    bool okIndex = false, okCount = false;
    const int index = parts.size() == 2 ? parts[0].toInt(&okIndex) : 0;
    const int count = parts.size() == 2 ? parts[1].toInt(&okCount) : 0;
    if (!okIndex || !okCount || index < 1 || index > count)
        qFatal("FUGUE_TEST_SHARD must be i/n with 1 <= i <= n, got '%s'", spec.constData());

    const QMetaObject *meta = test->metaObject();
    QList<QByteArray> chosen;
    int position = 0;
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        const QByteArray name = method.name();
        if (method.methodType() != QMetaMethod::Slot || method.access() != QMetaMethod::Private || method.parameterCount() != 0
            || method.returnType() != QMetaType::Void)
            continue;
        if (name == "initTestCase" || name == "cleanupTestCase" || name == "init" || name == "cleanup" || name.endsWith("_data"))
            continue;
        if (position++ % count == index - 1)
            chosen.append(name);
    }
    if (chosen.isEmpty()) {
        qInfo("shard %d/%d has no test function", index, count);
        return 0;
    }
    std::vector<char *> args{argv[0]};
    for (QByteArray &name : chosen)
        args.push_back(name.data());
    return QTest::qExec(test, int(args.size()), args.data());
}

} // namespace fugue_test

#define FUGUE_TEST_MAIN(TestObject, AppClass)                            \
    int main(int argc, char *argv[]) {                                   \
        AppClass app(argc, argv);                                        \
        app.setAttribute(Qt::AA_Use96Dpi, true);                         \
        TestObject tc;                                                   \
        QTEST_SET_MAIN_SOURCE_PATH                                       \
        return fugue_test::shardedExec(&tc, argc, argv);                 \
    }
