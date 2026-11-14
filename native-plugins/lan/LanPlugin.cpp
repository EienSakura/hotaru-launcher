#include "LanService.h"
#include "hotaru-native-plugin.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace HotaruLan {
namespace {
int featureFor(const QString& id)
{
    if (id == QStringLiteral("lan-clipboard"))
        return Clipboard;
    if (id == QStringLiteral("lan-files"))
        return Files;
    return 0;
}

QLabel* textLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QLabel* titleLabel(const QString& text, QWidget* parent)
{
    auto* label = textLabel(text, parent);
    auto font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() + 2);
    label->setFont(font);
    return label;
}

class DropPanel final : public QFrame {
public:
    explicit DropPanel(QWidget* parent) : QFrame(parent)
    {
        setAcceptDrops(true);
        setFrameShape(QFrame::StyledPanel);
    }
    std::function<void(QStringList)> dropped;
private:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        const auto urls = event->mimeData()->urls();
        if (!urls.isEmpty() && std::all_of(urls.cbegin(), urls.cend(), [](const QUrl& url) {
                return url.isLocalFile();
            }))
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override
    {
        QStringList paths;
        for (const auto& url : event->mimeData()->urls()) {
            if (!url.isLocalFile())
                return;
            paths.push_back(url.toLocalFile());
        }
        if (!paths.isEmpty() && dropped) {
            event->acceptProposedAction();
            dropped(paths);
        }
    }
};

