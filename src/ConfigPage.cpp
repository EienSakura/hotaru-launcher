#include "ConfigPage.h"
#include "BuildChannel.h"
#include "platform/Autostart.h"
#include "ShellMenu.h"
#include "DevEnv.h"
#include "CaptureHistory.h"
#include "LauncherTheme.h"
#include "Utils.h"
#include "PluginStore.h"
#include "PluginHost.h"
#include "PinnedApps.h"
#include "SettingsDialog.h"
#include "SettingsIcons.h"
#include "SettingsControlStyle.h"
#include "SettingsPalette.h"
#include "platform/SysConfig.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QTabWidget>
#include <QTabBar>
#include <QScrollArea>
#include <QKeySequence>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QAbstractNativeEventFilter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>
#include <functional>

#include <windows.h>

#include <algorithm>

// 固定项统一导入卡片：既支持拖放，也把点击选择放在同一个入口，避免程序/文件夹两个入口含义漂移。
class PinnedDropCard final : public QFrame {
public:
    explicit PinnedDropCard(QWidget* parent = nullptr) : QFrame(parent)
    {
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(64);
        setFrameShape(QFrame::StyledPanel);
        setObjectName(QStringLiteral("settingsDrop"));
        Theme::bindSettings(this, "QFrame#settingsDrop{border:1px dashed @line;border-radius:10px;background:@group;}"
                                  "QFrame#settingsDrop:hover{border-color:@accent;background:@hover;}");
        m_dragTimer.setInterval(90);
        connect(&m_dragTimer, &QTimer::timeout, this, [this] {
            m_dragPhase = (m_dragPhase + 2) % 20;
            update();
        });
    }
    std::function<void()> clicked;
    std::function<void(const QList<QUrl>&)> dropped;
protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && clicked)
            clicked();
        QFrame::mousePressEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!event->mimeData()->hasUrls()) { event->ignore(); return; }
        setDragFeedback(true);
        event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->mimeData()->hasUrls()) { setDragFeedback(true); event->acceptProposedAction(); }
        else event->ignore();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        setDragFeedback(false);
        event->accept();
    }
    void dropEvent(QDropEvent* event) override
    {
        if (event->mimeData()->hasUrls() && dropped) {
            dropped(event->mimeData()->urls());
            event->acceptProposedAction();
        } else event->ignore();
        setDragFeedback(false);
    }
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);
        if (!m_dragActive)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlpha(32);
        const QRectF area = rect().adjusted(2, 2, -2, -2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(area, 9, 9);
        QPen pen(accent, 2, Qt::DashLine);
        pen.setDashOffset(-m_dragPhase);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        painter.drawRoundedRect(area, 9, 9);
    }
public:
    void setDragFeedback(bool active)
    {
        if (m_dragActive == active)
            return;
        m_dragActive = active;
        if (active) m_dragTimer.start(); else m_dragTimer.stop();
        update();
    }

    QTimer m_dragTimer;
    int m_dragPhase = 0;
    bool m_dragActive = false;
};

// QKeySequenceEdit 会把“只按 Ctrl”当作未完成输入，无法表达双击 Ctrl，
// 某些键盘布局下 Alt+Space 也会被窗口系统吞掉。这里直接捕获原始按键，
// 只把一个完整组合交给设置页，避免让 QLineEdit 把它当普通文本处理。
class HotkeyCaptureEdit final : public QLineEdit {
public:
    explicit HotkeyCaptureEdit(QWidget* parent = nullptr) : QLineEdit(parent)
    {
        setReadOnly(true);
        setFocusPolicy(Qt::StrongFocus);
        setPlaceholderText(QStringLiteral("点击后按下快捷键"));
        setMinimumHeight(36);
    }
    std::function<void()> changed;
    QKeySequence keySequence() const { return m_sequence; }
    bool isDoubleCtrl() const { return m_doubleCtrl; }
    QString hotkeyText() const { return m_doubleCtrl ? QStringLiteral("Ctrl+Ctrl") : m_sequence.toString(); }
    void setHotkeyText(const QString& text)
    {
        m_doubleCtrl = text.compare(QStringLiteral("Ctrl+Ctrl"), Qt::CaseInsensitive) == 0;
        m_sequence = m_doubleCtrl ? QKeySequence() : QKeySequence(text);
        m_heldModifiers = Qt::NoModifier;
        m_lastCtrlPress = 0;
        m_lastCtrlRelease = 0;
        setText(m_doubleCtrl ? QStringLiteral("Ctrl+Ctrl（双击）") : m_sequence.toString());
    }
    void setDoubleCtrl()
    {
        m_doubleCtrl = true;
        m_sequence = QKeySequence();
        m_heldModifiers = Qt::NoModifier;
        setText(QStringLiteral("Ctrl+Ctrl（双击）"));
        if (changed) changed();
    }
    void clearHotkey()
    {
        m_doubleCtrl = false;
        m_sequence = QKeySequence();
        m_heldModifiers = Qt::NoModifier;
        m_lastCtrlPress = 0;
        m_lastCtrlRelease = 0;
        clear();
        if (changed) changed();
    }
protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->isAutoRepeat()) return;
        const int key = event->key();
        if (key == Qt::Key_Control) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_heldModifiers == Qt::NoModifier
                && ((m_lastCtrlPress > 0 && now - m_lastCtrlPress <= 520)
                || (m_lastCtrlRelease > 0 && now - m_lastCtrlRelease <= 520))) {
                setDoubleCtrl();
                m_lastCtrlPress = 0;
                m_lastCtrlRelease = 0;
            } else {
                m_waitingCtrl = true;
                m_lastCtrlPress = now;
                m_heldModifiers |= Qt::ControlModifier;
                setText(modifierText(m_heldModifiers)
                        + QStringLiteral(" · 再按一次确认双击，或继续按主键"));
            }
            event->accept();
            return;
        }
        const Qt::KeyboardModifiers modifier = modifierForKey(key);
        if (modifier != Qt::NoModifier) {
            m_heldModifiers |= modifier;
            if (key != Qt::Key_Control)
                m_lastCtrlPress = 0;
            setText(modifierText(m_heldModifiers) + QStringLiteral(" · 继续按主键"));
            event->accept();
            return;
        }
        if (key == Qt::Key_Escape || key == Qt::Key_Backspace || key == Qt::Key_Delete) {
            clearHotkey(); event->accept(); return;
        }
        Qt::KeyboardModifiers mods = m_heldModifiers
            | (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier
                                     | Qt::ShiftModifier | Qt::MetaModifier));
        if (!mods) {
            setText(QStringLiteral("格式无效：需要 Ctrl、Alt、Shift 或 Win 加主键"));
            m_sequence = QKeySequence(); m_doubleCtrl = false;
            if (changed) changed();
            event->accept(); return;
        }
        m_doubleCtrl = false;
        m_lastCtrlPress = 0;
        m_lastCtrlRelease = 0;
        m_sequence = QKeySequence(int(mods) | key);
        setText(m_sequence.toString());
        if (changed) changed();
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent* event) override
    {
        const Qt::KeyboardModifiers modifier = modifierForKey(event->key());
        if (!event->isAutoRepeat() && event->key() == Qt::Key_Control) {
            m_waitingCtrl = false;
            m_lastCtrlRelease = QDateTime::currentMSecsSinceEpoch();
        }
        if (!event->isAutoRepeat() && modifier != Qt::NoModifier)
            m_heldModifiers &= ~modifier;
        event->accept();
    }
private:
    static Qt::KeyboardModifiers modifierForKey(int key)
    {
        if (key == Qt::Key_Control) return Qt::ControlModifier;
        if (key == Qt::Key_Alt) return Qt::AltModifier;
        if (key == Qt::Key_Shift) return Qt::ShiftModifier;
        if (key == Qt::Key_Meta) return Qt::MetaModifier;
        return Qt::NoModifier;
    }

    static QString modifierText(Qt::KeyboardModifiers modifiers)
    {
        QStringList parts;
        if (modifiers & Qt::ControlModifier) parts << QStringLiteral("Ctrl");
        if (modifiers & Qt::AltModifier) parts << QStringLiteral("Alt");
        if (modifiers & Qt::ShiftModifier) parts << QStringLiteral("Shift");
        if (modifiers & Qt::MetaModifier) parts << QStringLiteral("Win");
        return parts.join(QLatin1Char('+'));
    }

    QKeySequence m_sequence;
    Qt::KeyboardModifiers m_heldModifiers = Qt::NoModifier;
    qint64 m_lastCtrlPress = 0;
    qint64 m_lastCtrlRelease = 0;
    bool m_waitingCtrl = false;
    bool m_doubleCtrl = false;
};

// Alt+Space 会以 WM_SYSKEYDOWN 进入窗口，Windows 默认会先把它交给系统菜单，
// 仅靠 Qt 的 keyPressEvent 收不到这一下。二级捕获窗在原生消息层先截获键盘消息，
// 再转成 Qt 事件交给 HotkeyCaptureEdit，同时阻止系统菜单和主窗口继续处理。
class HotkeyCaptureDialog final : public QDialog {
public:
    explicit HotkeyCaptureDialog(HotkeyCaptureEdit* editor, QWidget* parent = nullptr)
        : QDialog(parent), m_editor(editor) {}

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override
    {
        if (eventType != QByteArrayLiteral("windows_generic_MSG")
            && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))
            return QDialog::nativeEvent(eventType, message, result);
        auto* msg = static_cast<MSG*>(message);
        if (!msg || !m_editor)
            return QDialog::nativeEvent(eventType, message, result);
        if (msg->message == WM_SYSCOMMAND
            && (msg->wParam & 0xfff0) == SC_KEYMENU) {
            if (result)
                *result = 0;
            return true;
        }
        if (!m_editor->hasFocus())
            return QDialog::nativeEvent(eventType, message, result);
        const bool press = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
        const bool release = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
        if (!press && !release)
            return QDialog::nativeEvent(eventType, message, result);
        const Qt::Key key = qtKeyFromVirtual(static_cast<UINT>(msg->wParam));
        if (key == Qt::Key_unknown)
            return QDialog::nativeEvent(eventType, message, result);
        if (press && key == Qt::Key_Escape) {
            reject();
            if (result)
                *result = 0;
            return true;
        }
        const auto modifiers = currentModifiers();
        const bool repeat = press && ((msg->lParam & (1LL << 30)) != 0);
        QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease,
                        key, modifiers, QString(), repeat, 1);
        QApplication::sendEvent(m_editor, &event);
        if (result)
            *result = 0;
        return true;
    }

public:
    static Qt::KeyboardModifiers currentModifiers()
    {
        Qt::KeyboardModifiers modifiers = Qt::NoModifier;
        if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= Qt::ControlModifier;
        if (GetKeyState(VK_MENU) & 0x8000) modifiers |= Qt::AltModifier;
        if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= Qt::ShiftModifier;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            modifiers |= Qt::MetaModifier;
        return modifiers;
    }

    static Qt::Key qtKeyFromVirtual(UINT vk)
    {
        if (vk >= 'A' && vk <= 'Z') return static_cast<Qt::Key>(int(Qt::Key_A) + int(vk - 'A'));
        if (vk >= '0' && vk <= '9') return static_cast<Qt::Key>(int(Qt::Key_0) + int(vk - '0'));
        if (vk >= VK_F1 && vk <= VK_F24) return static_cast<Qt::Key>(int(Qt::Key_F1) + int(vk - VK_F1));
        switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return Qt::Key_Control;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return Qt::Key_Alt;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return Qt::Key_Shift;
        case VK_LWIN: case VK_RWIN: return Qt::Key_Meta;
        case VK_SPACE: return Qt::Key_Space;
        case VK_TAB: return Qt::Key_Tab;
        case VK_RETURN: return Qt::Key_Return;
        case VK_ESCAPE: return Qt::Key_Escape;
        case VK_BACK: return Qt::Key_Backspace;
        case VK_DELETE: return Qt::Key_Delete;
        case VK_INSERT: return Qt::Key_Insert;
        case VK_HOME: return Qt::Key_Home;
        case VK_END: return Qt::Key_End;
        case VK_LEFT: return Qt::Key_Left;
        case VK_RIGHT: return Qt::Key_Right;
        case VK_UP: return Qt::Key_Up;
        case VK_DOWN: return Qt::Key_Down;
        case VK_PRIOR: return Qt::Key_PageUp;
        case VK_NEXT: return Qt::Key_PageDown;
        case VK_OEM_COMMA: return Qt::Key_Comma;
        case VK_OEM_PERIOD: return Qt::Key_Period;
        case VK_OEM_2: return Qt::Key_Slash;
        case VK_OEM_5: return Qt::Key_Backslash;
        case VK_OEM_1: return Qt::Key_Semicolon;
        case VK_OEM_7: return Qt::Key_Apostrophe;
        case VK_OEM_4: return Qt::Key_BracketLeft;
        case VK_OEM_6: return Qt::Key_BracketRight;
        case VK_OEM_MINUS: return Qt::Key_Minus;
        case VK_OEM_PLUS: return Qt::Key_Equal;
        default: return Qt::Key_unknown;
        }
    }

