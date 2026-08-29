#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace s2gui {

enum class Readiness {
    Ready,
    MissingDat,
    MissingParent,
    AmbiguousDat,
    DuplicateBlob,
    MissingKey,
    CorruptBlob,
};

struct CatalogVersion {
    std::uint32_t depot{};
    std::uint32_t version{};
    std::optional<std::uint32_t> crc;
    std::uint32_t parentCrc{};
    std::uint32_t manifestFileCount{};
    std::uint64_t compressedBytes{};
    Readiness readiness{Readiness::CorruptBlob};
    QString status;
    QString topBlobPath;
    QStringList blobFiles;
    QStringList datFiles;
};

struct CatalogDepot {
    std::uint32_t id{};
    QString name;
    QVector<CatalogVersion> versions;
};

struct Catalog {
    QVector<CatalogDepot> depots;
    qsizetype scannedFiles{};
    qsizetype blobCount{};
    qsizetype datCount{};
    QStringList errors;
};

using ProgressCallback = std::function<void(qsizetype scanned, const QString& path)>;

Catalog scanLooseFiles(const QStringList& roots, const ProgressCallback& progress = {});
QString readinessText(Readiness readiness);
QString knownDepotName(std::uint32_t depot);
QString formatBytes(std::uint64_t bytes);

} // namespace s2gui

Q_DECLARE_METATYPE(s2gui::Catalog)