QWidget* pageFor(Service& service, int feature, QWidget* parent)
{
    const bool files = feature == Files;
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget(scroll);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 12, 28, 24);
    layout->setSpacing(16);
    auto* title = titleLabel(files ? QStringLiteral("局域网文件分享") :
                                    QStringLiteral("局域网剪贴板共享"), page);
    layout->addWidget(title);
    layout->addWidget(textLabel(files ? QStringLiteral("选择同一专用网络中的设备，发送文件或文件夹。接收前需要确认。") :
                                       QStringLiteral("与已配对设备同步新复制的文本和图片。之前的内容不会补发。"), page));

    auto* enable = new QCheckBox(QStringLiteral("启用此插件"), page);
    auto* status = textLabel({}, page);
    auto* feedback = textLabel({}, page);
    layout->addWidget(enable);
    layout->addWidget(status);
    auto* diagnostics = new QPushButton(QStringLiteral("打开诊断日志目录"), page);
    layout->addWidget(diagnostics, 0, Qt::AlignLeft);
    QObject::connect(diagnostics, &QPushButton::clicked, page, [=, &service] {
        const auto directory = service.diagnosticDirectory();
        if (!QDir().mkpath(directory) || !QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
            feedback->setText(QStringLiteral("无法打开诊断日志目录。"));
    });

    auto* connection = new QFrame(page);
    connection->setFrameShape(QFrame::StyledPanel);
    auto* connectionLayout = new QVBoxLayout(connection);
    connectionLayout->setContentsMargins(16, 12, 16, 12);
    connectionLayout->addWidget(titleLabel(QStringLiteral("共享连接设置"), connection));
    connectionLayout->addWidget(textLabel(
        QStringLiteral("两项插件共用设备名和安全码。两台设备填写相同的码，建议生成长码。"), connection));
    auto* fields = new QFormLayout;
    auto* name = new QLineEdit(service.name(), connection);
    name->setMaxLength(48);
    auto* code = new QLineEdit(connection);
    code->setEchoMode(QLineEdit::Password);
    code->setMaxLength(128);
    auto* clipboardAlias = new QLineEdit(service.clipboardSearchAlias(), connection);
    clipboardAlias->setMaxLength(24);
    clipboardAlias->setPlaceholderText(QStringLiteral("留空关闭，例如 jtb"));
    fields->addRow(QStringLiteral("设备名"), name);
    fields->addRow(QStringLiteral("安全码"), code);
    if (!files)
        fields->addRow(QStringLiteral("剪贴板快捷词"), clipboardAlias);
    connectionLayout->addLayout(fields);
    auto* actions = new QHBoxLayout;
    auto* generate = new QPushButton(QStringLiteral("生成长码"), connection);
    auto* copy = new QPushButton(QStringLiteral("复制已保存的码"), connection);
    auto* save = new QPushButton(QStringLiteral("保存"), connection);
    actions->addWidget(generate);
    actions->addWidget(copy);
    actions->addStretch();
    actions->addWidget(save);
    connectionLayout->addLayout(actions);
    layout->addWidget(connection);
    layout->addWidget(feedback);

    auto* devices = new QComboBox(page);
    devices->setAccessibleName(QStringLiteral("已配对设备"));
    auto* discovered = new QListWidget(page);
    discovered->setObjectName(QStringLiteral("discoveredDevices"));
    discovered->setAccessibleName(QStringLiteral("已发现设备"));
    discovered->setMaximumHeight(110);
    auto* probeRow = new QHBoxLayout;
    auto* probeAddress = new QLineEdit(page);
    probeAddress->setPlaceholderText(QStringLiteral("手动探测 IPv4，例如 192.168.1.20"));
    auto* probe = new QPushButton(QStringLiteral("探测"), page);
    probeRow->addWidget(probeAddress, 1);
    probeRow->addWidget(probe);
    layout->addWidget(titleLabel(QStringLiteral("已发现设备"), page));
    layout->addWidget(discovered);
    layout->addLayout(probeRow);
    layout->addWidget(titleLabel(QStringLiteral("已配对设备"), page));
    layout->addWidget(devices);

    DropPanel* outgoing = nullptr;
    QLabel *outStatus = nullptr, *inStatus = nullptr, *pending = nullptr;
    QPushButton *sendFiles = nullptr, *sendFolder = nullptr, *retry = nullptr, *cancelOut = nullptr;
    QPushButton *accept = nullptr, *decline = nullptr, *cancelIn = nullptr;
    if (files) {
        outgoing = new DropPanel(page);
        auto* out = new QVBoxLayout(outgoing);
        out->addWidget(titleLabel(QStringLiteral("发送"), outgoing));
        out->addWidget(textLabel(QStringLiteral("也可以把文件或文件夹拖到此处。"), outgoing));
        outStatus = textLabel({}, outgoing);
        out->addWidget(outStatus);
        auto* row = new QHBoxLayout;
        sendFiles = new QPushButton(QStringLiteral("选文件"), outgoing);
        sendFolder = new QPushButton(QStringLiteral("选文件夹"), outgoing);
        retry = new QPushButton(QStringLiteral("重试"), outgoing);
        cancelOut = new QPushButton(QStringLiteral("取消"), outgoing);
        for (auto* button : {sendFiles, sendFolder, retry, cancelOut})
            row->addWidget(button);
        row->addStretch();
        out->addLayout(row);
        layout->addWidget(outgoing);

        auto* incoming = new QFrame(page);
        incoming->setFrameShape(QFrame::StyledPanel);
        auto* in = new QVBoxLayout(incoming);
        in->addWidget(titleLabel(QStringLiteral("接收"), incoming));
        inStatus = textLabel({}, incoming);
        pending = textLabel({}, incoming);
        in->addWidget(inStatus);
        in->addWidget(pending);
        auto* inRow = new QHBoxLayout;
        accept = new QPushButton(QStringLiteral("选择目录并接收"), incoming);
        decline = new QPushButton(QStringLiteral("拒绝"), incoming);
        cancelIn = new QPushButton(QStringLiteral("取消"), incoming);
        for (auto* button : {accept, decline, cancelIn})
            inRow->addWidget(button);
        inRow->addStretch();
        in->addLayout(inRow);
        layout->addWidget(incoming);

        const auto share = [&service, devices](const QStringList& paths) {
            if (!paths.isEmpty())
                service.share(devices->currentData().toByteArray(), paths);
        };
        outgoing->dropped = share;
        QObject::connect(sendFiles, &QPushButton::clicked, page, [page, share] {
            share(QFileDialog::getOpenFileNames(page, QStringLiteral("选择要发送的文件")));
        });
        QObject::connect(sendFolder, &QPushButton::clicked, page, [page, share] {
            const auto folder = QFileDialog::getExistingDirectory(page, QStringLiteral("选择要发送的文件夹"));
            if (!folder.isEmpty())
                share({folder});
        });
        QObject::connect(retry, &QPushButton::clicked, page, [&service, share] {
            share(service.transfers().retryPaths());
        });
        QObject::connect(accept, &QPushButton::clicked, page, [&service, page] {
            const auto target = QFileDialog::getExistingDirectory(page, QStringLiteral("选择接收目录"),
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            if (!target.isEmpty())
                service.transfers().accept(target);
        });
        QObject::connect(decline, &QPushButton::clicked, page, [&service] { service.transfers().decline(); });
        QObject::connect(cancelIn, &QPushButton::clicked, page, [&service] { service.transfers().cancelIncoming(); });
        QObject::connect(cancelOut, &QPushButton::clicked, page, [&service] { service.transfers().cancelOutgoing(); });
    }

    const auto refresh = [=, &service] {
        {
            QSignalBlocker block(enable);
            enable->setChecked(service.enabled(feature));
        }
        status->setText(service.status());
        code->setPlaceholderText(service.hasCode() ? QStringLiteral("已保存，留空保留当前安全码")
                                                   : QStringLiteral("填写至少 6 个字符"));
        copy->setEnabled(service.hasCode());
        const auto found = service.discoveredDevices();
        discovered->clear();
        for (const auto& item : found) {
            const auto suffix = QString::fromLatin1(item.id.toHex().right(8));
            QString state = QStringLiteral("等待自动认证");
            if (item.paired)
                state = QStringLiteral("已配对");
            else if (item.phase == Network::Phase::Connecting)
                state = QStringLiteral("正在连接");
            else if (item.phase == Network::Phase::Authenticating)
                state = QStringLiteral("正在认证");
            else if (item.phase == Network::Phase::Failed)
                state = QStringLiteral("连接未完成，等待重试");
            else if (item.phase == Network::Phase::Rejected)
                state = QStringLiteral("校验未通过，请核对安全码");
            discovered->addItem(QStringLiteral("%1 · %2 · %3")
                                    .arg(state)
                                    .arg(item.address)
                                    .arg(suffix));
        }
        if (found.isEmpty())
            discovered->addItem(QStringLiteral("暂未发现设备；可使用上方 IPv4 探测"));
        const auto selected = devices->currentData().toByteArray();
        QList<Service::Device> relevant;
        for (const auto& item : service.devices())
            if (item.features & feature)
                relevant.push_back(item);
        bool unchanged = devices->count() == relevant.size() && !relevant.isEmpty();
        for (qsizetype i = 0; unchanged && i < relevant.size(); ++i)
            unchanged = devices->itemData(int(i)).toByteArray() == relevant[i].id &&
                        devices->itemText(int(i)) == relevant[i].name;
        if (!unchanged) {
            QSignalBlocker block(devices);
            devices->clear();
            for (const auto& item : relevant)
                devices->addItem(item.name, item.id);
            if (relevant.isEmpty())
                devices->addItem(QStringLiteral("暂无支持此功能的设备"));
            const auto index = devices->findData(selected);
            if (index >= 0)
                devices->setCurrentIndex(index);
        }
        devices->setEnabled(!relevant.isEmpty());
        if (!files)
            return;
        const auto& transfers = service.transfers();
        outStatus->setText(transfers.outgoingStatus().isEmpty() ? QStringLiteral("暂无发送任务")
                                                               : transfers.outgoingStatus());
        inStatus->setText(transfers.incomingStatus().isEmpty() ? QStringLiteral("暂无接收任务")
                                                              : transfers.incomingStatus());
        pending->setText(transfers.pendingSummary());
        pending->setVisible(transfers.awaitingDecision());
        const bool canSend = service.enabled(Files) && !transfers.outgoing() && !relevant.isEmpty();
        outgoing->setAcceptDrops(canSend);
        sendFiles->setEnabled(canSend);
        sendFolder->setEnabled(canSend);
        retry->setEnabled(canSend && !transfers.retryPaths().isEmpty());
        cancelOut->setEnabled(transfers.outgoing());
        accept->setEnabled(transfers.awaitingDecision());
        decline->setEnabled(transfers.awaitingDecision());
        cancelIn->setEnabled(transfers.incoming() && !transfers.awaitingDecision());
    };

    QObject::connect(&service, &Service::changed, page, refresh);
    QObject::connect(enable, &QCheckBox::toggled, page, [=, &service](bool checked) {
        QString error;
        service.setEnabled(feature, checked, error);
        feedback->setText(error);
        refresh();
    });
    QObject::connect(generate, &QPushButton::clicked, page, [=] {
        const auto value = randomBytes(18).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        if (value.isEmpty()) {
            feedback->setText(QStringLiteral("无法生成安全码，请重试。"));
            return;
        }
        code->setText(QString::fromLatin1(value));
        feedback->setText(QStringLiteral("长码已生成，保存后可复制。"));
    });
    QObject::connect(copy, &QPushButton::clicked, page, [=, &service] {
        service.copyCode();
        feedback->setText(QStringLiteral("已复制安全码，不会同步或进入 Hotaru 剪贴板历史。"));
    });
    QObject::connect(save, &QPushButton::clicked, page, [=, &service] {
        QString error;
        if (service.configure(name->text(), code->text(), error)) {
            if (!files && !service.setClipboardSearchAlias(clipboardAlias->text(), error)) {
                feedback->setText(error);
                return;
            }
            code->clear();
            name->setText(service.name());
            clipboardAlias->setText(service.clipboardSearchAlias());
            feedback->setText(QStringLiteral("连接设置已保存。"));
        } else {
            feedback->setText(error);
        }
    });
    QObject::connect(probe, &QPushButton::clicked, page, [=, &service] {
        QString error;
        if (!service.probeAddress(probeAddress->text(), error))
            feedback->setText(error);
        else
            feedback->setText(QStringLiteral("已发送探测，请等待设备回应。"));
    });
    refresh();
    layout->addStretch();
    scroll->setWidget(page);
    return scroll;
}
}