private:
    QPointer<HotkeyCaptureEdit> m_editor;
};

// QDialog::nativeEvent 在不同 Qt/Windows 组合下不一定能拿到发给输入焦点的
// WM_SYSKEYDOWN；全局原生过滤器在弹窗存活期间补上这一层，保证 Ctrl/Alt/主键
// 每一次按下都能实时送到编辑控件。
class HotkeyCaptureNativeFilter final : public QAbstractNativeEventFilter {
public:
    explicit HotkeyCaptureNativeFilter(HotkeyCaptureEdit* editor) : m_editor(editor) {}

    bool nativeEventFilter(const QByteArray& eventType, void* message, qint64* result) override
    {
        if (eventType != QByteArrayLiteral("windows_generic_MSG")
            && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))
            return false;
        auto* msg = static_cast<MSG*>(message);
        if (!msg || !m_editor)
            return false;
        if (msg->message == WM_SYSCOMMAND
            && (msg->wParam & 0xfff0) == SC_KEYMENU) {
            if (result) *result = 0;
            return true;
        }
        const bool press = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
        const bool release = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
        if (!press && !release)
            return false;
        const Qt::Key key = HotkeyCaptureDialog::qtKeyFromVirtual(static_cast<UINT>(msg->wParam));
        if (key == Qt::Key_unknown)
            return false;
        if (key == Qt::Key_Escape)
            return false;
        // 回车交给按钮，Esc 交给 QDialog 取消；其余键在捕获期间都必须吞掉，
        // 即使焦点刚好还停在弹窗标题或按钮上，也不能落到系统/主窗口。
        if (!m_editor->hasFocus() && (key == Qt::Key_Return || key == Qt::Key_Enter
                                      || key == Qt::Key_Escape))
            return false;
        const bool repeat = press && ((msg->lParam & (1LL << 30)) != 0);
        QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease,
                        key, HotkeyCaptureDialog::currentModifiers(), QString(), repeat, 1);
        QApplication::sendEvent(m_editor, &event);
        if (result) *result = 0;
        return true;
    }

private:
    QPointer<HotkeyCaptureEdit> m_editor;
};

QString choosePinnedTarget(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("添加固定项"));
    dialog.setObjectName(QStringLiteral("addPinDialog"));
    dialog.setMinimumWidth(520);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(12);
    auto* hint = new QLabel(QStringLiteral("选择一种目标类型。每次添加一个目标，磁盘文件夹会作为固定项保存。"), &dialog);
    hint->setObjectName(QStringLiteral("muted")); hint->setWordWrap(true); root->addWidget(hint);
    auto* cards = new QHBoxLayout; cards->setSpacing(12);
    auto* app = new QPushButton(QStringLiteral("程序或脚本\n.exe · .lnk · .bat · .cmd"), &dialog);
    auto* folder = new QPushButton(QStringLiteral("磁盘文件夹\n打开真实目录"), &dialog);
    for (auto* button : {app, folder}) {
        button->setMinimumHeight(88);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral("QPushButton{text-align:left;padding:14px;border:1px solid palette(mid);border-radius:10px;font-size:11pt;}QPushButton:hover{border-color:palette(highlight);background:palette(alternate-base);}"));
        cards->addWidget(button, 1);
    }
    root->addLayout(cards);
    auto* drop = new PinnedDropCard(&dialog);
    auto* dropLay = new QHBoxLayout(drop);
    dropLay->setContentsMargins(14, 10, 14, 10);
    auto* dropText = new QLabel(QStringLiteral("拖入程序、脚本或磁盘文件夹 · 拖入后自动填写目标"), drop);
    dropText->setObjectName(QStringLiteral("muted"));
    dropText->setWordWrap(true);
    dropLay->addWidget(dropText, 1);
    root->addWidget(drop);
    auto* cancel = new QPushButton(QStringLiteral("取消"), &dialog); cancel->setObjectName(QStringLiteral("secondary"));
    root->addWidget(cancel, 0, Qt::AlignRight);
    QString path;
    drop->dropped = [&](const QList<QUrl>& urls) {
        if (urls.size() != 1) { dropText->setText(QStringLiteral("一次只能拖入一个目标")); return; }
        if (!urls.first().isLocalFile()) { dropText->setText(QStringLiteral("只能拖入本地文件或文件夹")); return; }
        const QString candidate = urls.first().toLocalFile();
        if (!PinnedApps::supportsTarget(candidate)) { dropText->setText(QStringLiteral("目标类型不支持，请拖入 exe、lnk、bat、cmd 或文件夹")); return; }
        path = candidate;
        dropText->setText(QStringLiteral("已填入：%1").arg(QFileInfo(candidate).fileName().isEmpty()
                                                              ? candidate : QFileInfo(candidate).fileName()));
        dialog.accept();
    };
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(app, &QPushButton::clicked, &dialog, [&] {
        path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("选择程序或脚本"), QDir::rootPath(),
                                            QStringLiteral("程序和脚本 (*.exe *.lnk *.bat *.cmd);;所有文件 (*.*)"));
        if (!path.isEmpty()) dialog.accept();
    });
    QObject::connect(folder, &QPushButton::clicked, &dialog, [&] {
        path = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("选择磁盘文件夹"));
        if (!path.isEmpty()) dialog.accept();
    });
    Theme::bindModal(&dialog);
    return dialog.exec() == QDialog::Accepted ? path : QString();
}

namespace {

bool configKeySequenceToHotkey(const QKeySequence& sequence, UINT* mods, UINT* vk)
{
    if (!mods || !vk || sequence.count() != 1) return false;
    const auto combo = sequence[0];
    const auto key = combo.key();
    UINT m = 0;
    const auto qm = combo.keyboardModifiers();
    if (qm & Qt::ControlModifier) m |= MOD_CONTROL;
    if (qm & Qt::AltModifier) m |= MOD_ALT;
    if (qm & Qt::ShiftModifier) m |= MOD_SHIFT;
    if (qm & Qt::MetaModifier) m |= MOD_WIN;
    UINT k = 0;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) k = 'A' + UINT(key - Qt::Key_A);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) k = '0' + UINT(key - Qt::Key_0);
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) k = VK_F1 + UINT(key - Qt::Key_F1);
    else {
        switch (key) {
        case Qt::Key_Space: k = VK_SPACE; break; case Qt::Key_Tab: k = VK_TAB; break;
        case Qt::Key_Return: case Qt::Key_Enter: k = VK_RETURN; break; case Qt::Key_Escape: k = VK_ESCAPE; break;
        case Qt::Key_Backspace: k = VK_BACK; break; case Qt::Key_Delete: k = VK_DELETE; break;
        case Qt::Key_Insert: k = VK_INSERT; break; case Qt::Key_Home: k = VK_HOME; break; case Qt::Key_End: k = VK_END; break;
        case Qt::Key_Left: k = VK_LEFT; break; case Qt::Key_Right: k = VK_RIGHT; break;
        case Qt::Key_Up: k = VK_UP; break; case Qt::Key_Down: k = VK_DOWN; break;
        case Qt::Key_PageUp: k = VK_PRIOR; break; case Qt::Key_PageDown: k = VK_NEXT; break;
        case Qt::Key_Comma: k = VK_OEM_COMMA; break; case Qt::Key_Period: k = VK_OEM_PERIOD; break;
        case Qt::Key_Slash: k = VK_OEM_2; break; case Qt::Key_Backslash: k = VK_OEM_5; break;
        case Qt::Key_Semicolon: k = VK_OEM_1; break; case Qt::Key_Apostrophe: k = VK_OEM_7; break;
        case Qt::Key_BracketLeft: k = VK_OEM_4; break; case Qt::Key_BracketRight: k = VK_OEM_6; break;
        case Qt::Key_Minus: k = VK_OEM_MINUS; break; case Qt::Key_Equal: k = VK_OEM_PLUS; break;
        case Qt::Key_Plus: k = VK_ADD; break; case Qt::Key_Asterisk: k = VK_MULTIPLY; break;
        default: break;
        }
    }
    const bool functionKey = key >= Qt::Key_F1 && key <= Qt::Key_F24;
    if (!k || (!m && !functionKey)) return false;
    *mods = m; *vk = k; return true;
}

// 这个目录能不能用。空串代表能用（空值 = 用默认位置，永远合法）；否则返回给人看的原因。
QString dirProblem(const QString& path)
{
    if (path.isEmpty())
        return {};
    QDir().mkpath(path);
    if (!QFileInfo::exists(path))
        return QStringLiteral("建不出来");
    const QString probe = path + "/.ltool-write-test";
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly))
        return QStringLiteral("写不进去（没权限 / 只读 / 盘满）");
    f.close();
    QFile::remove(probe);
    return {};
}

// "现在这个目录里有多少个、多大" —— 只数 ltool 自己命名的那批文件
QString dirSummary(const QString& dir, const QStringList& patterns)
{
    QDir d(dir);
    if (!d.exists())
        return QStringLiteral("（目录还没建）");
    const QFileInfoList files = d.entryInfoList(patterns, QDir::Files);
    qint64 bytes = 0;
    for (const QFileInfo& fi : files)
        bytes += fi.size();
    return QStringLiteral("%1 个 · %2 MB").arg(files.size())
        .arg(bytes / 1048576.0, 0, 'f', 1);
}

// 列表里"是目录的那几项"有几个：往里找几层这个设置只对目录有效，
// 点名加的 exe 一条都不受它影响，所以状态行要把这两个数分开写
int dirItemCount(QListWidget* list)
{
    int n = 0;
    for (int i = 0; i < list->count(); ++i)
        if (QFileInfo(list->item(i)->text()).isDir())
            ++n;
    return n;
}

class ModePreview final : public QRadioButton {
public:
    ModePreview(const QString& title, bool dark, QWidget* parent)
        : QRadioButton(title, parent), m_dark(dark)
    {
        setFixedSize(132, 82);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    bool hitButton(const QPoint& position) const override { return rect().contains(position); }

    void paintEvent(QPaintEvent*) override
    {
        const auto colors = SettingsPalette::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(isChecked() || hasFocus() ? colors.accent : colors.line));
        p.setBrush(QColor(isChecked() ? colors.selected : underMouse() ? colors.hover : colors.field));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
        const QRect preview(7, 7, width() - 14, 46);
        const QColor background(m_dark ? "#282d37" : "#eef1f6");
        const QColor panel(m_dark ? "#3c4451" : "#ffffff");
        p.setPen(Qt::NoPen);
        p.setBrush(background);
        p.drawRoundedRect(preview, 5, 5);
        p.fillRect(preview.adjusted(1, 4, -1, -preview.height() + 8), panel);
        p.fillRect(QRect(preview.left() + 2, preview.top() + 8, 26, 36),
                   QColor(m_dark ? "#22262e" : "#dfe4ef"));
        const int left = preview.left() + 35;
        p.fillRect(QRect(left, 20, 24, 4), QColor(m_dark ? "#7893c3" : "#bbcbe4"));
        p.fillRect(QRect(left, 28, 74, 6), panel);
        p.fillRect(QRect(left, 39, 74, 6), panel);
        p.setPen(QColor(colors.text));
        settingsIcon(m_dark ? SettingsGlyph::Moon : SettingsGlyph::Sun, QColor(colors.text))
            .paint(&p, QRect(10, 60, 13, 13));
        p.drawText(QRect(27, 55, width() - 52, 23), Qt::AlignVCenter, text());
        if (isChecked())
            settingsIcon(SettingsGlyph::Check, QColor(colors.accent)).paint(&p, QRect(width() - 24, 60, 13, 13));
    }

private:
    bool m_dark;
};

class GridPreview final : public QWidget {
public:
    GridPreview(QSpinBox* rows, QSpinBox* columns, QWidget* parent)
        : QWidget(parent), m_rows(rows), m_columns(columns)
    {
        setFixedSize(64, 48);
        for (auto* size : {m_rows, m_columns})
            connect(size, &QSpinBox::valueChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        constexpr int step = 7;
        const int x = (width() - m_columns->value() * step + 2) / 2;
        const int y = (height() - m_rows->value() * step + 2) / 2;
        QColor color(SettingsPalette::current().accent);
        color.setAlphaF(0.72F);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int row = 0; row < m_rows->value(); ++row)
            for (int column = 0; column < m_columns->value(); ++column)
                painter.drawRoundedRect(QRect(x + column * step, y + row * step, 5, 5), 1.5, 1.5);
    }

private:
    QSpinBox* m_rows;
    QSpinBox* m_columns;
};

