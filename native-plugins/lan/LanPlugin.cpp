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
#include <QMessageBox>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStackedWidget>
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

QVBoxLayout* cardLayout(QFrame* card, const QString& title)
{
    card->setObjectName(QStringLiteral("lanDetailCard"));
    card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);
    layout->addWidget(titleLabel(title, card));
    return layout;
}

void addCardPair(QVBoxLayout* page, QFrame* left, QFrame* right)
{
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    row->addWidget(left, 1);
    row->addWidget(right, 1);
    page->addLayout(row);
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

QWidget* pageFor(Service& service, int feature, QWidget* parent, const QByteArray& target = {})
{
    const bool core = feature == 0;
    const bool files = feature == Files;
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* stack = core ? new QStackedWidget(parent) : nullptr;
    if (stack)
        stack->addWidget(scroll);
    auto* page = new QWidget(scroll);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(22, 12, 22, 20);
    layout->setSpacing(12);
    layout->addWidget(titleLabel(core ? QStringLiteral("局域网连接") : files ?
        QStringLiteral("局域网文件分享") : QStringLiteral("局域网剪贴板共享"), page));
    auto* enable = new QCheckBox(core ? QStringLiteral("启用局域网连接") : QStringLiteral("启用此插件"), page);
    auto* status = textLabel({}, page);
    auto* feedback = textLabel({}, page);
    layout->addWidget(enable);
    layout->addWidget(status);
    const auto showError = [feedback](QString error) { feedback->setText(error); };
    const auto refreshEnable = [=, &service] {
        const QSignalBlocker block(enable);
        enable->setChecked(core ? service.coreEnabled() : service.requested(feature));
        enable->setEnabled(core || service.coreEnabled());
        if (core)
            status->setText(service.coreEnabled() ? QStringLiteral("在两台电脑上打开此页，只需配对一次。")
                                                  : QStringLiteral("连接已关闭"));
        else if (!service.coreEnabled())
            status->setText(QStringLiteral("请先启用局域网连接。"));
        else if (!service.enabled(feature))
            status->setText(QStringLiteral("插件已关闭。"));
        else {
            int online = 0;
            for (const auto& device : service.devices())
                if (device.features & feature)
                    ++online;
            status->setText(online ? QStringLiteral("%1 台设备可用").arg(online)
                                   : QStringLiteral("等待已配对设备上线并启用此功能。"));
        }
    };
    QObject::connect(enable, &QCheckBox::toggled, page, [=, &service](bool on) {
        QString error;
        if (core)
            service.setCoreEnabled(on, error);
        else
            service.setEnabled(feature, on, error);
        showError(error);
        refreshEnable();
    });
    QObject::connect(&service, &Service::changed, page, refreshEnable);
    refreshEnable();

    if (core) {
        auto* form = new QFormLayout;
        auto* name = new QLineEdit(service.name(), page);
        name->setMaxLength(48);
        auto* save = new QPushButton(QStringLiteral("保存设备名"), page);
        form->addRow(QStringLiteral("此电脑的名称"), name);
        form->addRow(save);
        layout->addLayout(form);
        QObject::connect(save, &QPushButton::clicked, page, [=, &service] {
            QString error;
            if (service.configure(name->text(), error))
                name->setText(service.name());
            showError(error);
        });

        layout->addWidget(titleLabel(QStringLiteral("配对新设备"), page));
        layout->addWidget(textLabel(QStringLiteral("生成邀请码后，同一专用网络的 Hotaru 可看到此电脑的名称和地址。"), page));
        auto* create = new QPushButton(QStringLiteral("生成邀请码"), page);
        auto* cancel = new QPushButton(QStringLiteral("取消邀请"), page);
        auto* codeText = textLabel({}, page);
        auto* inviteRow = new QHBoxLayout;
        inviteRow->addWidget(create);
        inviteRow->addWidget(cancel);
        inviteRow->addWidget(codeText, 1);
        layout->addLayout(inviteRow);
        auto* discover = new QPushButton(QStringLiteral("发现邀请 ›"), page);
        layout->addWidget(discover, 0, Qt::AlignLeft);
        auto* invitationScroll = new QScrollArea(stack);
        invitationScroll->setWidgetResizable(true);
        invitationScroll->setFrameShape(QFrame::NoFrame);
        auto* invitationPage = new QWidget(invitationScroll);
        invitationScroll->setWidget(invitationPage);
        auto* invitationLayout = new QVBoxLayout(invitationPage);
        invitationLayout->setContentsMargins(22, 12, 22, 20);
        invitationLayout->setSpacing(12);
        stack->addWidget(invitationScroll);
        auto* back = new QPushButton(QStringLiteral("返回局域网连接"), invitationPage);
        invitationLayout->addWidget(back, 0, Qt::AlignLeft);
        invitationLayout->addWidget(titleLabel(QStringLiteral("发现邀请"), invitationPage));
        invitationLayout->addWidget(textLabel(QStringLiteral("请选择正在邀请的电脑，或手动填写 IPv4。设备名称仅供辨认，请与对方核对。"), invitationPage));
        auto* candidates = new QComboBox(invitationPage);
        candidates->setAccessibleName(QStringLiteral("正在邀请的设备"));
        auto* address = new QLineEdit(invitationPage);
        address->setPlaceholderText(QStringLiteral("未发现时，输入对方电脑的 IPv4"));
        auto* code = new QLineEdit(invitationPage);
        code->setMaxLength(5);
        code->setPlaceholderText(QStringLiteral("五位邀请码"));
        code->setAccessibleName(QStringLiteral("五位邀请码"));
        auto* join = new QPushButton(QStringLiteral("发起配对"), invitationPage);
        auto* joinForm = new QFormLayout;
        joinForm->addRow(QStringLiteral("发现的邀请"), candidates);
        joinForm->addRow(QStringLiteral("手动填写地址"), address);
        joinForm->addRow(QStringLiteral("邀请码"), code);
        joinForm->addRow(join);
        invitationLayout->addLayout(joinForm);
        auto* joinStatus = textLabel({}, invitationPage);
        auto* joinError = textLabel({}, invitationPage);
        invitationLayout->addWidget(joinStatus);
        invitationLayout->addWidget(joinError);
        invitationLayout->addStretch();
        QObject::connect(discover, &QPushButton::clicked, invitationPage, [=] {
            stack->setCurrentWidget(invitationScroll);
            code->setFocus();
        });
        QObject::connect(back, &QPushButton::clicked, page, [=] {
            stack->setCurrentWidget(scroll);
            discover->setFocus();
        });
        auto* request = textLabel({}, page);
        layout->addWidget(request);
        auto* confirm = new QPushButton(QStringLiteral("确认配对"), page);
        auto* reject = new QPushButton(QStringLiteral("拒绝"), page);
        auto* requestRow = new QHBoxLayout;
        requestRow->addWidget(confirm);
        requestRow->addWidget(reject);
        requestRow->addStretch();
        layout->addLayout(requestRow);
        auto* pairing = textLabel({}, page);
        layout->addWidget(pairing);

        layout->addWidget(titleLabel(QStringLiteral("已配对设备"), page));
        auto* devices = new QListWidget(page);
        devices->setAccessibleName(QStringLiteral("已配对设备"));
        devices->setMaximumHeight(136);
        layout->addWidget(devices);
        auto* forget = new QPushButton(QStringLiteral("撤销所选设备"), page);
        layout->addWidget(forget, 0, Qt::AlignLeft);

        auto* diagnostics = new QPushButton(QStringLiteral("连接诊断 ▸"), page);
        diagnostics->setCheckable(true);
        layout->addWidget(diagnostics, 0, Qt::AlignLeft);
        auto* diagnosticPanel = new QWidget(page);
        auto* details = new QVBoxLayout(diagnosticPanel);
        details->setContentsMargins(0, 0, 0, 0);
        auto* network = textLabel({}, diagnosticPanel);
        details->addWidget(network);
        auto* probeAddress = new QLineEdit(diagnosticPanel);
        probeAddress->setPlaceholderText(QStringLiteral("手动探测 IPv4"));
        auto* probe = new QPushButton(QStringLiteral("探测"), diagnosticPanel);
        auto* probeRow = new QHBoxLayout;
        probeRow->addWidget(probeAddress);
        probeRow->addWidget(probe);
        details->addLayout(probeRow);
        auto* logs = new QPushButton(QStringLiteral("打开诊断日志目录"), diagnosticPanel);
        details->addWidget(logs, 0, Qt::AlignLeft);
        QObject::connect(diagnostics, &QPushButton::toggled, diagnostics, [diagnostics](bool open) {
            diagnostics->setText(open ? QStringLiteral("连接诊断 ▾") : QStringLiteral("连接诊断 ▸"));
        });
        diagnosticPanel->hide();
        layout->addWidget(diagnosticPanel);
        QObject::connect(diagnostics, &QPushButton::toggled, diagnosticPanel, &QWidget::setVisible);
        QObject::connect(logs, &QPushButton::clicked, page, [=, &service] {
            const auto path = service.diagnosticDirectory();
            if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                showError(QStringLiteral("无法打开诊断日志目录"));
        });
        QObject::connect(probe, &QPushButton::clicked, page, [=, &service] {
            QString error;
            showError(service.probeAddress(probeAddress->text(), error)
                ? QStringLiteral("已发送探测，请等待回应") : error);
        });

        const auto refresh = [=, &service] {
            const auto invitation = service.invitationCode();
            const auto pending = service.pairingRequest();
            const auto old = candidates->currentData();
            QList<QPair<QString, QString>> options;
            for (const auto& item : service.discoveredDevices()) {
                if (!(item.features & 1) || std::any_of(options.cbegin(), options.cend(),
                        [&](const auto& option) { return option.second == item.address; }))
                    continue;
                const auto label = item.name.isEmpty() ? QStringLiteral("未知电脑") : item.name;
                options.append({QStringLiteral("%1 · %2").arg(label, item.address), item.address});
            }
            discover->setEnabled(service.coreEnabled());
            discover->setText(options.isEmpty() ? QStringLiteral("发现邀请 ›")
                                                : QStringLiteral("发现邀请（%1） ›").arg(options.size()));
            QList<QPair<QString, QString>> previous;
            for (int i = 0; i < candidates->count(); ++i)
                if (const auto ip = candidates->itemData(i).toString(); !ip.isEmpty())
                    previous.append({candidates->itemText(i), ip});
            if (previous != options || candidates->count() == 0) {
                const QSignalBlocker block(candidates);
                candidates->clear();
                if (options.isEmpty())
                    candidates->addItem(QStringLiteral("暂无邀请"));
                else if (options.size() > 1)
                    candidates->addItem(QStringLiteral("选择正在邀请的电脑"));
                for (const auto& [label, ip] : options)
                    candidates->addItem(label, ip);
                const int index = candidates->findData(old);
                if (index >= 0 && options.size() != 1)
                    candidates->setCurrentIndex(index);
            }
            network->setText(service.status());
            create->setEnabled(service.coreEnabled() && !service.invitationInProgress());
            cancel->setVisible(service.invitationInProgress());
            codeText->setText(invitation.isEmpty() ? QString{} : QStringLiteral("%1 · 两分钟内有效\n%2 · %3")
                .arg(QString::fromLatin1(invitation), service.name(), service.invitationAddresses().join(QStringLiteral("、"))));
            address->setEnabled(service.coreEnabled());
            code->setEnabled(service.coreEnabled());
            join->setEnabled(service.coreEnabled());
            request->setText(pending.isEmpty() ? QString{} : QStringLiteral("「%1」请求配对，请核对设备名称。").arg(pending));
            confirm->setVisible(!pending.isEmpty());
            reject->setVisible(!pending.isEmpty());
            pairing->setText(service.pairingStatus());
            joinStatus->setText(service.pairingStatus());
            const auto current = devices->currentItem() ? devices->currentItem()->data(Qt::UserRole) : QVariant{};
            const auto peers = service.pairedDevices();
            bool same = devices->count() == peers.size();
            for (int i = 0; same && i < peers.size(); ++i) {
                const auto& device = peers.at(i);
                const auto title = QStringLiteral("%1 · %2").arg(device.name,
                    device.lastSeen < 0 ? QStringLiteral("离线") : QStringLiteral("在线"));
                same = devices->item(i)->data(Qt::UserRole).toByteArray() == device.id &&
                       devices->item(i)->text() == title;
            }
            if (!same) {
                devices->clear();
                for (const auto& device : peers) {
                    auto* row = new QListWidgetItem(QStringLiteral("%1 · %2").arg(device.name,
                        device.lastSeen < 0 ? QStringLiteral("离线") : QStringLiteral("在线")), devices);
                    row->setData(Qt::UserRole, device.id);
                    if (current.toByteArray() == device.id)
                        devices->setCurrentItem(row);
                }
            }
            forget->setEnabled(devices->currentItem());
        };
        QObject::connect(&service, &Service::changed, page, refresh);
        QObject::connect(devices, &QListWidget::currentItemChanged, page, [forget](QListWidgetItem* item) {
            forget->setEnabled(item != nullptr);
        });
        QObject::connect(create, &QPushButton::clicked, page, [=, &service] {
            QString error;
            service.beginInvitation(error);
            showError(error);
        });
        QObject::connect(cancel, &QPushButton::clicked, page, [&service] { service.cancelInvitation(); });
        QObject::connect(join, &QPushButton::clicked, page, [=, &service] {
            QString error;
            const auto ip = address->text().trimmed().isEmpty() ? candidates->currentData().toString()
                                                                : address->text().trimmed();
            if (ip.isEmpty())
                error = QStringLiteral("请选择对方设备，或填写对方 IPv4");
            else if (service.joinInvitation(ip, code->text(), error))
                code->clear();
            joinError->setText(error);
        });
        QObject::connect(code, &QLineEdit::returnPressed, join, &QPushButton::click);
        const auto decide = [=, &service](bool yes) {
            QString error;
            service.decidePairing(yes, error);
            showError(error);
        };
        QObject::connect(confirm, &QPushButton::clicked, page, [decide] { decide(true); });
        QObject::connect(reject, &QPushButton::clicked, page, [decide] { decide(false); });
        QObject::connect(forget, &QPushButton::clicked, page, [=, &service] {
            if (!devices->currentItem())
                return;
            const auto id = devices->currentItem()->data(Qt::UserRole).toByteArray();
            if (QMessageBox::question(page, QStringLiteral("撤销配对"), QStringLiteral("以后需要重新配对才能连接这台设备。确认撤销？"))
                    != QMessageBox::Yes)
                return;
            QString error;
            service.forgetDevice(id, error);
            showError(error);
        });
        refresh();
    } else if (!files) {
        auto* sync = new QFrame(page);
        auto* syncLayout = cardLayout(sync, QStringLiteral("同步与快捷词"));
        auto* history = new QFrame(page);
        history->setProperty("hotaruClipboardHistorySlot", true);
        cardLayout(history, QStringLiteral("本机记录"));
        addCardPair(layout, sync, history);
        syncLayout->addWidget(textLabel(QStringLiteral("只同步启用后新复制的文本和图片。"), sync));
        auto* devices = textLabel({}, sync);
        syncLayout->addWidget(devices);
        const auto refresh = [=, &service] {
            QStringList names;
            for (const auto& device : service.devices())
                if (service.enabled(Clipboard) && (device.features & Clipboard))
                    names.push_back(device.name);
            devices->setText(names.isEmpty() ? QStringLiteral("暂无在线同步设备")
                                             : QStringLiteral("同步设备：%1").arg(names.join(QStringLiteral("、"))));
        };
        QObject::connect(&service, &Service::changed, page, refresh);
        refresh();
        auto* alias = new QLineEdit(service.clipboardSearchAlias(), sync);
        alias->setMaxLength(24);
        alias->setPlaceholderText(QStringLiteral("留空关闭，例如 clip"));
        auto* aliasLabel = textLabel(QStringLiteral("剪贴板快捷词"), sync);
        aliasLabel->setBuddy(alias);
        syncLayout->addWidget(aliasLabel);
        syncLayout->addWidget(alias);
        auto* save = new QPushButton(QStringLiteral("保存快捷词"), sync);
        syncLayout->addWidget(save, 0, Qt::AlignLeft);
        syncLayout->addStretch();
        QObject::connect(save, &QPushButton::clicked, page, [=, &service] {
            QString error;
            if (service.setClipboardSearchAlias(alias->text(), error))
                alias->setText(service.clipboardSearchAlias());
            showError(error);
        });
    } else {
        auto* outgoing = new DropPanel(page);
        auto* incoming = new QFrame(page);
        outgoing->setProperty("lanTransferSide", QStringLiteral("send"));
        incoming->setProperty("lanTransferSide", QStringLiteral("receive"));
        addCardPair(layout, outgoing, incoming);
        auto* in = cardLayout(incoming, QStringLiteral("接收文件"));
        in->addWidget(textLabel(QStringLiteral("收到请求后，确认才会保存文件。"), incoming));
        auto* inStatus = textLabel({}, incoming);
        in->addWidget(inStatus);
        auto* pending = textLabel({}, incoming);
        in->addWidget(pending);
        auto* receive = new QPushButton(QStringLiteral("接收"), incoming);
        receive->setObjectName(QStringLiteral("primary"));
        auto* choose = new QPushButton(QStringLiteral("另存到"), incoming);
        auto* decline = new QPushButton(QStringLiteral("拒绝"), incoming);
        auto* cancelIn = new QPushButton(QStringLiteral("取消接收"), incoming);
        auto* inRow = new QHBoxLayout;
        for (auto* button : {receive, choose, decline, cancelIn})
            inRow->addWidget(button);
        inRow->addStretch();
        in->addLayout(inRow);
        in->addStretch();
        auto* directoryLabel = textLabel(QStringLiteral("默认接收目录"), incoming);
        directoryLabel->setObjectName(QStringLiteral("muted"));
        in->addWidget(directoryLabel);
        auto* directory = textLabel({}, incoming);
        directory->setTextInteractionFlags(Qt::TextSelectableByMouse);
        directory->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        in->addWidget(directory);
        auto* changeDirectory = new QPushButton(QStringLiteral("修改目录"), incoming);
        in->addWidget(changeDirectory, 0, Qt::AlignLeft);
        const auto accept = [=, &service](const QString& path) {
            QString error;
            service.receiveFiles(path, error);
            showError(error);
        };
        QObject::connect(receive, &QPushButton::clicked, page, [=, &service] { accept(service.downloadDirectory()); });
        QObject::connect(choose, &QPushButton::clicked, page, [=, &service] {
            const auto path = QFileDialog::getExistingDirectory(page, QStringLiteral("选择接收目录"), service.downloadDirectory());
            if (!path.isEmpty())
                accept(path);
        });
        QObject::connect(changeDirectory, &QPushButton::clicked, page, [=, &service] {
            const auto path = QFileDialog::getExistingDirectory(page, QStringLiteral("选择默认接收目录"), service.downloadDirectory());
            if (path.isEmpty())
                return;
            QString error;
            service.setDownloadDirectory(path, error);
            showError(error);
        });
        QObject::connect(decline, &QPushButton::clicked, page, [&service] { service.transfers().decline(); });
        QObject::connect(cancelIn, &QPushButton::clicked, page, [&service] { service.transfers().cancelIncoming(); });

        auto* out = cardLayout(outgoing, QStringLiteral("发送文件"));
        auto* devices = new QComboBox(outgoing);
        devices->setAccessibleName(QStringLiteral("发送到"));
        devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        devices->setMinimumContentsLength(12);
        auto* deviceLabel = textLabel(QStringLiteral("发送到"), outgoing);
        deviceLabel->setBuddy(devices);
        out->addWidget(deviceLabel);
        out->addWidget(devices);
        auto* dropHint = textLabel(QStringLiteral("将文件或文件夹拖到此栏，也可以用下方按钮选择。"), outgoing);
        dropHint->setObjectName(QStringLiteral("lanDropHint"));
        dropHint->setMinimumHeight(48);
        dropHint->setAlignment(Qt::AlignCenter);
        out->addWidget(dropHint);
        auto* send = new QPushButton(QStringLiteral("选文件"), outgoing);
        send->setObjectName(QStringLiteral("primary"));
        auto* folder = new QPushButton(QStringLiteral("选文件夹"), outgoing);
        auto* retry = new QPushButton(QStringLiteral("重试"), outgoing);
        auto* cancelOut = new QPushButton(QStringLiteral("取消发送"), outgoing);
        auto* outRow = new QHBoxLayout;
        for (auto* button : {send, folder})
            outRow->addWidget(button);
        outRow->addStretch();
        out->addLayout(outRow);
        out->addStretch();
        auto* outStatus = textLabel({}, outgoing);
        out->addWidget(outStatus);
        auto* outActions = new QHBoxLayout;
        outActions->addWidget(retry);
        outActions->addWidget(cancelOut);
        outActions->addStretch();
        out->addLayout(outActions);
        QWidget::setTabOrder(enable, devices);
        QWidget::setTabOrder(devices, send);
        QWidget::setTabOrder(send, folder);
        QWidget::setTabOrder(folder, retry);
        QWidget::setTabOrder(retry, cancelOut);
        QWidget::setTabOrder(cancelOut, receive);
        QWidget::setTabOrder(receive, choose);
        QWidget::setTabOrder(choose, decline);
        QWidget::setTabOrder(decline, cancelIn);
        QWidget::setTabOrder(cancelIn, changeDirectory);

        const auto share = [=, &service](const QStringList& paths) {
            if (paths.isEmpty())
                return;
            QString error;
            service.sendFiles(devices->currentData().toByteArray(), paths, error);
            showError(error);
        };
        outgoing->dropped = share;
        QObject::connect(send, &QPushButton::clicked, page, [=] {
            share(QFileDialog::getOpenFileNames(page, QStringLiteral("选择要发送的文件")));
        });
        QObject::connect(folder, &QPushButton::clicked, page, [=] {
            const auto path = QFileDialog::getExistingDirectory(page, QStringLiteral("选择要发送的文件夹"));
            if (!path.isEmpty())
                share({path});
        });
        QObject::connect(retry, &QPushButton::clicked, page, [=, &service] { share(service.transfers().retryPaths()); });
        QObject::connect(cancelOut, &QPushButton::clicked, page, [&service] { service.transfers().cancelOutgoing(); });

        const auto refresh = [=, &service] {
            auto selected = devices->currentIndex() >= 0 ? devices->currentData().toByteArray() : target;
            const auto local = service.devices();
            QList<Service::Device> peers;
            for (const auto& device : local)
                if (device.features & Files)
                    peers.push_back(device);
            bool same = devices->count() == peers.size();
            for (int i = 0; same && i < peers.size(); ++i)
                same = devices->itemData(i).toByteArray() == peers[i].id && devices->itemText(i) == peers[i].name;
            if (!same) {
                const QSignalBlocker block(devices);
                devices->clear();
                for (const auto& peer : peers)
                    devices->addItem(peer.name, peer.id);
                const int index = devices->findData(selected);
                if (index >= 0 || !selected.isEmpty())
                    devices->setCurrentIndex(index);
            }
            auto& transfers = service.transfers();
            const bool waiting = transfers.awaitingDecision();
            const auto inText = transfers.incomingStatus();
            const auto outText = transfers.outgoingStatus();
            inStatus->setText(inText.isEmpty() ? QStringLiteral("暂无待接收文件") : inText);
            outStatus->setText(outText.isEmpty() ? QStringLiteral("暂无发送任务") : outText);
            devices->setPlaceholderText(peers.isEmpty() ? QStringLiteral("暂无可发送设备")
                                                        : QStringLiteral("请选择设备"));
            devices->setEnabled(service.enabled(Files) && !peers.isEmpty());
            pending->setText(transfers.pendingSummary());
            pending->setVisible(waiting);
            receive->setVisible(waiting);
            choose->setVisible(waiting);
            decline->setVisible(waiting);
            cancelIn->setVisible(transfers.incoming() && !waiting);
            cancelOut->setVisible(transfers.outgoing());
            const auto path = QDir::toNativeSeparators(service.downloadDirectory());
            directory->setText(path);
            directory->setToolTip(path);
            const bool canSend = service.enabled(Files) && !transfers.outgoing() && devices->currentIndex() >= 0;
            outgoing->setAcceptDrops(canSend);
            send->setEnabled(canSend);
            folder->setEnabled(canSend);
            retry->setVisible(!transfers.retryPaths().isEmpty());
            retry->setEnabled(canSend);
        };
        QObject::connect(&service, &Service::changed, page, refresh);
        QObject::connect(devices, &QComboBox::currentIndexChanged, page, refresh);
        refresh();
        if (!target.isEmpty() && send->isEnabled())
            send->setFocus();
        else if (service.transfers().awaitingDecision())
            receive->setFocus();
    }
    layout->addWidget(feedback);
    layout->addStretch();
    scroll->setWidget(page);
    return stack ? static_cast<QWidget*>(stack) : scroll;
}

}

