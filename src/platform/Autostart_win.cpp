#include "platform/Autostart.h"
#include "BuildChannel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

namespace Platform {

namespace {
QString runKey()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
}
QString approvedKey()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run");
}

bool owns(const QVariant& value, const QString& exe)
{
    if (value.metaType().id() != QMetaType::QString)
        return false;
    QString path = value.toString().trimmed();
    if (path.startsWith(u'"')) {
        static const QRegularExpression quoted(QStringLiteral("^\"([^\"]+)\"(?:\\s+--autostart)?$"));
        const auto match = quoted.match(path);
        if (!match.hasMatch())
            return false;
        path = match.captured(1);
    }
    const QFileInfo target(QDir::fromNativeSeparators(path));
    const QString normalized = target.isAbsolute() ? target.canonicalFilePath() : QString();
    return !normalized.isEmpty()
        && normalized.compare(QFileInfo(exe).canonicalFilePath(), Qt::CaseInsensitive) == 0;
}

bool allowedByWindows(QSettings& approved, const QString& name)
{
    const auto value = approved.value(name);
    const auto state = value.toByteArray();
    // 系统已禁用或状态不明时，避免因改名重开启动项。
    return !value.isValid() || (state.size() == 12 && (state[0] == 2 || state[0] == 6)
        && state[1] == 0 && state[2] == 0 && state[3] == 0);
}

enum class Mode { Enable, Disable, SetupSelected, SetupUnselected };

bool apply(QSettings& run, QSettings& approved, const QString& exe, Mode mode, QString* error,
           bool dev = BuildChannel::development)
{
    const auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    run.sync();
    const auto name = BuildChannel::appId(dev);
    const auto current = run.value(name);
    const auto legacy = dev ? QVariant() : run.value("ltool");
    const bool ourLegacy = owns(legacy, exe);
    if (current.isValid() && !owns(current, exe))
        return fail(QStringLiteral("%1 登录项已被其他目标使用，未改动。").arg(name));
    const auto removeUnchanged = [&](const QString& name, const QVariant& previous) {
        run.sync();
        if (run.value(name) != previous)
            return false;
        run.remove(name);
        run.sync();
        return run.status() == QSettings::NoError && !run.contains(name);
    };
    if (mode == Mode::Disable) {
        if (current.isValid() && !removeUnchanged(name, current))
            return fail(QStringLiteral("无法关闭 %1 登录项。").arg(name));
        if (ourLegacy && !removeUnchanged("ltool", legacy))
            return fail(QStringLiteral("旧登录项已变化或无法删除。"));
        return true;
    }
    if (mode != Mode::Enable) {
        if (!current.isValid() && !ourLegacy && mode == Mode::SetupUnselected)
            return true;
        if (!allowedByWindows(approved, name)
            || (!current.isValid() && ourLegacy && !allowedByWindows(approved, "ltool")))
            return true;
    } else if (!allowedByWindows(approved, name)) {
        return fail(QStringLiteral("Windows 已禁用 %1 登录项，请在系统「启动应用」中启用。").arg(name));
    }
    const auto entry = autostartEntry(exe);
    if (entry.size() > 260)
        return fail(QStringLiteral("登录启动命令超过 Windows 的 260 字符限制。"));
    run.sync();
    if (run.value(name) != current)
        return fail(QStringLiteral("%1 登录项已变化，未改动。").arg(name));
    run.setValue(name, entry);
    run.sync();
    if (run.status() != QSettings::NoError || run.value(name) != entry)
        return fail(QStringLiteral("无法验证新的 Hotaru 登录项，已保留旧值。"));
    if (ourLegacy && !removeUnchanged("ltool", legacy))
        return fail(QStringLiteral("新登录项已写入，旧值已变化或无法删除。"));
    return true;
}
} // namespace

QString autostartTarget() { return runKey() + QLatin1Char('\\') + BuildChannel::appId(); }

QString autostartEntry(const QString& exePath)
{
    return QStringLiteral("\"%1\" --autostart").arg(QDir::toNativeSeparators(exePath));
}

void setAutostart(bool on)
{
    setAutostart(on, nullptr);
}

bool setAutostart(bool on, QString* error)
{
    QSettings run(runKey(), QSettings::NativeFormat), approved(approvedKey(), QSettings::NativeFormat);
    return apply(run, approved, QCoreApplication::applicationFilePath(), on ? Mode::Enable : Mode::Disable, error);
}

bool setupAutostart(bool selected, QString* error)
{
    QSettings run(runKey(), QSettings::NativeFormat), approved(approvedKey(), QSettings::NativeFormat);
    return apply(run, approved, QCoreApplication::applicationFilePath(),
                 selected ? Mode::SetupSelected : Mode::SetupUnselected, error);
}

bool autostartEnabled()
{
    QSettings run(runKey(), QSettings::NativeFormat), approved(approvedKey(), QSettings::NativeFormat);
    const QString exe = QCoreApplication::applicationFilePath();
    const auto name = BuildChannel::appId();
    return run.contains(name) ? owns(run.value(name), exe) && allowedByWindows(approved, name)
        : !BuildChannel::development && owns(run.value("ltool"), exe) && allowedByWindows(approved, "ltool");
}

bool autostartSelftest()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return false;
    QSettings run(dir.filePath("run.ini"), QSettings::IniFormat);
    QSettings approved(dir.filePath("approved.ini"), QSettings::IniFormat);
    const auto exe = QCoreApplication::applicationFilePath();
    const auto old = QDir::toNativeSeparators(exe);
    if (autostartEntry("C:/Program Files/Hotaru/Hotaru.exe")
        != QStringLiteral("\"C:\\Program Files\\Hotaru\\Hotaru.exe\" --autostart"))
        return false;
    run.setValue("ltool", old);
    approved.setValue("ltool", QByteArray(12, '\0'));
    if (!apply(run, approved, exe, Mode::SetupSelected, nullptr, false) || run.contains("Hotaru"))
        return false;
    approved.remove("ltool");
    run.setValue("Hotaru", QStringLiteral("C:\\other.exe"));
    if (apply(run, approved, exe, Mode::Enable, nullptr, false) || run.value("ltool") != old)
        return false;
    run.remove("Hotaru");
    if (!apply(run, approved, exe, Mode::SetupUnselected, nullptr, false)
        || run.value("Hotaru") != autostartEntry(exe) || run.contains("ltool"))
        return false;
    run.setValue("ltool", QStringLiteral("C:\\ltool.exe"));
    if (!apply(run, approved, exe, Mode::SetupUnselected, nullptr, false)
        || !apply(run, approved, exe, Mode::Disable, nullptr, false) || run.contains("Hotaru") || !run.contains("ltool"))
        return false;
    // 即使旧值也指向当前 exe，开发版仍不得认领正式版或 ltool 的值。
    run.setValue("Hotaru", old);
    run.setValue("ltool", old);
    if (!apply(run, approved, exe, Mode::SetupUnselected, nullptr, true) || run.contains("HotaruDev"))
        return false;
    if (!apply(run, approved, exe, Mode::Enable, nullptr, true)
        || run.value("HotaruDev") != autostartEntry(exe) || run.value("Hotaru") != old || run.value("ltool") != old)
        return false;
    return apply(run, approved, exe, Mode::Disable, nullptr, true)
        && !run.contains("HotaruDev") && run.value("Hotaru") == old && run.value("ltool") == old;
}

} // namespace Platform