void addAppearanceRow(QVBoxLayout* section, const QString& title, const QString& caption,
                      QWidget* controls, SettingsGlyph icon)
{
    auto* row = new QWidget(section->parentWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(13, 8, 13, 8);
    layout->setSpacing(11);
    layout->addWidget(new SettingsIconTile(icon, row));
    auto* copy = new QWidget(row);
    auto* description = new QVBoxLayout(copy);
    description->setContentsMargins(0, 0, 0, 0);
    description->setSpacing(2);
    auto* name = new QLabel(title, row);
    name->setObjectName(QStringLiteral("settingRowTitle"));
    description->addWidget(name);
    auto* hint = new QLabel(caption, row);
    hint->setObjectName(QStringLiteral("muted"));
    hint->setWordWrap(true);
    description->addWidget(hint);
    layout->addWidget(copy, 1, Qt::AlignVCenter);
    for (auto* button : controls->findChildren<QPushButton*>())
        button->setMinimumHeight(32);
    layout->addWidget(controls);
    if (section->count() > 0) {
        auto* divider = new QFrame(section->parentWidget());
        divider->setObjectName(QStringLiteral("settingsDivider"));
        divider->setFixedHeight(1);
        section->addWidget(divider);
    }
    section->addWidget(row);
}

const char* kConfigLayoutQss = R"QSS(
QLabel { color: @text; background: transparent; }
QLabel#pageTitle { font-size: 21pt; font-weight: 700; }
QLabel#muted, QLabel#settingsSaveStatus { color: @muted; }
QLabel#settingsSaveStatus[saveError="true"] { color: @bad; }
QLabel#settingRowTitle { font-weight: 600; }
QLabel#pinStatus { color: @muted; background: @field; border-radius: 7px; padding: 3px 8px; }
QLabel#pinStatus[active="true"] { color: @okText; }
QPushButton#pinnedAddButton { border: 1px solid @primary; border-radius: 8px; padding: 6px 12px; color: @onPrimary; background: @primary; }
QPushButton#pinnedAddButton:hover { background: @accent; }
QPushButton#pinnedAddButton:focus { border: 2px solid @text; }
QFrame#settingsDivider { background: @line; }
QFrame#settingsFooter { border-top: 1px solid @line; background: @group; }
QDialog#hotaruSettings QFrame#settingsFooter { border: none; background: transparent; }
QFrame#pinnedSection { background: @group; border: 1px solid @line; border-radius: 13px; }
QGroupBox {
    background: @group; color: @text; border: 1px solid @line; border-radius: 13px;
    margin-top: 23px; padding: 8px 12px 12px; font-weight: 600;
}
QGroupBox::title { subcontrol-origin: margin; left: 1px; }
QGroupBox#appearanceSection { padding: 0; }
QTabWidget#cfgTabs::pane { border: none; background: transparent; }
QTabBar#settingsSegments { background: @field; border: 1px solid @line; border-radius: 10px; }
QTabBar#settingsSegments::tab { color: @muted; min-width: 64px; padding: 6px 6px;
    margin: 3px 2px; border: 1px solid transparent; border-radius: 7px; }
QTabBar#settingsSegments::tab:selected { background: @group; color: @text; border-color: @border; }
QTabBar#settingsSegments::tab:focus { border-color: @accent; }
QScrollArea#cfgScroll, QScrollArea#cfgScroll > QWidget#qt_scrollarea_viewport { background: transparent; }
QWidget#cfgBody { background: @window; }
QScrollArea#cfgScroll QScrollBar:vertical { background: transparent; width: 7px; margin: 0 1px; }
QScrollArea#cfgScroll QScrollBar::handle:vertical { background: @line; border-radius: 2px; min-height: 24px; }
QScrollArea#cfgScroll QScrollBar::add-line:vertical, QScrollArea#cfgScroll QScrollBar::sub-line:vertical { height: 0; }
QScrollArea#cfgScroll QScrollBar::add-page:vertical, QScrollArea#cfgScroll QScrollBar::sub-page:vertical { background: transparent; }
QListWidget { background: @field; color: @text; border: 1px solid @line; border-radius: 9px; }
QListWidget::item { border-bottom: 1px solid @line; padding: 0 8px; }
QListWidget::item:selected { background: @selBg; color: @text; }
QListWidget#settingsAppRoots::item { min-height: 28px; }
QListWidget#pinnedApps, QListWidget#pinnedApps > QWidget#qt_scrollarea_viewport {
    background: transparent; border: none; outline: none;
}
QListWidget#pinnedApps::item, QListWidget#pinnedApps::item:selected {
    background: transparent; border: none; padding: 0;
}
QFrame#pinnedAppRow { background: @group; border: 1px solid @line; border-radius: 10px; }
QFrame#pinnedAppRow[selected="true"] { background: @hover; border-color: @accent; }
)QSS";

constexpr int kPinnedRowHeight = 52;
constexpr int kPinnedRowSpacing = 3;

} // namespace

