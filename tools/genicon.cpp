// 从同一张原图生成 Qt、Windows 和 README 所用的图标。
#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>

#include <array>
#include <cstdio>

namespace {

constexpr std::array kIconSizes{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
constexpr qreal kCornerRadiusRatio = 0.22;

QByteArray pngForSize(const QImage& source, int size)
{
    const auto scaled = source.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // 每个尺寸单独裁出抗锯齿圆角，并保留源图的透明度。
    QImage pixels(size, size, QImage::Format_ARGB32_Premultiplied);
    pixels.fill(Qt::transparent);
    QPainter painter(&pixels);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    const qreal radius = size * kCornerRadiusRatio;
    painter.drawRoundedRect(QRectF(pixels.rect()), radius, radius);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.drawImage(0, 0, scaled);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "png");
    writer.setCompression(100);
    return writer.write(pixels) ? bytes : QByteArray{};
}

bool writeAsset(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        std::fprintf(stderr, "图标写入失败：%s\n", qPrintable(path));
        return false;
    }
    std::printf("%s（%lld 字节）\n", qPrintable(path), static_cast<long long>(bytes.size()));
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) {
        std::fprintf(stderr, "用法：genicon <源 PNG> <项目目录>\n");
        return 2;
    }

    const QImage source(args[1], "PNG");
    if (source.isNull() || source.width() != source.height()) {
        std::fprintf(stderr, "应用图标原图必须是有效的方形 PNG。\n");
        return 1;
    }
    const QDir root(args[2]);
    if (!root.mkpath(QStringLiteral("resources/branding")) || !root.mkpath(QStringLiteral("docs/images"))) {
        std::fprintf(stderr, "无法创建图标输出目录。\n");
        return 1;
    }

    // Windows ICONDIR 固定为小端，256 像素的目录宽高以零表示。
    QByteArray ico;
    QDataStream directory(&ico, QIODevice::WriteOnly);
    directory.setByteOrder(QDataStream::LittleEndian);
    directory << quint16(0) << quint16(1) << quint16(kIconSizes.size());
    quint32 offset = 6 + 16 * kIconSizes.size();
    QByteArray images;
    for (const int size : kIconSizes) {
        const auto png = pngForSize(source, size);
        if (png.isEmpty()) {
            std::fprintf(stderr, "无法编码 %d 像素的 PNG。\n", size);
            return 1;
        }
        const auto dimension = quint8(size == 256 ? 0 : size);
        directory << dimension << dimension << quint8(0) << quint8(0)
                  << quint16(1) << quint16(32) << quint32(png.size()) << offset;
        offset += quint32(png.size());
        images += png;

        if (size == 16 || size == 32 || size == 64 || size == 128) {
            const auto path = root.filePath(QStringLiteral("resources/branding/hotaru-icon-%1.png").arg(size));
            if (!writeAsset(path, png))
                return 1;
        }
        if (size == 256 && !writeAsset(root.filePath(QStringLiteral("docs/images/hotaru-icon-256.png")), png))
            return 1;
    }
    return writeAsset(root.filePath(QStringLiteral("resources/ltool.ico")), ico + images) ? 0 : 1;
}
