#include "catalog.hpp"

#include "s2fs/steam2_archive.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QResource>
#include <QSet>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>

#include <tuple>
#include <utility>
#include <vector>

static void initializeCatalogResources() {
    Q_INIT_RESOURCE(resources);
}

namespace s2gui {
namespace {

enum class PayloadKind {
    Blob,
    Dat,
};

struct ParsedFilename {
    std::uint32_t depot{};
    std::uint32_t version{};
    std::uint32_t crc{};
    PayloadKind kind{};
};

struct BlobIdentity {
    std::uint32_t depot{};
    std::uint32_t version{};
    std::uint32_t crc{};

    auto operator<=>(const BlobIdentity&) const = default;
};

struct BlobRecord {
    ParsedFilename filename;
    QString path;
    std::optional<s2fs::BlobMetadata> metadata;
    QString error;
};

struct DatRecord {
    ParsedFilename filename;
    QString path;
    std::optional<std::uint64_t> size;
};

bool pathLess(const QString& left, const QString& right) {
    const int folded = QString::compare(left, right, Qt::CaseInsensitive);
    return folded != 0 ? folded < 0 : left < right;
}

bool isAsciiDecimal(const QStringView text) {
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar character : text) {
        if (character < u'0' || character > u'9') {
            return false;
        }
    }
    return true;
}

bool isAsciiHex(const QStringView text, qsizetype width) {
    if (text.size() != width) {
        return false;
    }
    for (const QChar character : text) {
        const ushort value = character.unicode();
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedFilename> parseStrictFilename(const QString& path) {
    const QString filename = QFileInfo(path).fileName();
    PayloadKind kind;
    qsizetype extensionLength{};
    if (filename.endsWith(QStringLiteral(".blob"), Qt::CaseSensitive)) {
        kind = PayloadKind::Blob;
        extensionLength = 5;
    } else if (filename.endsWith(QStringLiteral(".dat"), Qt::CaseSensitive)) {
        kind = PayloadKind::Dat;
        extensionLength = 4;
    } else {
        return std::nullopt;
    }

    const QStringView stem(filename.constData(), filename.size() - extensionLength);
    const auto fields = stem.split(u'_', Qt::KeepEmptyParts);
    if (fields.size() != 4 || !isAsciiDecimal(fields[0]) || !isAsciiDecimal(fields[1]) ||
        !isAsciiHex(fields[2], 8) || !isAsciiHex(fields[3], 64)) {
        return std::nullopt;
    }

    bool depotOk = false;
    bool versionOk = false;
    bool crcOk = false;
    const std::uint32_t depot = fields[0].toUInt(&depotOk, 10);
    const std::uint32_t version = fields[1].toUInt(&versionOk, 10);
    const std::uint32_t crc = fields[2].toUInt(&crcOk, 16);
    if (!depotOk || !versionOk || !crcOk) {
        return std::nullopt;
    }
    return ParsedFilename{depot, version, crc, kind};
}

std::filesystem::path nativePath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    const QByteArray encoded = path.toUtf8();
    const auto* begin = reinterpret_cast<const char8_t*>(encoded.constData());
    return std::filesystem::path(
        std::u8string(begin, begin + encoded.size()));
#endif
}

QString exceptionText(const std::exception& error) {
    return QString::fromLocal8Bit(error.what());
}

QString normalizedAbsolutePath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString pathKey(const QString& path) {
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

QString withDetail(Readiness readiness, const QString& detail) {
    if (detail.isEmpty()) {
        return readinessText(readiness);
    }
    return readinessText(readiness) + QStringLiteral(": ") + detail;
}

} // namespace

Catalog scanLooseFiles(const QStringList& roots, const ProgressCallback& progress) {
    Catalog catalog;

    QStringList sortedRoots;
    QSet<QString> seenRoots;
    for (const QString& root : roots) {
        const QString normalized = normalizedAbsolutePath(root);
        const QString key = pathKey(normalized);
        if (!seenRoots.contains(key)) {
            seenRoots.insert(key);
            sortedRoots.push_back(normalized);
        }
    }
    std::sort(sortedRoots.begin(), sortedRoots.end(), pathLess);

    QStringList files;
    QSet<QString> seenFiles;
    for (const QString& root : sortedRoots) {
        const QFileInfo rootInfo(root);
        if (!rootInfo.exists()) {
            catalog.errors.push_back(QStringLiteral("Scan root does not exist: %1").arg(root));
            continue;
        }
        if (!rootInfo.isDir() || rootInfo.isSymLink()) {
            catalog.errors.push_back(QStringLiteral("Scan root is not a directory: %1").arg(root));
            continue;
        }

        QDirIterator iterator(root,
            QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (!info.isFile() || info.isSymLink()) {
                continue;
            }
            const QString path = QDir::cleanPath(info.absoluteFilePath());
            const QString key = pathKey(path);
            if (!seenFiles.contains(key)) {
                seenFiles.insert(key);
                files.push_back(path);
            }
        }
    }
    std::sort(files.begin(), files.end(), pathLess);

    std::vector<BlobRecord> blobs;
    std::vector<DatRecord> dats;
    blobs.reserve(static_cast<std::size_t>(files.size()));
    dats.reserve(static_cast<std::size_t>(files.size()));

    for (const QString& path : files) {
        ++catalog.scannedFiles;
        if (progress) {
            progress(catalog.scannedFiles, path);
        }

        const QFileInfo currentInfo(path);
        if (!currentInfo.isFile() || currentInfo.isSymLink()) {
            catalog.errors.push_back(QStringLiteral("Skipped payload that is no longer a regular file: %1").arg(path));
            continue;
        }

        const auto parsed = parseStrictFilename(path);
        if (!parsed) {
            continue;
        }

        if (parsed->kind == PayloadKind::Dat) {
            ++catalog.datCount;
            DatRecord record{*parsed, path, std::nullopt};
            const qint64 size = currentInfo.size();
            if (size < 0) {
                catalog.errors.push_back(QStringLiteral("Cannot determine DAT size: %1").arg(path));
            } else {
                record.size = static_cast<std::uint64_t>(size);
            }
            dats.push_back(std::move(record));
            continue;
        }

        ++catalog.blobCount;
        BlobRecord record{*parsed, path, std::nullopt, {}};
        try {
            const auto metadata = s2fs::inspect_blob(nativePath(path));
            if (metadata.depot_id != parsed->depot || metadata.version != parsed->version ||
                !metadata.filename_crc || *metadata.filename_crc != parsed->crc) {
                record.error = QStringLiteral("parsed metadata identity does not match the strict filename");
            } else {
                record.metadata = metadata;
            }
        } catch (const std::exception& error) {
            record.error = exceptionText(error);
        } catch (...) {
            record.error = QStringLiteral("unknown blob inspection error");
        }
        if (!record.error.isEmpty()) {
            catalog.errors.push_back(QStringLiteral("%1: %2").arg(path, record.error));
        }
        blobs.push_back(std::move(record));
    }

    using DatIdentity = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
    std::map<DatIdentity, std::vector<const DatRecord*>> datGroups;
    for (const auto& dat : dats) {
        if (dat.size) {
            datGroups[{dat.filename.depot, dat.filename.version, *dat.size}].push_back(&dat);
        }
    }
    for (auto& [identity, group] : datGroups) {
        (void)identity;
        std::sort(group.begin(), group.end(), [](const DatRecord* left, const DatRecord* right) {
            return pathLess(left->path, right->path);
        });
    }

    std::map<BlobIdentity, std::vector<const BlobRecord*>> blobGroups;
    for (const auto& blob : blobs) {
        blobGroups[{blob.filename.depot, blob.filename.version, blob.filename.crc}].push_back(&blob);
    }
    for (auto& [identity, group] : blobGroups) {
        (void)identity;
        std::sort(group.begin(), group.end(), [](const BlobRecord* left, const BlobRecord* right) {
            return pathLess(left->path, right->path);
        });
    }

    std::map<std::uint32_t, QString> missingKeyErrors;
    const auto keyError = [&](std::uint32_t depot) -> const QString& {
        const auto existing = missingKeyErrors.find(depot);
        if (existing != missingKeyErrors.end()) {
            return existing->second;
        }
        QString error;
        try {
            (void)s2fs::depot_key(depot);
        } catch (const std::exception& exception) {
            error = exceptionText(exception);
        } catch (...) {
            error = QStringLiteral("unknown depot key lookup error");
        }
        return missingKeyErrors.emplace(depot, std::move(error)).first->second;
    };

    std::map<std::uint32_t, CatalogDepot> depots;
    for (const auto& [identity, group] : blobGroups) {
        CatalogVersion item;
        item.depot = identity.depot;
        item.version = identity.version;
        item.crc = identity.crc;
        item.topBlobPath = group.front()->path;

        auto& depot = depots[identity.depot];
        depot.id = identity.depot;
        depot.name = knownDepotName(identity.depot);

        if (group.size() != 1) {
            item.readiness = Readiness::DuplicateBlob;
            item.status = withDetail(item.readiness,
                QStringLiteral("%1 files have depot %2, version %3, CRC %4")
                    .arg(group.size())
                    .arg(identity.depot)
                    .arg(identity.version)
                    .arg(identity.crc, 8, 16, QLatin1Char('0')));
            depot.versions.push_back(std::move(item));
            continue;
        }

        const BlobRecord* current = group.front();
        if (!current->metadata) {
            item.readiness = Readiness::CorruptBlob;
            item.status = withDetail(item.readiness, current->error);
            depot.versions.push_back(std::move(item));
            continue;
        }

        item.parentCrc = current->metadata->parent_crc;
        item.manifestFileCount = current->metadata->manifest_file_count;

        std::vector<const BlobRecord*> newestFirst;
        QString ancestryFailure;
        Readiness ancestryReadiness = Readiness::Ready;
        for (;;) {
            newestFirst.push_back(current);
            const auto& metadata = *current->metadata;
            if (metadata.parent_crc == 0) {
                break;
            }
            if (metadata.version == 0) {
                ancestryReadiness = Readiness::MissingParent;
                ancestryFailure = QStringLiteral("version 0 names parent CRC %1")
                    .arg(metadata.parent_crc, 8, 16, QLatin1Char('0'));
                break;
            }

            const BlobIdentity parentIdentity{
                metadata.depot_id, metadata.version - 1, metadata.parent_crc};
            const auto parent = blobGroups.find(parentIdentity);
            if (parent == blobGroups.end()) {
                ancestryReadiness = Readiness::MissingParent;
                ancestryFailure = QStringLiteral("version %1 requires version %2 CRC %3")
                    .arg(metadata.version)
                    .arg(metadata.version - 1)
                    .arg(metadata.parent_crc, 8, 16, QLatin1Char('0'));
                break;
            }
            if (parent->second.size() != 1) {
                ancestryReadiness = Readiness::DuplicateBlob;
                ancestryFailure = QStringLiteral("parent version %1 CRC %2 has %3 files")
                    .arg(metadata.version - 1)
                    .arg(metadata.parent_crc, 8, 16, QLatin1Char('0'))
                    .arg(parent->second.size());
                break;
            }
            current = parent->second.front();
            if (!current->metadata) {
                ancestryReadiness = Readiness::CorruptBlob;
                ancestryFailure = QStringLiteral("parent %1 could not be inspected: %2")
                    .arg(current->path, current->error);
                break;
            }
        }

        if (ancestryReadiness != Readiness::Ready) {
            item.readiness = ancestryReadiness;
            item.status = withDetail(item.readiness, ancestryFailure);
            depot.versions.push_back(std::move(item));
            continue;
        }

        std::reverse(newestFirst.begin(), newestFirst.end());
        QStringList missingVersions;
        QStringList ambiguousVersions;
        bool byteTotalOverflow = false;
        for (const BlobRecord* chainBlob : newestFirst) {
            item.blobFiles.push_back(chainBlob->path);
            const auto expectedSize = chainBlob->metadata->expected_dat_size;
            if (expectedSize > std::numeric_limits<std::uint64_t>::max() - item.compressedBytes) {
                byteTotalOverflow = true;
            } else {
                item.compressedBytes += expectedSize;
            }
            std::vector<const DatRecord*> matches;
            const auto datGroup = datGroups.find({
                chainBlob->metadata->depot_id,
                chainBlob->metadata->version,
                expectedSize,
            });
            if (datGroup != datGroups.end()) {
                matches = datGroup->second;
            }
            if (matches.empty()) {
                missingVersions.push_back(QString::number(chainBlob->metadata->version));
                continue;
            }
            if (matches.size() != 1) {
                ambiguousVersions.push_back(QStringLiteral("%1 (%2 matches)")
                    .arg(chainBlob->metadata->version)
                    .arg(matches.size()));
                continue;
            }
            item.datFiles.push_back(matches.front()->path);
        }

        const QString& depotKeyError = keyError(identity.depot);
        if (byteTotalOverflow) {
            item.readiness = Readiness::CorruptBlob;
            item.status = withDetail(item.readiness, QStringLiteral("compressed byte total overflows 64 bits"));
        } else if (!ambiguousVersions.isEmpty()) {
            item.readiness = Readiness::AmbiguousDat;
            QString detail = QStringLiteral("versions %1").arg(ambiguousVersions.join(QStringLiteral(", ")));
            if (!missingVersions.isEmpty()) {
                detail += QStringLiteral("; missing versions %1").arg(missingVersions.join(QStringLiteral(", ")));
            }
            item.status = withDetail(item.readiness, detail);
        } else if (!missingVersions.isEmpty()) {
            item.readiness = Readiness::MissingDat;
            item.status = withDetail(item.readiness,
                QStringLiteral("versions %1 have no DAT of the embedded size")
                    .arg(missingVersions.join(QStringLiteral(", "))));
        } else if (!depotKeyError.isEmpty()) {
            item.readiness = Readiness::MissingKey;
            item.status = withDetail(item.readiness, depotKeyError);
        } else {
            item.readiness = Readiness::Ready;
            item.status = readinessText(item.readiness);
        }
        depot.versions.push_back(std::move(item));
    }

    catalog.depots.reserve(static_cast<qsizetype>(depots.size()));
    for (auto& [id, depot] : depots) {
        (void)id;
        std::sort(depot.versions.begin(), depot.versions.end(), [](const CatalogVersion& left,
                                                                 const CatalogVersion& right) {
            return std::tie(left.version, left.crc) < std::tie(right.version, right.crc);
        });
        catalog.depots.push_back(std::move(depot));
    }
    return catalog;
}

QString readinessText(Readiness readiness) {
    switch (readiness) {
    case Readiness::Ready:
        return QStringLiteral("Ready");
    case Readiness::MissingDat:
        return QStringLiteral("Missing DAT");
    case Readiness::MissingParent:
        return QStringLiteral("Missing parent");
    case Readiness::AmbiguousDat:
        return QStringLiteral("Ambiguous DAT");
    case Readiness::DuplicateBlob:
        return QStringLiteral("Duplicate blob");
    case Readiness::MissingKey:
        return QStringLiteral("Missing depot key");
    case Readiness::CorruptBlob:
        return QStringLiteral("Corrupt blob");
    }
    return QStringLiteral("Unknown");
}

QString knownDepotName(std::uint32_t depot) {
    static const QHash<std::uint32_t, QString> labels = [] {
        initializeCatalogResources();
        QHash<std::uint32_t, QString> result;
        QFile file(QStringLiteral(":/data/depot_labels.tsv"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return result;
        }
        while (!file.atEnd()) {
            QByteArray line = file.readLine();
            while (line.endsWith('\n') || line.endsWith('\r')) {
                line.chop(1);
            }
            const qsizetype separator = line.indexOf('\t');
            if (separator <= 0 || separator + 1 >= line.size()) {
                continue;
            }
            bool idOk = false;
            const std::uint32_t id = line.first(separator).toUInt(&idOk, 10);
            if (idOk) {
                result.insert(id, QString::fromUtf8(line.sliced(separator + 1)));
            }
        }
        return result;
    }();

    const auto found = labels.constFind(depot);
    return found == labels.cend() ? QStringLiteral("Depot %1").arg(depot) : *found;
}

QString formatBytes(std::uint64_t bytes) {
    static constexpr const char* units[]{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }

    long double value = static_cast<long double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0L && unit + 1 < std::size(units)) {
        value /= 1024.0L;
        ++unit;
    }
    return QStringLiteral("%1 %2")
        .arg(QLocale::c().toString(static_cast<double>(value), 'f', 2), QString::fromLatin1(units[unit]));
}

} // namespace s2gui