ConfigPage::ConfigPage(QWidget* parent)
    : QWidget(parent)
{
    // 配置项越加越多，早就超出窗口高度了：整页放进滚动区，不然下面的分组看不见也点不到
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* content = new QVBoxLayout;
    content->setContentsMargins(30, 20, 30, 0);
    content->setSpacing(12);
    outer->addLayout(content, 1);
    auto* header = new QHBoxLayout;
    auto* titleLayout = new QVBoxLayout;
    titleLayout->setSpacing(4);
    auto* heading = new QLabel(QStringLiteral("通用"), this);
    heading->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitle = new QLabel(QStringLiteral("管理启动、搜索和存放目录。"), this);
    subtitle->setObjectName(QStringLiteral("muted"));
    titleLayout->addWidget(heading);
    titleLayout->addWidget(subtitle);
    header->addLayout(titleLayout, 1);
    auto* segments = new QTabBar(this);
    segments->setObjectName(QStringLiteral("settingsSegments"));
    segments->setExpanding(false);
    segments->setDrawBase(false);
    segments->addTab(QStringLiteral("通用"));
    segments->addTab(QStringLiteral("外观"));
    header->addWidget(segments, 0, Qt::AlignVCenter);
    content->addLayout(header);
    // 2026-10-20 他点名把配置页拆成两个 tab：通用 / 外观。
    // 外观那一页眼下只有「夜间 / 日间」这一行，但它以后是所有跟长相有关的开关的落点 ——
    // 继续挤在通用那张长列表里，下次加字号/密度就得再搬一次。
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("cfgTabs"));
    tabs->tabBar()->hide();
    connect(segments, &QTabBar::currentChanged, tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, this, [segments, heading, subtitle](int index) {
        segments->setCurrentIndex(index);
        heading->setText(index == 1 ? QStringLiteral("外观") : QStringLiteral("通用"));
        subtitle->setText(index == 1 ? QStringLiteral("管理界面配色、启动器主题和固定软件。")
                                     : QStringLiteral("管理启动、搜索和存放目录。"));
    });
    auto* scroller = new QScrollArea(tabs);
    scroller->setObjectName(QStringLiteral("cfgScroll"));
    scroller->setWidgetResizable(true);
    scroller->setFrameShape(QFrame::NoFrame);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("cfgBody"));
    scroller->setWidget(body);
    tabs->addTab(scroller, QString::fromUtf8("通用"));
    content->addWidget(tabs, 1);
    auto* lay = new QVBoxLayout(body);
    lay->setContentsMargins(0, 0, 4, 14);
    lay->setSpacing(12);

    auto* general = new QGroupBox(QStringLiteral("启动与快捷键"), this);
    auto* generalLay = new QFormLayout(general);
    m_autostart = new QCheckBox("开机自动启动 Hotaru", general);
    generalLay->addRow(m_autostart);
    auto* hotkeyRow = new QWidget(general);
    auto* hotkeyLay = new QHBoxLayout(hotkeyRow);
    hotkeyLay->setContentsMargins(0, 0, 0, 0);
    m_hotkey = new HotkeyCaptureEdit(hotkeyRow);
    m_hotkey->setObjectName(QStringLiteral("hotkeyCapture"));
    m_hotkey->setVisible(false);
    m_hotkeyButton = new QPushButton(QStringLiteral("点击录入快捷键…"), hotkeyRow);
    m_hotkeyButton->setObjectName(QStringLiteral("hotkeyCaptureButton"));
    m_hotkeyButton->setMinimumHeight(32);
    m_hotkeyButton->setMinimumWidth(136);
    m_hotkeyButton->setCursor(Qt::PointingHandCursor);
    hotkeyLay->addStretch(1);
    hotkeyLay->addWidget(m_hotkeyButton);
    generalLay->addRow(QStringLiteral("呼出快捷键："), hotkeyRow);
    m_hotkeyStatus = new QLabel(general);
    m_hotkeyStatus->setObjectName(QStringLiteral("muted"));
    generalLay->addRow(QString(), m_hotkeyStatus);
    m_hotkeyStatus->setAlignment(Qt::AlignRight);
    m_hotkey->changed = [this] {
        updateHotkeyStatus();
    };
    connect(m_hotkeyButton, &QPushButton::clicked, this, &ConfigPage::openHotkeyCapture);
    m_hotkeyButton->setToolTip(QStringLiteral("点击后在二级弹窗中按下 Alt+Space、Ctrl+K、Ctrl+Alt+K 或双击 Ctrl"));
    auto* downloads = new QGroupBox(QStringLiteral("下载与插件更新"), body);
    auto* downloadsLayout = new QFormLayout(downloads);
    m_downloadsLayout = downloadsLayout;
    m_installPath = new QLineEdit(downloads);
    m_installPath->setPlaceholderText(QDir::toNativeSeparators(Platform::defaultInstallRoot()));
    m_installPath->setClearButtonEnabled(true);
    auto* installRow = new QWidget(downloads);
    m_installRow = installRow;
    auto* installLay = new QHBoxLayout(installRow);
    installLay->setContentsMargins(0, 0, 0, 0);
    installLay->setSpacing(8);
    auto* browseInstall = new QPushButton(QStringLiteral("浏览…"), installRow);
    auto* clearInstall = new QPushButton(QStringLiteral("清空"), installRow);
    installLay->addWidget(m_installPath, 1);
    installLay->addWidget(browseInstall);
    installLay->addWidget(clearInstall);
    downloadsLayout->addRow(QStringLiteral("第三方软件安装根目录："), installRow);
    auto* installInfo = new QLabel(downloads);
    m_installInfo = installInfo;
    installInfo->setObjectName(QStringLiteral("muted"));
    installInfo->setWordWrap(true);
    downloadsLayout->addRow(QString(), installInfo);
    connect(browseInstall, &QPushButton::clicked, this, [this] {
        const QString start = m_installPath->text().trimmed().isEmpty()
            ? Platform::defaultInstallRoot() : m_installPath->text().trimmed();
        const QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择第三方软件安装根目录"), start);
        if (!path.isEmpty()) {
            m_installPath->setText(QDir::toNativeSeparators(path));
            saveText(m_installPath, "install/path");
        }
    });
    connect(clearInstall, &QPushButton::clicked, this, [this] {
        m_installPath->clear();
        saveText(m_installPath, "install/path");
    });
    m_proxy = new QLineEdit(downloads);
    if (const QString sys = DevEnv::systemProxyUrl(); !sys.isEmpty()) {
        m_proxy->setPlaceholderText(QString("留空 = 自动用系统代理（当前检测到 %1）").arg(sys));
        m_proxy->setToolTip("填别的地址可覆盖系统代理。\n"
                            "内置下载器一直走它；winget 要用命令行代理，需管理员先执行一次\n"
                            "  winget settings --enable ProxyCommandLineOptions\n"
                            "没启用时 Hotaru 会自动不带代理装（否则 winget 会直接拒绝整条命令）。");
    } else {
        m_proxy->setPlaceholderText("例：http://127.0.0.1:10808（GitHub 包下载慢时有用）");
        m_proxy->setToolTip("没检测到系统代理。填上本地代理后，内置下载器会走它；"
                            "winget 还需要管理员先执行 winget settings --enable ProxyCommandLineOptions "
                            "才认命令行代理。");
    }
    m_proxy->setClearButtonEnabled(true);
    downloadsLayout->addRow("下载代理：", m_proxy);
    m_storeMirrors = new QLineEdit(downloads);
    m_storeMirrors->setPlaceholderText(PluginStore::mirrorList());
    m_storeMirrors->setToolTip("多个前缀用逗号分隔，留空使用内置镜像。\n"
                               "兼容插件的 GitHub 下载和 Hotaru 直属插件 ZIP 都可以使用镜像。\n"
                               "直属插件只接受 HTTPS 镜像；下载或校验失败时改试下一条，最后使用 GitHub。官方目录不走镜像。");
    m_storeMirrors->setClearButtonEnabled(true);
    downloadsLayout->addRow("插件镜像前缀：", m_storeMirrors);
    // 默认**开**：他这句是"自动做 X"（2026-10-21「插件商店增加一个自动更新功能，一天自动更新一次」），
    // 不是"给我个按钮我按了才做"。范围是他拍的：只换已装的，清单里新出现的插件仍要点「安装」。
    m_autoUpdate = new QCheckBox("每天自动更新一次已装的插件（启动之后后台跑，不弹框）", downloads);
    m_autoUpdate->setToolTip("程序启动十几秒后拉一次插件清单，把**已经装着**的那几颗换成清单里更新的版本，一天只跑一次。\n"
                             "清单里新出现的插件不会自动装 —— 那仍然要你在商店里自己点。\n"
                             "商店那一页会常驻一行说上次跑的结果（换了几颗、哪几颗没成）。换下来的那颗如果这个进程还加载着，"
                             "文件当场已经换好，界面要下次启动才用新的。");
    downloadsLayout->addRow(m_autoUpdate);
    auto* search = new QGroupBox(QStringLiteral("搜索与面板"), body);
    auto* searchLayout = new QFormLayout(search);
    m_engine = new SettingsComboBox(search);
    m_engine->addItem("百度", "baidu");
    m_engine->addItem("Google", "google");
    m_engine->addItem("必应", "bing");
    m_engine->addItem("DuckDuckGo", "duckduckgo");
    auto* engineRow = new QWidget(search);
    auto* engineLay = new QHBoxLayout(engineRow);
    engineLay->setContentsMargins(0, 0, 0, 0);
    engineLay->addStretch(1);
    m_engine->setMinimumWidth(150);
    engineLay->addWidget(m_engine);
    searchLayout->addRow("默认搜索引擎：", engineRow);
    m_favorites = new QCheckBox("呼出时显示常驻网格", search);
    m_favorites->setObjectName("showResidentGrid");
    m_favorites->setToolTip("打开：呼出时显示手动固定的软件和文件夹。\n"
                            "关闭：只显示搜索框，输入后展开搜索结果。");
    searchLayout->addRow(m_favorites);

    auto* shell = new QGroupBox("资源管理器右键菜单", this);
    m_shellGroup = shell;
    shell->setObjectName(QStringLiteral("settingsShellGroup"));
    auto* shellLay = new QFormLayout(shell);
    m_shellMenu = new QCheckBox("在文件 / 文件夹的右键里加入 Hotaru 子菜单", shell);
    m_shellMenu->setEnabled(!BuildChannel::development);
    m_shellMenu->setToolTip(QStringLiteral("子菜单里是：解压到当前目录、解压到同名文件夹、压缩为 .zip、压缩为 .7z、\n"
                                          "粉碎（不可恢复）、占用分析。\n"
                                          "后端是自带的 libarchive，zip / 7z / tar / gz / bz2 / xz / zstd / iso 都能解，"
                                          "写只写 zip 和 7z。"));
    shellLay->addRow(m_shellMenu);
    m_archiveProgress = new QCheckBox("压缩 / 解压时弹出进度窗口", shell);
    m_archiveProgress->setToolTip(QStringLiteral("关闭（默认）：后台静默做完，不弹任何窗口；只有失败才弹框告诉你，"
                                                 "结果照常写进日志。\n"
                                                 "打开：弹一个进度窗口，能看到正在处理哪个文件、百分比，并且可以取消。"));
    shellLay->addRow(m_archiveProgress);
    m_shellInfo = new QLabel(shell);
    m_shellInfo->setObjectName("muted");
    m_shellInfo->setWordWrap(true);
    shellLay->addRow("当前：", m_shellInfo);

    auto* index = new QGroupBox("定时扫描（文件索引刷新）", this);
    auto* indexLay = new QFormLayout(index);
    m_deep = new QCheckBox("启用全盘深度索引（后台进行，占用更多内存）", index);
    indexLay->addRow(m_deep);
    m_refreshHours = new QSpinBox(index);
    m_refreshHours->setRange(0, 168);
    m_refreshHours->setSuffix(" 小时");
    indexLay->addRow("扫描间隔（0 = 不自动）：", m_refreshHours);
    m_maxEntries = new QSpinBox(index);
    m_maxEntries->setRange(100000, 20000000);
    m_maxEntries->setSingleStep(500000);
    m_maxEntries->setGroupSeparatorShown(true);
    indexLay->addRow("索引条目上限：", m_maxEntries);

    auto* appDirs = new QGroupBox("应用搜索的额外项目", this);
    auto* appLay = new QFormLayout(appDirs);
    // 这些是"启动器输名字要能搜到"的应用来源。两种条目：填文件夹 = 往里找若干层，
    // 只收看起来是主程序的那些 exe（子进程和命令行工具会被跳过）；点名填某个 exe = 一定要它，
    // 无条件收。扫描是重活，所以只在保存之后才跑，不随输入跑。
    m_appRoots = new QListWidget(appDirs);
    m_appRoots->setObjectName(QStringLiteral("settingsAppRoots"));
    m_appRoots->setFixedHeight(126);
    m_appRoots->setToolTip(QStringLiteral(
        "一行一个：文件夹（往里找 exe）或者某个 exe 的完整路径（直接收它）。\n"
        "填目录时，命令行工具和子进程（7z / cmake / BugReport / updater 这类）不算应用，会被跳过；\n"
        "真想要其中哪一个，用「添加应用…」点名指到那个 exe。\n"
        "开始菜单那些快捷方式不用往这儿加，本来就扫。"));
    auto* rootRow = new QHBoxLayout;
    rootRow->setSpacing(12);
    auto* addRoot = new QPushButton("添加目录…", appDirs);
    addRoot->setObjectName(QStringLiteral("addAppDir"));
    auto* addExe = new QPushButton("添加应用…", appDirs);
    addExe->setObjectName(QStringLiteral("addAppExe"));
    addExe->setToolTip(QStringLiteral("直接挑一个 exe：它一定进应用列表，不做\u201c像不像主程序\u201d那两道判断"));
    auto* delRoot = new QPushButton("移除选中", appDirs);
    delRoot->setObjectName(QStringLiteral("removeAppRoot"));
    rootRow->addWidget(m_appRoots, 1);
    auto* rootBtns = new QVBoxLayout;
    rootBtns->setSpacing(8);
    rootBtns->addWidget(addRoot);
    rootBtns->addWidget(addExe);
    rootBtns->addWidget(delRoot);
    rootBtns->addStretch(1);
    rootRow->addLayout(rootBtns);
    appLay->addRow("项目列表：", rootRow);
    m_appDepth = new QSpinBox(appDirs);
    m_appDepth->setObjectName(QStringLiteral("settingsAppDepth"));
    m_appDepth->setRange(0, 6);
    m_appDepth->setSuffix(QStringLiteral(" 层"));
    m_appDepth->setToolTip(QStringLiteral("只对填进来的**目录**起作用。0 = 只看这个目录本身；3 = 往里三层里的 exe 都算。"));
    appLay->addRow("往里找几层：", m_appDepth);
    m_appInfo = new QLabel(appDirs);
    m_appInfo->setObjectName("muted");
    m_appInfo->setWordWrap(true);
    appLay->addRow("当前：", m_appInfo);
    m_applyIndex = new QPushButton(QStringLiteral("应用索引设置"), appDirs);
    m_applyIndex->setObjectName(QStringLiteral("applyIndex"));
    appLay->addRow(QString(), m_applyIndex);
    connect(addRoot, &QPushButton::clicked, this, &ConfigPage::addAppRoot);
    connect(addExe, &QPushButton::clicked, this, &ConfigPage::addAppExe);
    connect(delRoot, &QPushButton::clicked, this, &ConfigPage::removeAppRoot);

    auto* data = new QGroupBox("数据", this);
    auto* dataLay = new QFormLayout(data);
    m_dataLayout = dataLay;
    const auto pathRow = [data, dataLay](QLineEdit** member, const QString& label,
                                      const QString& defaultDir, const QString& what, bool isCap) {
        auto* row = new QWidget(data);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(8);
        auto* e = new QLineEdit(row);
        e->setPlaceholderText(QDir::toNativeSeparators(defaultDir));
        e->setToolTip(QString("%1放这里。留空 = %2").arg(what, QDir::toNativeSeparators(defaultDir)));
        auto* browse = new QPushButton("浏览…", row);
        auto* open = new QPushButton("打开", row);
        open->setToolTip(QString("在资源管理器里打开%1现在存放的目录").arg(what));
        QObject::connect(browse, &QPushButton::clicked, row, [e, what, defaultDir] {
            // 没填的时候从"以后会存过去的那个地方"开始挑，而不是数据目录
            const QString start = e->text().trimmed().isEmpty()
                                      ? QDir::toNativeSeparators(defaultDir) : e->text().trimmed();
            const QString got = QFileDialog::getExistingDirectory(e, QString("选%1的存放目录").arg(what), start);
            if (!got.isEmpty())
                e->setText(QDir::toNativeSeparators(got));
        });
        QObject::connect(open, &QPushButton::clicked, row, [isCap] {
            const QString dir = isCap ? Utils::capturesDir() : Utils::recordingsDir();
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        rl->addWidget(e, 1);
        rl->addWidget(browse);
        rl->addWidget(open);
        *member = e;
        dataLay->addRow(label, row);
    };
    pathRow(&m_capDir, "截图存放：", Utils::mediaRoot() + "/captures", QStringLiteral("截图"), true);
    pathRow(&m_recDir, "录像存放：", Utils::mediaRoot() + "/recordings", QStringLiteral("录像"), false);
    m_dirInfo = new QLabel(data);
    m_dirInfo->setObjectName("muted");
    m_dirInfo->setWordWrap(true);
    dataLay->addRow("当前：", m_dirInfo);
    m_applyPaths = new QPushButton(QStringLiteral("应用存放目录"), data);
    m_applyPaths->setObjectName(QStringLiteral("applyPaths"));
    dataLay->addRow(QString(), m_applyPaths);
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    auto* openBtn = new QPushButton("打开数据目录", data);
    auto* clearBtn = new QPushButton("清除索引缓存", data);
    openBtn->setObjectName(QStringLiteral("settingsOpenData"));
    clearBtn->setObjectName(QStringLiteral("settingsClearIndex"));
    btnRow->addWidget(openBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch(1);
    dataLay->addRow(btnRow);

    // ---- 「外观」那一页：夜间 / 日间 从通用那张长列表里搬到这里（他 2026-10-20 点名）----
    // 搬完，通用那一行「外观：」的标签就没了 —— 判据盯着"外观那一行只有一颗、而且它在
    // 外观这一页上"，别哪天又被人加回通用。
    auto* appearancePage = new QWidget(tabs);
    appearancePage->setObjectName(QStringLiteral("cfgBody"));
    auto* appearanceLayBox = new QVBoxLayout(appearancePage);
    appearanceLayBox->setContentsMargins(0, 0, 4, 16);
    appearanceLayBox->setSpacing(14);
    const auto makeSection = [appearancePage, appearanceLayBox](const QString& title) {
        auto* section = new QGroupBox(title, appearancePage);
        section->setObjectName(QStringLiteral("appearanceSection"));
        auto* layout = new QVBoxLayout(section);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        appearanceLayBox->addWidget(section);
        return layout;
    };
    auto* appearanceLay = makeSection(QStringLiteral("界面"));
    auto* appearance = appearanceLay->parentWidget();
    // 外观：夜间 / 日间。拨一下就当场生效（Theme::setMode 会重刷调色板、两份弹层样式表，
    // 并给每个顶层窗口发一次 ThemeChange），不用按保存、也不用重启。
    auto* themeRow = new QWidget(appearance);
    auto* themeLay = new QHBoxLayout(themeRow);
    themeLay->setContentsMargins(0, 0, 0, 0);
    themeLay->setSpacing(8);
    m_themeDark = new ModePreview(QStringLiteral("夜间"), true, themeRow);
    m_themeLight = new ModePreview(QStringLiteral("日间"), false, themeRow);
    m_themeDark->setChecked(Theme::current() == Theme::Mode::Dark);
    m_themeLight->setChecked(Theme::current() == Theme::Mode::Light);
    themeLay->addWidget(m_themeLight);
    themeLay->addWidget(m_themeDark);
    themeRow->setToolTip(QStringLiteral(
        "基础主题可快捷切换夜间与日间，黑、灰、白在下方主题列表中选择。\n"
        "自带颜色的第三方主题使用包内配色；切换立即生效。插件自行绘制的内容由插件负责。"));
    addAppearanceRow(appearanceLay, QStringLiteral("界面配色"), QStringLiteral("切换后立即生效。"),
                     themeRow, SettingsGlyph::Sun);
    connect(m_themeDark, &QRadioButton::toggled, this,
            [this](bool on) { if (on) {
                Theme::setMode(Theme::Mode::Dark);
                syncThemeControls();
            } });
    connect(m_themeLight, &QRadioButton::toggled, this,
            [this](bool on) { if (on) {
                Theme::setMode(Theme::Mode::Light);
                syncThemeControls();
            } });

    auto* packageRow = new QWidget(appearance);
    auto* packageLay = new QHBoxLayout(packageRow);
    packageLay->setContentsMargins(0, 0, 0, 0);
    m_themePackages = new SettingsComboBox(packageRow);
    m_themePackages->setObjectName(QStringLiteral("themePackages"));
    m_themePackages->setMinimumContentsLength(11);
    m_themePackages->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* importTheme = new QPushButton(QStringLiteral("导入 ZIP…"), packageRow);
    importTheme->setObjectName(QStringLiteral("settingsImportTheme"));
    auto* removeTheme = new QPushButton(QStringLiteral("删除主题"), packageRow);
    removeTheme->setObjectName(QStringLiteral("settingsRemoveTheme"));
    packageLay->addWidget(m_themePackages, 1);
    packageLay->addWidget(importTheme);
    packageLay->addWidget(removeTheme);
    addAppearanceRow(appearanceLay, QStringLiteral("启动器主题"),
                     QStringLiteral("选择内置主题，或导入自定义主题包。"), packageRow, SettingsGlyph::Palette);
    m_themePackageInfo = new QLabel(appearance);
    m_themePackageInfo->setTextFormat(Qt::PlainText);
    m_themePackageInfo->setObjectName(QStringLiteral("muted"));
    m_themePackageInfo->setWordWrap(true);
    m_themePackageInfo->setContentsMargins(13, 0, 13, 8);
    appearanceLay->addWidget(m_themePackageInfo);

    auto* launcherLayout = makeSection(QStringLiteral("启动器布局"));
    auto* launcher = launcherLayout->parentWidget();
    m_gridColumns = new QSpinBox(launcher);
    m_gridColumns->setRange(4, 8);
    m_gridColumns->setSuffix(QStringLiteral(" 列"));
    m_gridColumns->setObjectName(QStringLiteral("gridColumns"));
    m_gridRows = new QSpinBox(launcher);
    m_gridRows->setObjectName(QStringLiteral("gridRows"));
    m_gridRows->setRange(2,6);m_gridRows->setSuffix(QStringLiteral(" 行"));
    auto* gridSize=new QWidget(launcher);auto* gridSizeLayout=new QHBoxLayout(gridSize);
    gridSizeLayout->setContentsMargins(0,0,0,0);
    gridSizeLayout->setSpacing(8);
    gridSizeLayout->addWidget(new GridPreview(m_gridRows, m_gridColumns, gridSize));
    gridSizeLayout->addWidget(m_gridRows);
    gridSizeLayout->addWidget(m_gridColumns);
    m_applyGrid = new QPushButton(QStringLiteral("应用网格"), gridSize);
    m_applyGrid->setObjectName(QStringLiteral("applyGrid"));
    gridSizeLayout->addWidget(m_applyGrid);
    addAppearanceRow(launcherLayout, QStringLiteral("每页网格"),
                     QStringLiteral("行数与列数一起应用。分组面板固定 3×3。"), gridSize,
                     SettingsGlyph::Grid);
    m_pageStart = new SettingsComboBox(launcher);
    m_pageStart->setObjectName(QStringLiteral("pageStart"));
    m_pageStart->addItem(QStringLiteral("每次打开回到首页"),false);
    m_pageStart->addItem(QStringLiteral("记住上次所在页"),true);
    addAppearanceRow(launcherLayout, QStringLiteral("打开面板时"),
                     QStringLiteral("自动保存，下次呼出时生效。"), m_pageStart, SettingsGlyph::Panels);
    auto* pinnedSection = new QWidget(appearancePage);
    auto* pinnedSectionLayout = new QVBoxLayout(pinnedSection);
    pinnedSectionLayout->setContentsMargins(0, 0, 0, 0);
    pinnedSectionLayout->setSpacing(6);
    appearanceLayBox->addWidget(pinnedSection);
    auto* pinned = new QFrame(pinnedSection);
    pinned->setObjectName(QStringLiteral("pinnedSection"));
    pinned->setAccessibleName(QStringLiteral("固定软件"));
    auto* pinnedLay = new QVBoxLayout(pinned);
    pinnedLay->setContentsMargins(12, 7, 12, 10);
    pinnedLay->setSpacing(6);
    m_pinnedApps = new QListWidget(pinned);
    m_pinnedApps->setObjectName(QStringLiteral("pinnedApps"));
    m_pinnedApps->setAccessibleName(QStringLiteral("固定软件列表"));
    m_pinnedApps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pinnedApps->setSpacing(kPinnedRowSpacing);
    connect(m_pinnedApps->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection& selected, const QItemSelection& deselected) {
        const auto updateRows = [this](const QItemSelection& items, bool active) {
            for (const auto& index : items.indexes()) {
                if (auto* row = m_pinnedApps->indexWidget(index)) {
                    row->setProperty("selected", active);
                    row->style()->unpolish(row);
                    row->style()->polish(row);
                    row->update();
                }
            }
        };
        updateRows(deselected, false);
        updateRows(selected, true);
    });
    auto* pinnedRow = new QWidget(pinnedSection);
    auto* pinnedRowLay = new QHBoxLayout(pinnedRow);
    pinnedRowLay->setContentsMargins(0, 0, 0, 0);
    auto* pinsTitle = new QLabel(QStringLiteral("固定软件"), pinnedRow);
    pinsTitle->setObjectName(QStringLiteral("settingRowTitle"));
    pinnedRowLay->addWidget(pinsTitle);
    m_pinnedCount = new QLabel(pinnedRow);
    m_pinnedCount->setObjectName(QStringLiteral("muted"));
    pinnedRowLay->addWidget(m_pinnedCount);
    auto* configMenu = new QMenu(pinnedRow);
    Theme::bindMenu(configMenu);
    configMenu->addAction(QStringLiteral("导入配置"), this, &ConfigPage::importHotaruConfig);
    configMenu->addAction(QStringLiteral("导出配置"), this, &ConfigPage::exportHotaruConfig);
    auto* configActions = new QPushButton(pinnedRow);
    configActions->setObjectName(QStringLiteral("settingsPinConfigActions"));
    configActions->setAccessibleName(QStringLiteral("固定配置操作"));
    configActions->setToolTip(configActions->accessibleName());
    configActions->setFixedSize(34, 32);
    configActions->setMenu(configMenu);
    auto* addPinned = new QPushButton(QStringLiteral("添加固定项"), pinnedRow);
    addPinned->setObjectName(QStringLiteral("pinnedAddButton"));
    addPinned->setMinimumHeight(30);
    addPinned->setCursor(Qt::PointingHandCursor);
    pinnedRowLay->addStretch(1);
    pinnedRowLay->addWidget(configActions);
    pinnedRowLay->addWidget(addPinned);
    pinnedSectionLayout->addWidget(pinnedRow);
    pinnedSectionLayout->addWidget(pinned);
    pinnedLay->addWidget(m_pinnedApps);
    m_pinnedAppsInfo = new QLabel(pinned);
    m_pinnedAppsInfo->setObjectName(QStringLiteral("muted"));
    m_pinnedAppsInfo->setWordWrap(true);
    pinnedLay->addWidget(m_pinnedAppsInfo);
    appearanceLayBox->addStretch(1);

    connect(importTheme, &QPushButton::clicked, this, &ConfigPage::importThemePackage);
    connect(removeTheme, &QPushButton::clicked, this, &ConfigPage::removeThemePackage);
    connect(m_themePackages, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ConfigPage::activateThemePackage);
    connect(addPinned, &QPushButton::clicked, this, &ConfigPage::addPinnedApp);
    auto* appearanceScroll = new QScrollArea(tabs);
    appearanceScroll->setObjectName(QStringLiteral("cfgScroll"));
    appearanceScroll->setFrameShape(QFrame::NoFrame);
    appearanceScroll->setWidgetResizable(true);
    appearanceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    appearanceScroll->setWidget(appearancePage);
    tabs->addTab(appearanceScroll, QString::fromUtf8("外观"));

    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto* btnLay = new QHBoxLayout(footer);
    btnLay->setContentsMargins(30, 8, 30, 8);
    btnLay->setSpacing(8);
    footer->setMinimumHeight(51);
    m_saveStatus = new QLabel(QStringLiteral("单项修改后自动保存，分组设置在本组应用。"), this);
    m_saveStatus->setObjectName(QStringLiteral("settingsSaveStatus"));
    m_saveStatus->setWordWrap(true);
    btnLay->addWidget(m_saveStatus, 1);
    m_discard = new QPushButton(QStringLiteral("放弃更改"), this);
    m_discard->setObjectName(QStringLiteral("discardSettings"));
    m_discard->setFocusPolicy(Qt::TabFocus);
    m_discard->setAutoDefault(false);
    btnLay->addWidget(m_discard);
    lay->addWidget(general);
    lay->addWidget(search);
    lay->addWidget(index);
    lay->addWidget(appDirs);
    lay->addWidget(downloads);
    lay->addWidget(shell);
    lay->addWidget(data);
    for (auto* form : {generalLay, searchLayout, indexLay, appLay, downloadsLayout,
                      shellLay, dataLay}) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setVerticalSpacing(9);
    }
    outer->addWidget(footer);

    for (auto* action : {m_applyGrid, m_applyIndex, m_applyPaths})
        action->setAutoDefault(false);
    connect(m_discard, &QPushButton::clicked, this, &ConfigPage::discardChanges);
    connect(m_applyGrid, &QPushButton::clicked, this, &ConfigPage::applyGrid);
    connect(m_applyIndex, &QPushButton::clicked, this, &ConfigPage::applyIndex);
    connect(m_applyPaths, &QPushButton::clicked, this, &ConfigPage::applyPaths);
    connect(openBtn, &QPushButton::clicked, this, &ConfigPage::onOpenDataDir);
    connect(clearBtn, &QPushButton::clicked, this, &ConfigPage::onClearIndexCache);
    m_installPath->setObjectName(QStringLiteral("settingsInstallPath"));
    m_proxy->setObjectName(QStringLiteral("settingsDownloadProxy"));
    m_storeMirrors->setObjectName(QStringLiteral("settingsStoreMirrors"));
    m_engine->setObjectName(QStringLiteral("settingsSearchEngine"));
    m_capDir->setObjectName(QStringLiteral("settingsCapturePath"));
    m_recDir->setObjectName(QStringLiteral("settingsRecordingPath"));
    refreshPluginOptions();
    load();

    const auto bindCheck = [this](QCheckBox* box, const char* key, bool fallback) {
        connect(box, &QCheckBox::clicked, this, [this, box, key, fallback](bool checked) {
            if (!saveIni({{QLatin1String(key), checked}})) {
                const QSignalBlocker blocker(box);
                box->setChecked(QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                                    .value(QLatin1String(key), fallback).toBool());
            }
        });
    };
    bindCheck(m_favorites, "ui/favoritesOnEmpty", true);
    bindCheck(m_autoUpdate, "store/autoUpdate", false);
    bindCheck(m_archiveProgress, "ui/archiveProgress", false);
    connect(m_engine, &QComboBox::activated, this, [this] {
        const auto saved = QSettings(Utils::dataDir() + "/config.ini", QSettings::IniFormat)
                               .value(QStringLiteral("web/engine"), QStringLiteral("baidu"));
        if (m_engine->currentIndex() == qMax(0, m_engine->findData(saved)))
            return;
        if (saveIni({{QStringLiteral("web/engine"), m_engine->currentData()}}))
            return;
        const QSignalBlocker blocker(m_engine);
        m_engine->setCurrentIndex(qMax(0, m_engine->findData(saved)));
    });
    connect(m_pageStart, &QComboBox::activated, this, [this] {
        auto config = PinnedApps::config();
        const bool selected = m_pageStart->currentData().toBool();
        if (config.rememberPage == selected)
            return;
        config.rememberPage = selected;
        QString error;
        if (PinnedApps::saveConfig(config, &error)) {
            showResult(QStringLiteral("已保存。下次呼出时使用新的页面策略。"));
        } else {
            const QSignalBlocker blocker(m_pageStart);
            m_pageStart->setCurrentIndex(PinnedApps::config().rememberPage ? 1 : 0);
            showResult(error, true);
        }
    });
    connect(m_autostart, &QCheckBox::clicked, this, [this](bool checked) {
        QString error;
        const bool written = Platform::setAutostart(checked, &error);
        const QSignalBlocker blocker(m_autostart);
        m_autostart->setChecked(Platform::autostartEnabled());
        const bool applied = written && m_autostart->isChecked() == checked;
        showResult(applied ? QStringLiteral("已保存登录启动设置。")
                           : error.isEmpty() ? QStringLiteral("登录启动设置未生效。") : error, !applied);
        if (!written)
            QMessageBox::warning(this, QStringLiteral("登录启动"), error);
    });
    connect(m_shellMenu, &QCheckBox::clicked, this, [this](bool checked) {
        if (BuildChannel::development || checked == ShellMenu::intent())
            return;
        ShellMenu::setIntent(checked);
        const QString result = ShellMenu::syncWithPlugin();
        refreshShellState();
        if (!result.isEmpty())
            m_shellInfo->setText(result);
        const bool recorded = ShellMenu::intent() == checked;
        showResult(recorded ? QStringLiteral("已记录右键菜单偏好，实际集成状态见上方。")
                            : QStringLiteral("未能保存右键菜单设置。"), !recorded);
    });
    const auto bindText = [this](QLineEdit* field, const char* key) {
        connect(field, &QLineEdit::textEdited, this, [this, field] {
            field->setModified(true);
            updateDraftStatus();
        });
        connect(field, &QLineEdit::returnPressed, this, [this, field, key] {
            if (field->isModified())
                saveText(field, key);
        });
        connect(field, &QLineEdit::editingFinished, this, [this, field, key] {
            if (!field->isModified())
                return;
            const QString pending = field->text();
            // 窗口关闭或主动放弃时，失焦不能提交草稿。
            QTimer::singleShot(0, this, [this, field, key, pending] {
                if (window()->isVisible() && field->isModified() && field->text() == pending)
                    saveText(field, key);
            });
        });
    };
    bindText(m_installPath, "install/path");
    bindText(m_proxy, "net/proxy");
    bindText(m_storeMirrors, "store/mirrors");

    for (auto* spin : {m_gridRows, m_gridColumns, m_refreshHours, m_maxEntries, m_appDepth})
        connect(spin, &QSpinBox::valueChanged, this, &ConfigPage::updateDraftStatus);
    connect(m_deep, &QCheckBox::toggled, this, &ConfigPage::updateDraftStatus);
    for (auto* field : {m_capDir, m_recDir})
        connect(field, &QLineEdit::textChanged, this, &ConfigPage::updateDraftStatus);
    connect(m_appRoots->model(), &QAbstractItemModel::rowsInserted, this, &ConfigPage::updateDraftStatus);
    connect(m_appRoots->model(), &QAbstractItemModel::rowsRemoved, this, &ConfigPage::updateDraftStatus);
    updateDraftStatus();

    m_controlStyle = new SettingsControlStyle(this);
    // 明暗切换直接更新控件颜色，避免用旧主题的动画帧绘制文字和背景。
    m_controlStyle->setAnimationsEnabled(false);
    updateControlTheme();
    for (auto* child : findChildren<QWidget*>()) {
        // 菜单和标准弹窗继续走 Hotaru 的基础配色。
        if (qobject_cast<QPushButton*>(child) || qobject_cast<QCheckBox*>(child)
            || qobject_cast<QComboBox*>(child) || qobject_cast<QSpinBox*>(child)
            || qobject_cast<QLineEdit*>(child))
            child->setStyle(m_controlStyle);
    }
}

