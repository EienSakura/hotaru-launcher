#pragma once

#include <bitset>
#include <cstdint>

// 只识别同侧 Ctrl 的两次完整点按。键盘钩子负责提供原始事件及时间戳。
class CtrlTap {
public:
    static constexpr unsigned left = 0xa2;
    static constexpr unsigned right = 0xa3;

    void prime(unsigned key) { m_held.set(key); }

    bool event(unsigned key, bool down, bool injected, std::uint32_t time)
    {
        if (injected || key >= m_held.size()) {
            cancel();
            return false;
        }
        const bool ctrl = key == left || key == right;
        if (down) {
            if (m_held.test(key))
                return false;
            const bool alone = m_held.none();
            m_held.set(key);
            if (!ctrl || !alone) {
                cancel();
                return false;
            }
            m_second = m_firstKey == key && time - m_firstUp <= 300;
            m_firstKey = 0;
            m_tapKey = key;
            m_downAt = time;
            return false;
        }
        const bool wasDown = m_held.test(key);
        m_held.reset(key);
        if (!ctrl || !wasDown || m_tapKey != key)
            return false;
        m_tapKey = 0;
        if (time - m_downAt > 250 || m_held.any()) {
            cancel();
            return false;
        }
        if (m_second) {
            cancel();
            return true;
        }
        m_firstKey = key;
        m_firstUp = time;
        return false;
    }

private:
    void cancel()
    {
        m_firstKey = 0;
        m_tapKey = 0;
        m_second = false;
    }

    std::bitset<256> m_held;
    unsigned m_firstKey = 0, m_tapKey = 0;
    std::uint32_t m_firstUp = 0, m_downAt = 0;
    bool m_second = false;
};
