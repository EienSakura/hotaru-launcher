#include "platform/FileMeta.h"

#include <QDir>
#include <QImage>

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <QImageReader>
#include <QFileInfo>

namespace Platform {

ShortcutInfo shortcutInfo(const QString& path)
{
    ShortcutInfo info;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IShellLinkW* link=nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))) {
        IPersistFile* file=nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (SUCCEEDED(file->Load(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),STGM_READ))) {
                wchar_t buffer[32768]{};
                link->GetPath(buffer,32768,nullptr,SLGP_RAWPATH);info.target=QString::fromWCharArray(buffer);
                buffer[0]=0;link->GetArguments(buffer,32768);info.arguments=QString::fromWCharArray(buffer);
                buffer[0]=0;link->GetWorkingDirectory(buffer,32768);info.workingDirectory=QString::fromWCharArray(buffer);
                buffer[0]=0;link->GetIconLocation(buffer,32768,&info.iconIndex);info.iconPath=QString::fromWCharArray(buffer);
                wchar_t expanded[32768]{};
                if (ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(info.iconPath.utf16()),expanded,32768)>0)
                    info.iconPath=QString::fromWCharArray(expanded);
                if (!info.iconPath.isEmpty() && QDir::isRelativePath(info.iconPath))
                    info.iconPath=QFileInfo(path).dir().absoluteFilePath(info.iconPath);
            }
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return info;
}

QImage iconImage(const QString& path, int pixels, int index)
{
    const QString abs=QDir::toNativeSeparators(path);
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    QImage result;
    const QString suffix=QFileInfo(path).suffix().toLower();
    if (QStringList{"png","jpg","jpeg","svg","bmp","webp"}.contains(suffix)) {
        QImageReader reader(path);result=reader.read();
    }
    // 明确的资源索引和 ICO 优先提取，不经过带快捷方式箭头的系统小图标。
    if (QStringList{"ico","dll","icl"}.contains(suffix) || index!=0) {
        HICON icon=nullptr;
        PrivateExtractIconsW(reinterpret_cast<LPCWSTR>(abs.utf16()),index,pixels,pixels,&icon,nullptr,1,0);
        if (icon) { result=QImage::fromHICON(icon);DestroyIcon(icon); }
    }
    if (result.isNull()) {
        IShellItemImageFactory* factory=nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(abs.utf16()),nullptr,IID_PPV_ARGS(&factory)))) {
            HBITMAP bitmap=nullptr;
            if (SUCCEEDED(factory->GetImage({pixels,pixels},SIIGBF_ICONONLY|SIIGBF_BIGGERSIZEOK,&bitmap))) {
                // 直接读取 32 位 DIB，避免 RGB 转换把 Shell 图标的 alpha 全部改成 255。
                BITMAP description{};
                if (GetObjectW(bitmap,sizeof(description),&description)) {
                    const int height=qAbs(description.bmHeight);
                    QImage pixels(description.bmWidth,height,QImage::Format_ARGB32_Premultiplied);
                    pixels.fill(Qt::transparent);
                    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth=description.bmWidth;info.bmiHeader.biHeight=-height;
                    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                    HDC dc=GetDC(nullptr);
                    const bool read=GetDIBits(dc,bitmap,0,height,pixels.bits(),&info,DIB_RGB_COLORS)==height;
                    ReleaseDC(nullptr,dc);
                    if (read) {
                        bool hasAlpha=false;
                        for (int y=0;y<height && !hasAlpha;++y) {
                            const auto* line=reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
                            for (int x=0;x<pixels.width();++x) if (qAlpha(line[x])!=0) {hasAlpha=true;break;}
                        }
                        if (!hasAlpha) pixels.reinterpretAsFormat(QImage::Format_RGB32);
                        result=pixels;
                    }
                }
                DeleteObject(bitmap);
            }
            factory->Release();
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (result.isNull()) { QImageReader reader(path);result=reader.read(); }
    return result;
}

bool extractIconToPng(const QString& iconFile, int iconIndex, const QString& pngOut)
{
    const QImage image=iconImage(iconFile,256,iconIndex);
    return !image.isNull() && image.save(pngOut);
}

QString fileVersion(const QString& exePath)
{
    const std::wstring w = QDir::toNativeSeparators(exePath).toStdWString();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (size == 0)
        return {};
    QByteArray data(int(size), 0);
    if (!GetFileVersionInfoW(w.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", (LPVOID*)&ffi, &len) || !ffi)
        return {};
    if (ffi->dwFileVersionMS == 0 && ffi->dwFileVersionLS == 0)
        return {};
    return QString("%1.%2.%3.%4")
        .arg(HIWORD(ffi->dwFileVersionMS))
        .arg(LOWORD(ffi->dwFileVersionMS))
        .arg(HIWORD(ffi->dwFileVersionLS))
        .arg(LOWORD(ffi->dwFileVersionLS));
}

} // namespace Platform
