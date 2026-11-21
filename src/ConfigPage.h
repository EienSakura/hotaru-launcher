#pragma once

#include <QWidget>
#include <QVariantMap>

class QListWidget;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QSpinBox;
class QLineEdit;
class QRadioButton;
class HotkeyCaptureEdit;
class QPushButton;
class SettingsControlStyle;

// 配置页：启动与快捷键、搜索与索引、兼容插件设置、数据和外观。
class ConfigPage : public QWidget {
    Q_OBJECT
public:
    explicit ConfigPage(QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

signals:
    // 保存后通知：快捷键变更 / 索引设置变化需要重扫
    void hotkeyChanged(const QString& sequence);
    void rescanRequested();
    void hotkeyCaptureActive(bool active);

private slots:
    void onOpenDataDir();
    void onClearIndexCache();
    void addAppRoot();
    void addAppExe();
    void removeAppRoot();
    void addPinnedApp();
    void removePinnedApp();
    void togglePinnedApp();
    void movePinnedUp();
    void movePinnedDown();
    void editPinnedApp();
    void importThemePackage();
    void removeThemePackage();
    void activateThemePackage(int index);
    void syncThemeControls();
    void importHotaruConfig();
    void exportHotaruConfig();
    void updateHotkeyStatus();
    void openHotkeyCapture();

private:
    void updateControlTheme();
    void load();
    void refreshAppearance();
    void refreshPluginOptions();
    void refreshShellState();
    void discardChanges();
    void updateDraftStatus();
    QVariantMap indexValues() const;
    void applyGrid();
    void applyIndex();
    void applyPaths();
    void saveText(QLineEdit* field, const char* key);
    bool saveIni(const QVariantMap& values, bool notify = true);
    void showResult(const QString& message, bool error = false);
    bool applyHotkey(const QString& sequence);
    bool pathChanged(const QLineEdit* field, const QString& saved) const;
    void reloadPaths();
    void refreshPathInfo();

    QCheckBox* m_autostart = nullptr;
    HotkeyCaptureEdit* m_hotkey = nullptr;
    QPushButton* m_hotkeyButton = nullptr;
    QLabel* m_hotkeyStatus = nullptr;
    QLineEdit* m_installPath = nullptr;
    QFormLayout* m_downloadsLayout = nullptr;
    QWidget* m_installRow = nullptr;
    QLabel* m_installInfo = nullptr;
    QLineEdit* m_proxy = nullptr;
    QRadioButton* m_themeDark = nullptr;
    QRadioButton* m_themeLight = nullptr;
    SettingsControlStyle* m_controlStyle = nullptr;
    QLineEdit* m_storeMirrors = nullptr;   // 拉插件时先试的这些前缀，留空=内置那两条   // 插件商店那份 index.json 的地址，留空=内置那条
    QComboBox* m_engine = nullptr;
    QCheckBox* m_deep = nullptr;
    QSpinBox* m_maxEntries = nullptr;
    QSpinBox* m_refreshHours = nullptr;
    // 应用搜索的额外目录（config.ini 的 apps/roots）+ 往里找几层（apps/depth）
    QListWidget* m_appRoots = nullptr;
    QSpinBox* m_appDepth = nullptr;
    QLabel* m_appInfo = nullptr;
    // 截图 / 录像的存放目录（config.ini 的 paths/captures、paths/recordings）
    QFormLayout* m_dataLayout = nullptr;
    QLineEdit* m_capDir = nullptr;
    QLineEdit* m_recDir = nullptr;
    QLabel* m_dirInfo = nullptr;
    QCheckBox* m_favorites = nullptr;
    QGroupBox* m_shellGroup = nullptr;
    QCheckBox* m_shellMenu = nullptr;
    QCheckBox* m_archiveProgress = nullptr;
    QCheckBox* m_autoUpdate = nullptr;
    QLabel* m_shellInfo = nullptr;
    QComboBox* m_themePackages = nullptr;
    QLabel* m_themePackageInfo = nullptr;
    QListWidget* m_pinnedApps = nullptr;
    QSpinBox* m_gridColumns = nullptr;
    QSpinBox* m_gridRows = nullptr;
    QComboBox* m_pageStart = nullptr;
    QLabel* m_pinnedAppsInfo = nullptr;
    QLabel* m_pinnedCount = nullptr;
    QLabel* m_saveStatus = nullptr;
    QPushButton* m_discard = nullptr;
    QPushButton* m_applyGrid = nullptr;
    QPushButton* m_applyIndex = nullptr;
    QPushButton* m_applyPaths = nullptr;
    QVariantMap m_savedIndex;
    QString m_savedCapturePath;
    QString m_savedRecordingPath;
    int m_savedRows = 0;
    int m_savedColumns = 0;
};