class Plugin final : public QObject, public HotaruNativePlugin, public HotaruNativePluginV2, public HotaruLanPackage,
                     public HotaruLanClipboardReceive {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID HOTARU_LAN_PACKAGE_IID FILE "published-plugin.json")
    Q_INTERFACES(HotaruNativePlugin HotaruNativePluginV2 HotaruLanPackage HotaruLanClipboardReceive)
public:
    QString id() const override { return QLatin1String(kHotaruLanCoreId); }
    int feature() const override { return 0; }
    void setAvailableFeatures(int features) override { if (m_service) m_service->setAvailableFeatures(features); }
    bool hasPendingFileOffer() const override
    {
        return m_service && m_service->enabled(Files) && m_service->transfers().awaitingDecision();
    }
    QWidget* createFeaturePage(const QString& id, const QByteArray& target, QWidget* parent) override
    {
        const auto feature = featureFor(id);
        return m_service && (feature || id == QLatin1String(kHotaruLanCoreId))
            ? pageFor(*m_service, feature, parent, target) : nullptr;
    }
    bool start(const QString& dataDirectory) override
    {
        if (dataDirectory.isEmpty())
            return false;
        if (!m_service) {
            m_service = std::make_unique<Service>(dataDirectory);
            connect(m_service.get(), &Service::clipboardReceived, this, [this](const QMimeData& mime) {
                if (m_clipboardReceived)
                    m_clipboardReceived(mime);
            });
            connect(m_service.get(), &Service::incomingOffer, this, [this] {
                if (m_notify)
                    m_notify(m_service->transfers().pendingSummary());
            });
        }
        return true;
    }
    void setClipboardReceiveNotifier(std::function<void(const QMimeData&)> callback) override
    {
        m_clipboardReceived = std::move(callback);
    }

    void stop() override
    {
        m_notify = {};
        m_clipboardReceived = {};
        m_service.reset();
    }
    QString state(const QString& id) const override
    {
        const auto feature = featureFor(id);
        if (!m_service)
            return {};
        if (id == QLatin1String(kHotaruLanCoreId))
            return m_service->coreEnabled() ? QStringLiteral("连接已启用") : QStringLiteral("已关闭");
        if (!feature)
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
        return createFeaturePage(id, {}, parent);
    }
    void setFileOfferNotifier(std::function<void(QString)> callback) override
    {
        m_notify = std::move(callback);
    }

    bool enabled(const QString& id) const override
    {
        const int feature = featureFor(id);
        return m_service && (id == QLatin1String(kHotaruLanCoreId) ? m_service->coreEnabled()
                                                                   : m_service->enabled(feature));
    }

    bool setEnabled(const QString& id, bool enabledValue, QString& error) override
    {
        const int feature = featureFor(id);
        return m_service && (id == QLatin1String(kHotaruLanCoreId) ? m_service->setCoreEnabled(enabledValue, error)
                                                                   : m_service->setEnabled(feature, enabledValue, error));
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
        return m_service ? m_service->clipboardSearchAlias() : QString{};
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
    std::function<void(const QMimeData&)> m_clipboardReceived;
    std::function<void()> m_changed;
    QMetaObject::Connection m_changedConnection;
};
}

#include "LanPlugin.moc"