void ConfigPage::updateControlTheme()
{
    if (!m_controlStyle)
        return;
    const auto colors = SettingsPalette::current();
    m_controlStyle->applyTheme(colors);
    setStyleSheet(colors.expand(kConfigLayoutQss));
    for (auto* button : findChildren<QPushButton*>()) {
        const QString name = button->objectName();
        if (name == QLatin1String("settingsImportTheme"))
            button->setIcon(settingsIcon(SettingsGlyph::Upload, QColor(colors.text)));
        else if (name == QLatin1String("pinnedAddButton"))
            button->setIcon(settingsIcon(SettingsGlyph::Plus, QColor(colors.onPrimary)));
        else if (name == QLatin1String("pinActions") || name == QLatin1String("settingsPinConfigActions"))
            button->setIcon(settingsIcon(SettingsGlyph::More, QColor(colors.text)));
    }
}

void ConfigPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ThemeChange && m_themePackageInfo) {
        updateControlTheme();
        syncThemeControls();
    }
}

void ConfigPage::load()
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const PinnedApps::LauncherConfig hotaruCfg = PinnedApps::config();
    m_autostart->setChecked(Platform::autostartEnabled());
    const QString configuredHotkey = cfg.value("general/hotkey", BuildChannel::defaultHotkey()).toString();
    const bool doubleCtrl = configuredHotkey.compare(QStringLiteral("Ctrl+Ctrl"), Qt::CaseInsensitive) == 0;
    m_hotkey->setHotkeyText(doubleCtrl ? QStringLiteral("Ctrl+Ctrl") : configuredHotkey);
    m_hotkeyButton->setText(doubleCtrl ? QStringLiteral("双击 Ctrl（点击修改）")
                                       : (m_hotkey->hotkeyText().isEmpty()
                                              ? QStringLiteral("点击录入快捷键…")
                                              : m_hotkey->hotkeyText()));
    m_hotkeyButton->setEnabled(true);
    updateHotkeyStatus();
    m_installPath->setText(cfg.value("install/path", Platform::defaultInstallRoot()).toString());
    m_proxy->setText(cfg.value("net/proxy").toString());
    m_storeMirrors->setText(cfg.value("store/mirrors").toString());
    m_autoUpdate->setChecked(cfg.value("store/autoUpdate", false).toBool());
    const QString engine = cfg.value("web/engine", "baidu").toString();
    const int idx = m_engine->findData(engine);
    m_engine->setCurrentIndex(idx < 0 ? 0 : idx);
    m_favorites->setChecked(cfg.value("ui/favoritesOnEmpty", true).toBool());
    m_gridColumns->setValue(hotaruCfg.gridColumns);
    m_gridRows->setValue(hotaruCfg.gridRows);
    m_pageStart->setCurrentIndex(hotaruCfg.rememberPage?1:0);
    m_deep->setChecked(cfg.value("index/deep", true).toBool());
    m_refreshHours->setValue(cfg.value("index/refreshHours", 6).toInt());
    m_maxEntries->setValue(cfg.value("index/maxEntries", 5000000).toInt());
    m_appDepth->setValue(cfg.value("apps/depth", 3).toInt());
    m_appRoots->clear();
    for (const QString& r : cfg.value("apps/roots").toStringList())
        if (!r.trimmed().isEmpty())
            new QListWidgetItem(QDir::toNativeSeparators(r), m_appRoots);
    m_appInfo->setText(QStringLiteral("%1 项 · 其中目录 %2 个（往里 %3 层只对目录有效）· 应用索引设置后重扫")
                           .arg(m_appRoots->count()).arg(dirItemCount(m_appRoots))
                           .arg(m_appDepth->value()));
    reloadPaths();
    m_archiveProgress->setChecked(cfg.value("ui/archiveProgress", false).toBool());
    refreshShellState();
    m_savedRows = hotaruCfg.gridRows;
    m_savedColumns = hotaruCfg.gridColumns;
    m_savedIndex = indexValues();
    for (auto* field : {m_installPath, m_proxy, m_storeMirrors})
        field->setModified(false);
    refreshAppearance();
    updateDraftStatus();
}

