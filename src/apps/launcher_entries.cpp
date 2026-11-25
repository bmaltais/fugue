#include "apps/launcher_entries.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace fugue {

namespace {

QString safe(const QString &text) {
    QString out = text;
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    out.remove(QRegularExpression(QStringLiteral("^[-.]+|-+$")));
    return out.isEmpty() ? QStringLiteral("app") : out;
}

QString keyOf(const QString &ref) {
    if (!ref.contains(QLatin1Char('/')))
        return safe(ref);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(ref.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return safe(QFileInfo(ref).completeBaseName()) + QLatin1Char('-') + hash;
}

// A value of the entry: one line, a backslash doubled (the spec's escape for a string).
QString value(QString text) {
    text.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    return text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
}

} // namespace

LauncherEntries::LauncherEntries(QString dataHome, QString program)
    : m_dataHome(dataHome.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) : std::move(dataHome)),
      m_program(std::move(program)) {
    if (m_program.isEmpty()) {
        const QString appImage = QProcessEnvironment::systemEnvironment().value(QStringLiteral("APPIMAGE"));
        m_program = appImage.isEmpty() ? QStringLiteral("fugue") : appImage;
    }
}

QString LauncherEntries::desktopName(const QString &ref) { return QStringLiteral("fugue-app-") + keyOf(ref); }

QString LauncherEntries::entryPath(const QString &ref) const {
    return m_dataHome + QStringLiteral("/applications/") + desktopName(ref) + QStringLiteral(".desktop");
}

QString LauncherEntries::iconPath(const QString &ref, const QString &suffix) const {
    return m_dataHome + QStringLiteral("/fugue/app-icons/") + desktopName(ref) + QLatin1Char('.') + suffix;
}

bool LauncherEntries::has(const QString &ref) const { return QFileInfo::exists(entryPath(ref)); }

QString LauncherEntries::execLine(const QString &program, const QString &ref) {
    const auto quote = [](QString arg) {
        arg.replace(QLatin1Char('%'), QStringLiteral("%%"));
        static const QRegularExpression special(QStringLiteral("[\\s\"'\\\\<>~|&;$*?#()`]"));
        if (arg.isEmpty() || arg.contains(special)) {
            QString escaped;
            for (const QChar c : std::as_const(arg)) {
                if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\'))
                    escaped += QLatin1Char('\\');
                escaped += c;
            }
            arg = QLatin1Char('"') + escaped + QLatin1Char('"');
        }
        return arg;
    };
    return quote(program) + QStringLiteral(" --app ") + quote(ref);
}

QString LauncherEntries::add(const LauncherApp &app) const {
    if (app.ref.isEmpty())
        return QStringLiteral("This app has no name to start it by.");
    remove(app.ref); // the picture may change its file type
    QString icon = QStringLiteral("fugue");
    const QFileInfo picture(app.icon);
    if (!app.icon.isEmpty() && picture.isFile()) {
        const QString target = iconPath(app.ref, picture.suffix().toLower());
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (QFile::copy(app.icon, target))
            icon = target;
    }
    const QString name = desktopName(app.ref);
    QString text = QStringLiteral("[Desktop Entry]\nType=Application\nName=") + value(app.name.isEmpty() ? app.ref : app.name) + QLatin1Char('\n');
    if (!app.comment.isEmpty())
        text += QStringLiteral("Comment=") + value(app.comment) + QLatin1Char('\n');
    text += QStringLiteral("Exec=") + value(execLine(m_program, app.ref)) + QLatin1Char('\n');
    text += QStringLiteral("Icon=") + value(icon) + QLatin1Char('\n');
    text += QStringLiteral("Terminal=false\nCategories=Graphics;\nStartupWMClass=") + name + QStringLiteral("\nX-Fugue-App=true\n");
    const QString path = entryPath(app.ref);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit())
        return QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
    return {};
}

QString LauncherEntries::remove(const QString &ref) const {
    QString problem;
    const QString path = entryPath(ref);
    if (QFileInfo::exists(path) && !QFile::remove(path))
        problem = QStringLiteral("Could not remove %1.").arg(path);
    for (const QString &suffix : {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("webp")})
        if (const QString icon = iconPath(ref, suffix); QFileInfo::exists(icon))
            QFile::remove(icon);
    return problem;
}

} // namespace fugue