class Plugin final : public QObject, public HotaruNativePlugin, public HotaruNativePluginV2 {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HOTARU_NATIVE_PLUGIN_IID FILE "plugin.json")
    Q_INTERFACES(HotaruNativePlugin HotaruNativePluginV2)
public:
    bool start(const QString& dataDirectory) override
    {
        if (dataDirectory.isEmpty())
            return false;
        if (!m_service) {
            m_service = std::make_unique<Service>(dataDirectory);
            connect(m_service.get(), &Service::incomingOffer, this, [this] {
                if (m_notify)
                    m_notify(m_service->transfers().pendingSummary());
            });
        }
        return true;
    }
    void stop() override
    {
        m_notify = {};
        m_service.reset();
    }
    QString state(const QString& id) const override
    {
        const auto feature = featureFor(id);
        if (!m_service || !feature)
            return {};
        if (!m_service->enabled(feature))
            return QStringLiteral("开发预览 · 已关闭");
        if (feature == Files && m_service->transfers().awaitingDecision())
            return QStringLiteral("有文件等待接收");
        int connected = 0;
        for (const auto& device : m_service->devices())
            if (device.features & feature)
                ++connected;
        return connected ? QStringLiteral("已连接 %1 台设备").arg(connected)
                         : QStringLiteral("已启用 · 等待连接");
    }
    QWidget* createPage(const QString& id, QWidget* parent) override
    {
        const auto feature = featureFor(id);
        return m_service && feature ? pageFor(*m_service, feature, parent) : nullptr;
    }
    void setFileOfferNotifier(std::function<void(QString)> callback) override
    {
        m_notify = std::move(callback);
    }

    bool enabled(const QString& id) const override
    {
        const int feature = featureFor(id);
        return m_service && feature && m_service->enabled(feature);
    }

    bool setEnabled(const QString& id, bool enabledValue, QString& error) override
    {
        const int feature = featureFor(id);
        return m_service && feature && m_service->setEnabled(feature, enabledValue, error);
    }

    QVector<HotaruNativePeer> peers(int feature) const override
    {
        QVector<HotaruNativePeer> result;
        if (!m_service)
            return result;
        for (const auto& device : m_service->devices()) {
            if (!(device.features & feature))
                continue;
            HotaruNativePeer peer;
            peer.id = device.id;
            peer.name = device.name;
            peer.address = device.address;
            peer.features = device.features;
            peer.state = QStringLiteral("已认证");
            peer.lastSeen = device.lastSeen;
            result.push_back(std::move(peer));
        }
        return result;
    }

    HotaruClipboardSnapshot clipboardSnapshot() const override
    {
        return m_service ? m_service->clipboardSnapshot() : HotaruClipboardSnapshot{};
    }

    bool sendFiles(const QByteArray& peerId, const QStringList& paths, QString& error) override
    {
        return m_service && m_service->sendFiles(peerId, paths, error);
    }

    QString clipboardSearchAlias() const override
    {
        return m_service ? m_service->clipboardSearchAlias() : QStringLiteral("jtb");
    }

    bool setClipboardSearchAlias(const QString& alias, QString& error) override
    {
        return m_service && m_service->setClipboardSearchAlias(alias, error);
    }

    void setChangedNotifier(std::function<void()> callback) override
    {
        m_changed = std::move(callback);
        if (m_changedConnection)
            disconnect(m_changedConnection);
        if (m_service)
            m_changedConnection = connect(m_service.get(), &Service::changed, this, [this] {
                if (m_changed)
                    m_changed();
            });
    }
private:
    std::unique_ptr<Service> m_service;
    std::function<void(QString)> m_notify;
    std::function<void()> m_changed;
    QMetaObject::Connection m_changedConnection;
};
}

#include "LanPlugin.moc"