void ConfigPage::refreshShellState()
{
    m_shellMenu->setChecked(ShellMenu::intent());
    // 三件事分开说：勾是你想要的，注册表里有没有是另一回事 —— 提供右键那几项的
    // 「右键动作」插件被卸载/禁用了，勾还留着，但右键里此刻没有 ltool。
    m_shellInfo->setText(BuildChannel::development ? QStringLiteral("开发版不修改资源管理器菜单。") : ShellMenu::installed()
                              ? QStringLiteral("已集成。%1").arg(ShellMenu::locationHint())
                              : ShellMenu::intent()
                                    ? QStringLiteral("你勾了，但「右键动作」插件不在了（卸载或禁用）—— "
                                                     "右键里此刻没有 Hotaru，装回来会自动写上。%1")
                                          .arg(ShellMenu::locationHint())
                                    : QStringLiteral("未集成（默认）。切换开关后应用。"));

}

void ConfigPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshPluginOptions();
    refreshShellState();
    refreshPathInfo();
    updateDraftStatus();
}

void ConfigPage::refreshPluginOptions()
{
    const auto pending = PluginStore::pendingRemoves();
    const auto active = [&pending](const QString& id) {
        const auto* plugin = PluginHost::meta(id);
        return plugin && plugin->ok() && !PluginHost::isDisabled(id)
            && !pending.contains(QFileInfo(plugin->file).fileName());
    };
    const bool winget = active(QStringLiteral("software")) || active(QStringLiteral("language"));
    const bool downloader = active(QStringLiteral("downloads"));
    m_downloadsLayout->setRowVisible(m_installRow, winget || downloader);
    m_downloadsLayout->setRowVisible(m_installInfo, winget || downloader);
    QStringList usage;
    if (winget)
        usage << QStringLiteral("用于 winget 安装第三方工具。清空后由安装器决定位置。");
    if (downloader)
        usage << QStringLiteral("兼容下载器使用此目录作为默认保存位置。");
    m_installInfo->setText(usage.join(QLatin1Char(' ')));
    m_shellGroup->setVisible(ShellMenu::providerPresent());
    const bool capture = active(QStringLiteral("capture"));
    const bool recording = active(QStringLiteral("record"));
    m_dataLayout->setRowVisible(m_capDir->parentWidget(), capture);
    m_dataLayout->setRowVisible(m_recDir->parentWidget(), recording);
    m_dataLayout->setRowVisible(m_dirInfo, capture || recording);
    m_dataLayout->setRowVisible(m_applyPaths, capture || recording);
}

