#pragma once
#include <QString>
#include <windows.h>

// 句柄生命周期决定宿主是否仍存在，不依赖先探测再监听的时序。
class ResidentLock {
public:
    enum Result { Owner, Existing, Error };
    ResidentLock() = default;
    ResidentLock(const ResidentLock&) = delete;
    ResidentLock& operator=(const ResidentLock&) = delete;
    ~ResidentLock() { if (m_handle) CloseHandle(m_handle); }
    Result acquire(const QString& name)
    {
        const auto local = QStringLiteral("Local\\") + name;
        m_handle = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(local.utf16()));
        const auto status = GetLastError();
        if (!m_handle)
            return status == ERROR_ACCESS_DENIED ? Existing : Error;
        return status == ERROR_ALREADY_EXISTS ? Existing : Owner;
    }
private:
    HANDLE m_handle = nullptr;
};
