#pragma once

#include <QString>

// 渠道标识随二进制固定，改名或放进别的目录都不会改变它。
namespace BuildChannel {

#if defined(HOTARU_DEV_BUILD) && HOTARU_DEV_BUILD
inline constexpr bool development = true;
#else
inline constexpr bool development = false;
#endif

inline QString appId(bool dev = development)
{
    return dev ? QStringLiteral("HotaruDev") : QStringLiteral("Hotaru");
}

inline QString displayName()
{
    return development ? QStringLiteral("萤 Hotaru 开发版") : QStringLiteral("萤 Hotaru");
}

inline QString defaultHotkey()
{
    return development ? QStringLiteral("Ctrl+Alt+H") : QStringLiteral("Alt+Space");
}

inline constexpr const char* dataOverride = development ? "HOTARU_DEV_DATA_DIR" : "LTOOL_DATA_DIR";

} // namespace BuildChannel