void ConfigPage::refreshAppearance()
{
    const auto hotaruCfg = PinnedApps::config();
    {
        const QSignalBlocker blocker(m_themePackages);
        m_themePackages->clear();
        const QString current = Theme::currentPackageId();
        int selected = 0;
        for (const Theme::Package& p : Theme::packages()) {
            const QString label = p.valid ? QStringLiteral("%1 · %2").arg(p.name, p.version)
                                          : QStringLiteral("%1 · 无效：%2").arg(p.id, p.error);
            m_themePackages->addItem(label, p.id);
            if (p.id == current)
                selected = m_themePackages->count() - 1;
        }
        m_themePackages->setCurrentIndex(selected);
    }
    syncThemeControls();

    m_pinnedApps->clear();
    const auto pinnedApps = hotaruCfg.pinnedApps;
    m_pinnedCount->setText(QStringLiteral("· %1 项").arg(pinnedApps.size()));
    m_pinnedApps->setVisible(!pinnedApps.isEmpty());
    m_pinnedApps->setFixedHeight(qMin<qsizetype>(pinnedApps.size(), 4)
                                 * (kPinnedRowHeight + 2 * kPinnedRowSpacing));
    for (const PinnedApp& app : pinnedApps) {
        auto* item = new QListWidgetItem(m_pinnedApps);
        item->setData(Qt::UserRole, app.id);
        const QString kind = PinnedApps::targetType(app) == QLatin1String("folder")
            ? QStringLiteral("磁盘文件夹") : QStringLiteral("程序或脚本");
        item->setSizeHint(QSize(0, kPinnedRowHeight));
        item->setData(Qt::AccessibleTextRole, app.title);
        QString scope=QStringLiteral("主面板");
        for (const auto& group:hotaruCfg.groups) if (group.id==app.groupId) scope=QStringLiteral("分组：")+group.title;
        const int rows=app.groupId.isEmpty()?hotaruCfg.gridRows:3;
        item->setToolTip(app.targetPath);
        auto* row = new QFrame(m_pinnedApps);
        row->setObjectName(QStringLiteral("pinnedAppRow"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(10, 4, 9, 4);
        layout->setSpacing(8);
        auto* tile = new SettingsIconTile(PinnedApps::targetType(app) == QLatin1String("folder")
                                            ? SettingsGlyph::Folder : SettingsGlyph::Terminal, row);
        layout->addWidget(tile);
        auto* labels = new QVBoxLayout;
        labels->setSpacing(1);
        auto* title = new QLabel(app.title, row);
        title->setTextFormat(Qt::PlainText);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        title->setObjectName(QStringLiteral("settingRowTitle"));
        auto* meta = new QLabel(QStringLiteral("%1 · %2 · 第 %3 页 %4 行 %5 列")
                       .arg(kind, scope).arg(app.gridRow / rows + 1).arg(app.gridRow % rows + 1).arg(app.gridColumn + 1), row);
        meta->setTextFormat(Qt::PlainText);
        meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        meta->setObjectName(QStringLiteral("muted"));
        labels->addWidget(title);
        labels->addWidget(meta);
        layout->addLayout(labels, 1);
        auto* status = new QLabel(app.enabled ? QStringLiteral("已启用") : QStringLiteral("已停用"), row);
        status->setObjectName(QStringLiteral("pinStatus"));
        status->setProperty("active", app.enabled);
        layout->addWidget(status, 0, Qt::AlignVCenter);
        auto* edit = new QPushButton(QStringLiteral("编辑"), row);
        edit->setObjectName(QStringLiteral("pinEditButton"));
        edit->setAccessibleName(QStringLiteral("编辑 %1").arg(app.title));
        auto* more = new QPushButton(row);
        more->setObjectName(QStringLiteral("pinActions"));
        more->setFixedSize(34, 32);
        more->setIcon(settingsIcon(SettingsGlyph::More, QColor(SettingsPalette::current().text)));
        more->setAccessibleName(QStringLiteral("%1的更多操作").arg(app.title));
        auto* menu = new QMenu(more);
        Theme::bindMenu(menu);
        const auto select = [this, item] { m_pinnedApps->setCurrentItem(item); };
        connect(edit, &QPushButton::clicked, this, [this, select] { select(); editPinnedApp(); });
        menu->addAction(QStringLiteral("格位…"), this, [this, select] { select(); movePinnedUp(); });
        menu->addAction(app.enabled ? QStringLiteral("禁用") : QStringLiteral("启用"), this,
                        [this, select] { select(); togglePinnedApp(); });
        menu->addAction(QStringLiteral("移除"), this, [this, select] { select(); removePinnedApp(); });
        more->setMenu(menu);
        for (auto* action : {edit, more}) {
            action->setAutoDefault(false);
            if (m_controlStyle)
                action->setStyle(m_controlStyle);
            layout->addWidget(action, 0, Qt::AlignVCenter);
        }
        m_pinnedApps->setItemWidget(item, row);
    }
    m_pinnedAppsInfo->setText(pinnedApps.isEmpty()
        ? QStringLiteral("暂无固定项。可以添加程序、快捷方式、脚本或文件夹。")
        : QStringLiteral("隐藏项保留格位。"));
}

void ConfigPage::showResult(const QString& message, bool error)
{
    m_saveStatus->setText(message);
    m_saveStatus->setProperty("saveError", error);
    m_saveStatus->style()->unpolish(m_saveStatus);
    m_saveStatus->style()->polish(m_saveStatus);
    updateDraftStatus();
}

bool ConfigPage::saveIni(const QVariantMap& values, bool notify)
{
    QSettings config(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    QVariantMap previous;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto existing = config.value(it.key());
        if (existing != it.value())
            previous.insert(it.key(), existing);
    }
    if (previous.isEmpty())
        return true;
    for (auto it = previous.cbegin(); it != previous.cend(); ++it)
        config.setValue(it.key(), values.value(it.key()));
    config.sync();
    if (config.status() != QSettings::NoError) {
        // 写入失败时同步恢复内存缓存，当前进程内的其他配置消费者也继续使用原值。
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (it.value().isValid())
                config.setValue(it.key(), it.value());
            else
                config.remove(it.key());
        }
        config.sync();
        showResult(QStringLiteral("无法写入设置，请检查数据目录。"), true);
        return false;
    }
    if (notify)
        showResult(QStringLiteral("已保存。"));
    return true;
}

void ConfigPage::saveText(QLineEdit* field, const char* key)
{
    const QString value = field->text().trimmed();
    field->setModified(false);
    if (saveIni({{QLatin1String(key), value}})) {
        field->setText(value);
    } else {
        QSettings config(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        const QString fallback = field == m_installPath ? Platform::defaultInstallRoot() : QString();
        field->setText(config.value(QLatin1String(key), fallback).toString());
    }
    updateDraftStatus();
}

QVariantMap ConfigPage::indexValues() const
{
    QStringList roots;
    for (int index = 0; index < m_appRoots->count(); ++index) {
        const QString path = m_appRoots->item(index)->text().trimmed();
        if (!path.isEmpty())
            roots.append(QDir::fromNativeSeparators(path));
    }
    return {{QStringLiteral("index/deep"), m_deep->isChecked()},
            {QStringLiteral("index/maxEntries"), m_maxEntries->value()},
            {QStringLiteral("index/refreshHours"), m_refreshHours->value()},
            {QStringLiteral("apps/roots"), roots},
            {QStringLiteral("apps/depth"), m_appDepth->value()}};
}

void ConfigPage::updateDraftStatus()
{
    const bool grid = m_gridRows->value() != m_savedRows || m_gridColumns->value() != m_savedColumns;
    const bool index = indexValues() != m_savedIndex;
    const bool paths = pathChanged(m_capDir, m_savedCapturePath) || pathChanged(m_recDir, m_savedRecordingPath);
    const bool text = m_installPath->isModified() || m_proxy->isModified() || m_storeMirrors->isModified();
    m_applyGrid->setEnabled(grid);
    m_applyIndex->setEnabled(index);
    m_applyPaths->setEnabled(paths);
    m_discard->setVisible(grid || index || paths || text);
}

void ConfigPage::discardChanges()
{
    // 清除编辑标记，后续失焦事件不会提交已放弃的值。
    for (auto* field : {m_installPath, m_proxy, m_storeMirrors})
        field->setModified(false);
    load();
    showResult(QStringLiteral("已放弃未应用的更改。"));
}

void ConfigPage::applyGrid()
{
    m_gridRows->interpretText();
    m_gridColumns->interpretText();
    auto config = PinnedApps::config();
    config.gridRows = m_gridRows->value();
    config.gridColumns = m_gridColumns->value();
    QString error;
    if (!PinnedApps::saveConfig(config, &error)) {
        const auto saved = PinnedApps::config();
        m_savedRows = saved.gridRows;
        m_savedColumns = saved.gridColumns;
        m_gridRows->setValue(m_savedRows);
        m_gridColumns->setValue(m_savedColumns);
        updateDraftStatus();
        m_pinnedAppsInfo->setText(error);
        showResult(error, true);
        return;
    }
    const auto saved = PinnedApps::config();
    m_savedRows = saved.gridRows;
    m_savedColumns = saved.gridColumns;
    refreshAppearance();
    showResult(QStringLiteral("已应用网格。"));
}

void ConfigPage::applyIndex()
{
    for (auto* field : {m_maxEntries, m_refreshHours, m_appDepth})
        field->interpretText();
    const auto values = indexValues();
    if (values == m_savedIndex)
        return;

    QSettings config(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const auto roots = values.value(QStringLiteral("apps/roots")).toStringList();
    const auto previous = config.value(QStringLiteral("apps/roots")).toStringList();
    bool rootsChanged = roots.size() != previous.size();
    for (qsizetype index = 0; !rootsChanged && index < roots.size(); ++index)
        rootsChanged = roots.at(index).compare(QDir::fromNativeSeparators(previous.at(index)), Qt::CaseInsensitive) != 0;
    const bool rescan = rootsChanged
        || config.value(QStringLiteral("apps/depth"), 3).toInt() != m_appDepth->value()
        || config.value(QStringLiteral("index/deep"), true).toBool() != m_deep->isChecked()
        || config.value(QStringLiteral("index/maxEntries"), 5000000).toInt() != m_maxEntries->value()
        || config.value(QStringLiteral("index/refreshHours"), 6).toInt() != m_refreshHours->value();
    if (!saveIni(values))
        return;
    m_savedIndex = values;
    m_appInfo->setText(QStringLiteral("%1 项 · 其中目录 %2 个（往里 %3 层只对目录有效）")
                           .arg(m_appRoots->count()).arg(dirItemCount(m_appRoots)).arg(m_appDepth->value()));
    if (rescan)
        emit rescanRequested();
    showResult(rescan ? QStringLiteral("已应用索引设置，正在刷新索引。") : QStringLiteral("已应用索引设置。"));
}

void ConfigPage::applyPaths()
{
    QVariantMap changes;
    QStringList errors;
    const auto validate = [&](const QLineEdit* field, const QString& saved, const QString& key, const QString& title) {
        if (!pathChanged(field, saved))
            return;
        const QString path = field->text().trimmed();
        const QString problem = dirProblem(path);
        if (problem.isEmpty())
            changes.insert(key, path);
        else
            errors.append(title + problem);
    };
    const QString captureKey = QStringLiteral("paths/captures");
    validate(m_capDir, m_savedCapturePath, captureKey, QStringLiteral("截图目录"));
    validate(m_recDir, m_savedRecordingPath, QStringLiteral("paths/recordings"), QStringLiteral("录像目录"));
    if (!errors.isEmpty()) {
        const QString error = errors.join(QStringLiteral("；"));
        Utils::log(QStringLiteral("paths change refused: %1").arg(error));
        reloadPaths();
        const QString message = QStringLiteral("未应用：%1。仍使用原目录。").arg(error);
        m_dirInfo->setText(message);
        showResult(message, true);
        return;
    }
    if (changes.isEmpty() || !saveIni(changes))
        return;

    if (changes.contains(captureKey)) {
        CaptureHistory::reload();
        for (auto* window : QApplication::topLevelWidgets())
            if (window->inherits("PinWindow"))
                window->update();
    }
    reloadPaths();
    showResult(QStringLiteral("已应用存放目录。"));
}

bool ConfigPage::applyHotkey(const QString& sequence)
{
    const QString key = QStringLiteral("general/hotkey");
    const QString path = Utils::dataDir() + "/config.ini";
    const QString previous = QSettings(path, QSettings::IniFormat).value(key, BuildChannel::defaultHotkey()).toString();
    if (sequence.compare(previous, Qt::CaseInsensitive) == 0)
        return true;
    if (sequence != QStringLiteral("Ctrl+Ctrl")) {
        UINT modifiers = 0, vk = 0;
        if (!configKeySequenceToHotkey(QKeySequence(sequence), &modifiers, &vk)) {
            QMessageBox::warning(this, QStringLiteral("快捷键格式无效"),
                                 QStringLiteral("请输入修饰键加主键，例如 Alt+Space、Ctrl+K 或 Ctrl+Alt+K。"));
            return false;
        }
        const int probeId = 0x6f41;
        if (!RegisterHotKey(nullptr, probeId, modifiers | MOD_NOREPEAT, vk)) {
            QMessageBox::warning(this, QStringLiteral("快捷键已被占用"),
                                 QStringLiteral("系统拒绝注册这个组合，请更换一个快捷键。"));
            return false;
        }
        UnregisterHotKey(nullptr, probeId);
    }
    if (!saveIni({{key, sequence}}, false))
        return false;

    // 主程序在同一线程注册新热键，失败时回写原首选值。
    window()->setProperty(kSettingsHotkeyApplied, false);
    emit hotkeyChanged(sequence);
    const QString saved = QSettings(path, QSettings::IniFormat).value(key, BuildChannel::defaultHotkey()).toString();
    if (!window()->property(kSettingsHotkeyApplied).toBool() || saved.compare(sequence, Qt::CaseInsensitive) != 0) {
        if (saved.compare(sequence, Qt::CaseInsensitive) == 0 && !saveIni({{key, previous}}, false))
            return false;
        showResult(QStringLiteral("新快捷键未生效，请选择其他组合。"), true);
        return false;
    }
    showResult(QStringLiteral("已保存呼出快捷键。"));
    return true;
}

void ConfigPage::addPinnedApp()
{
    const QString path = choosePinnedTarget(this);
    if (path.isEmpty())
        return;
    if (!PinnedApps::supportsTarget(path)) {
        QMessageBox::warning(this, QStringLiteral("不支持的启动目标"),
                             QStringLiteral("请提供 exe、lnk、bat、cmd 文件或真实磁盘文件夹。"));
        return;
    }
    QString error;
    if (!PinnedApps::add(PinnedApps::fromTarget(path),-1,-1,&error)) {
        m_pinnedAppsInfo->setText(error);return;
    }
    refreshAppearance();
}

void ConfigPage::removePinnedApp()
{
    auto* item = m_pinnedApps->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    QString error;
    if (!PinnedApps::remove(id, &error))
        m_pinnedAppsInfo->setText(error);
    else
        refreshAppearance();
}

void ConfigPage::togglePinnedApp()
{
    auto* item = m_pinnedApps->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    auto apps = PinnedApps::load();
    for (PinnedApp& app : apps) {
        if (app.id == id) {
            app.enabled = !app.enabled;
            break;
        }
    }
    QString error;
    if (!PinnedApps::save(apps, &error))
        m_pinnedAppsInfo->setText(error);
    else
        refreshAppearance();
}

void ConfigPage::movePinnedUp() { editPinnedApp(); }
void ConfigPage::movePinnedDown() { editPinnedApp(); }

void ConfigPage::editPinnedApp()
{
    auto* item=m_pinnedApps->currentItem();if (!item) return;
    if (PinnedApps::editProperties(item->data(Qt::UserRole).toString(),this)) refreshAppearance();
}

void ConfigPage::openHotkeyCapture()
{
    auto* editor = new HotkeyCaptureEdit;
    HotkeyCaptureDialog captureDialog(editor, this);
    editor->setParent(&captureDialog);
    QDialog* dialog = &captureDialog;
    captureDialog.setObjectName(QStringLiteral("hotkeyCaptureDialog"));
    captureDialog.setWindowTitle(QStringLiteral("录入呼出快捷键"));
    captureDialog.setMinimumWidth(480);
    auto* lay = new QVBoxLayout(dialog);
    lay->setContentsMargins(24, 22, 24, 20);
    lay->setSpacing(12);
    auto* hint = new QLabel(QStringLiteral("点击输入框后按下组合键。支持 Alt+Space、Ctrl+K、Ctrl+Alt+K，也支持连续按两次 Ctrl。"), dialog);
    hint->setObjectName(QStringLiteral("modalHint"));
    hint->setWordWrap(true);
    lay->addWidget(hint);

    editor->setObjectName(QStringLiteral("hotkeyEditor"));
    editor->setMinimumHeight(48);
    // 二级弹窗每次都从“等待输入”开始，避免用户必须先手动清除旧组合。
    editor->clearHotkey();
    lay->addWidget(editor);
    auto* waiting = new QLabel(QStringLiteral("等待输入…"), dialog);
    waiting->setObjectName(QStringLiteral("modalHint"));
    lay->addWidget(waiting);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    auto* cancel = buttons->button(QDialogButtonBox::Cancel);
    ok->setText(QStringLiteral("使用此快捷键"));
    cancel->setText(QStringLiteral("取消"));
    ok->setObjectName(QStringLiteral("hotkeyPrimary"));
    cancel->setObjectName(QStringLiteral("hotkeySecondary"));
    lay->addWidget(buttons);

    auto refresh = [editor, waiting, ok] {
        if (editor->isDoubleCtrl()) {
            waiting->setText(QStringLiteral("已识别：双击 Ctrl"));
            ok->setEnabled(true);
        } else if (editor->keySequence().isEmpty()) {
            waiting->setText(QStringLiteral("等待输入…"));
            ok->setEnabled(false);
        } else {
            waiting->setText(QStringLiteral("已识别：%1").arg(editor->hotkeyText()));
            ok->setEnabled(true);
        }
    };
    editor->changed = refresh;
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, ok] {
        if (ok->isEnabled()) dialog->accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    refresh();
    emit hotkeyCaptureActive(true);
    HotkeyCaptureNativeFilter nativeFilter(editor);
    QCoreApplication::instance()->installNativeEventFilter(&nativeFilter);
    QTimer::singleShot(0, &captureDialog, [editor] {
        editor->setFocus(Qt::OtherFocusReason);
        editor->selectAll();
    });
    Theme::bindModal(dialog);
    const int result = dialog->exec();
    QCoreApplication::instance()->removeNativeEventFilter(&nativeFilter);
    emit hotkeyCaptureActive(false);
    if (result != QDialog::Accepted)
        return;
    const QString sequence = editor->isDoubleCtrl() ? QStringLiteral("Ctrl+Ctrl") : editor->hotkeyText();
    if (!applyHotkey(sequence))
        return;
    if (editor->isDoubleCtrl())
        m_hotkey->setDoubleCtrl();
    else
        m_hotkey->setHotkeyText(editor->hotkeyText());
    m_hotkeyButton->setText(m_hotkey->isDoubleCtrl()
                                ? QStringLiteral("双击 Ctrl（点击修改）")
                                : m_hotkey->hotkeyText());
    updateHotkeyStatus();
}

void ConfigPage::updateHotkeyStatus()
{
    if (!m_hotkeyStatus) return;
    if (m_hotkey && m_hotkey->isDoubleCtrl()) {
        m_hotkeyStatus->setText(QStringLiteral("快捷键可用：双击 Ctrl"));
        return;
    }
    const QKeySequence seq = m_hotkey ? m_hotkey->keySequence() : QKeySequence();
    UINT mods = 0, vk = 0;
    if (!configKeySequenceToHotkey(seq, &mods, &vk)) {
        m_hotkeyStatus->setText(seq.isEmpty() ? QStringLiteral("请输入快捷键")
                                               : QStringLiteral("格式无效：需要修饰键和主键"));
        return;
    }
    QSettings probeCfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const QString current = probeCfg.value("general/hotkey", BuildChannel::defaultHotkey()).toString();
    const QString text = seq.toString();
    if (current.compare(text, Qt::CaseInsensitive) == 0) {
        m_hotkeyStatus->setText(QStringLiteral("快捷键可用"));
        return;
    }
    const int probeId = 0x6f42;
    const bool registered = RegisterHotKey(nullptr, probeId, mods | MOD_NOREPEAT, vk);
    if (registered) {
        UnregisterHotKey(nullptr, probeId);
        m_hotkeyStatus->setText(QStringLiteral("快捷键可用"));
    } else {
        m_hotkeyStatus->setText(QStringLiteral("快捷键已被占用"));
    }
}

void ConfigPage::importThemePackage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入主题包"), QDir::homePath(),
        QStringLiteral("主题包 (*.zip);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!Theme::importPackage(path, &error)) {
        m_themePackageInfo->setText(QStringLiteral("导入失败：%1").arg(error));
        return;
    }
    refreshAppearance();
}

void ConfigPage::removeThemePackage()
{
    const QString id = m_themePackages->currentData().toString();
    if (id.isEmpty())
        return;
    QString error;
    if (!Theme::removePackage(id, &error))
        m_themePackageInfo->setText(error);
    else
        refreshAppearance();
}

void ConfigPage::activateThemePackage(int index)
{
    if (index < 0)
        return;
    const QString id = m_themePackages->itemData(index).toString();
    QString error;
    if (!Theme::activatePackage(id, &error)) {
        syncThemeControls();
        m_themePackageInfo->setText(QStringLiteral("切换失败：%1").arg(error));
        return;
    }
    syncThemeControls();
}

void ConfigPage::syncThemeControls()
{
    const auto package=Theme::currentPackage();
    const QSignalBlocker darkBlocker(m_themeDark), lightBlocker(m_themeLight), packageBlocker(m_themePackages);
    m_themePackages->setCurrentIndex(m_themePackages->findData(package.id));
    m_themeDark->setChecked(Theme::current()==Theme::Mode::Dark);
    m_themeLight->setChecked(Theme::current()==Theme::Mode::Light);
    m_themeDark->setEnabled(true);
    m_themeLight->setEnabled(true);
    m_themePackageInfo->setText(QStringLiteral("当前主题：%1。黑灰白为内置主题；第三方主题仅改变启动器；设置页使用独立的日间/夜间配色。")
        .arg(package.name));
}

void ConfigPage::importHotaruConfig()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入萤配置"), QDir::homePath(),
        QStringLiteral("JSON 配置 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!PinnedApps::importConfig(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    QString themeError;
    if (!Theme::activatePackage(PinnedApps::config().themeId, &themeError)) {
        QMessageBox::warning(this, QStringLiteral("主题未切换"), themeError);
    }
    const bool gridWasEdited = m_gridRows->value() != m_savedRows || m_gridColumns->value() != m_savedColumns;
    const auto imported = PinnedApps::config();
    m_savedRows = imported.gridRows;
    m_savedColumns = imported.gridColumns;
    if (!gridWasEdited) {
        m_gridRows->setValue(m_savedRows);
        m_gridColumns->setValue(m_savedColumns);
    }
    m_pageStart->setCurrentIndex(imported.rememberPage ? 1 : 0);
    refreshAppearance();
    updateDraftStatus();
    m_pinnedAppsInfo->setText(QStringLiteral("配置已导入。"));
}

void ConfigPage::exportHotaruConfig()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出萤配置"), QDir::homePath() + QStringLiteral("/hotaru-config.json"),
        QStringLiteral("JSON 配置 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!PinnedApps::exportConfig(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_pinnedAppsInfo->setText(QStringLiteral("配置已导出：%1").arg(path));
}

bool ConfigPage::pathChanged(const QLineEdit* field, const QString& saved) const
{
    return m_dataLayout->isRowVisible(field->parentWidget()) && field->text() != saved;
}

void ConfigPage::reloadPaths()
{
    m_savedCapturePath = QDir::toNativeSeparators(Utils::capturesDir());
    m_savedRecordingPath = QDir::toNativeSeparators(Utils::recordingsDir());
    m_capDir->setText(m_savedCapturePath);
    m_recDir->setText(m_savedRecordingPath);
    refreshPathInfo();
}

void ConfigPage::refreshPathInfo()
{
    QStringList summary;
    // 说明行不再重复整条路径（上面两个框里就是），只报数量和大小。
    // 截图要数两种名字：往期是 cap-*.png，手动"保存"那张是 capture-*.png
    if (m_dataLayout->isRowVisible(m_capDir->parentWidget())) {
        summary << QStringLiteral("截图 %1").arg(dirSummary(QDir::fromNativeSeparators(m_savedCapturePath),
            {QStringLiteral("cap-*.png"), QStringLiteral("capture-*.png")}));
        summary << QStringLiteral("置顶窗往期 %1 张").arg(CaptureHistory::count());
    }
    if (m_dataLayout->isRowVisible(m_recDir->parentWidget())) {
        summary << QStringLiteral("录像 %1").arg(dirSummary(QDir::fromNativeSeparators(m_savedRecordingPath),
            {QStringLiteral("rec-*.mp4")}));
    }
    m_dirInfo->setText(summary.isEmpty() ? QString{} : summary.join(QStringLiteral(" · "))
        + QStringLiteral("\n改目录只影响以后存的文件，已有的文件不会搬走。"));
}

void ConfigPage::onOpenDataDir()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(Utils::dataDir()));
}

void ConfigPage::onClearIndexCache()
{
    QFile::remove(Utils::dataDir() + "/deepindex.dat");
    QFile::remove(Utils::dataDir() + "/usnstate.dat");
    Utils::log("index cache cleared from settings");
    emit rescanRequested();
}

void ConfigPage::addAppRoot()
{
    const QString start = m_appRoots->count()
        ? m_appRoots->item(m_appRoots->count() - 1)->text()
        : QDir::rootPath();
    const QString got = QFileDialog::getExistingDirectory(this, QStringLiteral("选应用目录"), start);
    if (got.isEmpty())
        return;
    const QString nat = QDir::toNativeSeparators(got);
    for (int i = 0; i < m_appRoots->count(); ++i)
        if (m_appRoots->item(i)->text().compare(nat, Qt::CaseInsensitive) == 0)
            return;   // 同一个目录列两遍没有意义
    new QListWidgetItem(nat, m_appRoots);
}

void ConfigPage::addAppExe()
{
    // 点名加某个 exe：这一条不走"像不像主程序"那两道判断，加了就一定在应用列表里 ——
    // 目录扫描必然有误杀，这是给它留的出路（也是"这软件根本不在开始菜单"的那类东西）
    const QString last = m_appRoots->count()
        ? m_appRoots->item(m_appRoots->count() - 1)->text() : QString();
    const QFileInfo li(last);
    const QString start = last.isEmpty() ? QDir::rootPath()
                                         : (li.isDir() ? last : li.absolutePath());
    const QString got = QFileDialog::getOpenFileName(this, QStringLiteral("选要加进搜索的应用（exe）"),
                                                     start, QStringLiteral("程序 (*.exe)"));
    if (got.isEmpty())
        return;
    const QString nat = QDir::toNativeSeparators(got);
    for (int i = 0; i < m_appRoots->count(); ++i)
        if (m_appRoots->item(i)->text().compare(nat, Qt::CaseInsensitive) == 0)
            return;                       // 同一个 exe 列两遍没有意义
    new QListWidgetItem(nat, m_appRoots);
}

void ConfigPage::removeAppRoot()
{
    delete m_appRoots->currentItem();
}
