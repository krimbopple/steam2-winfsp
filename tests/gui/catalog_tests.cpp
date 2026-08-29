#include "catalog.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = QByteArray;
constexpr std::uint32_t kDepot = 242;

void appendU16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>((value >> 8) & 0xff));
}

void appendU32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

void setU32(Bytes& bytes, qsizetype offset, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes[offset++] = static_cast<char>((value >> shift) & 0xff);
    }
}

Bytes scalar32(std::uint32_t value) {
    Bytes result;
    appendU32(result, value);
    return result;
}

Bytes makeBlob(const std::vector<std::pair<std::uint32_t, Bytes>>& fields) {
    Bytes bytes;
    appendU16(bytes, 0x5001);
    appendU32(bytes, 0);
    appendU32(bytes, 0);
    for (const auto& [key, value] : fields) {
        appendU16(bytes, 4);
        appendU32(bytes, static_cast<std::uint32_t>(value.size()));
        appendU32(bytes, key);
        bytes.append(value);
    }
    setU32(bytes, 2, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

Bytes zlibCompress(const Bytes& input) {
    uLongf size = compressBound(static_cast<uLong>(input.size()));
    Bytes output(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (compress2(reinterpret_cast<Bytef*>(output.data()), &size,
            reinterpret_cast<const Bytef*>(input.constData()), static_cast<uLong>(input.size()),
            Z_BEST_SPEED) != Z_OK) {
        throw std::runtime_error("fixture compression failed");
    }
    output.resize(static_cast<qsizetype>(size));
    return output;
}

Bytes compressedBlob(const Bytes& unpacked) {
    const Bytes packed = zlibCompress(unpacked);
    Bytes result;
    appendU16(result, 0x4301);
    for (int shift = 0; shift != 64; shift += 8) {
        result.push_back(static_cast<char>((static_cast<std::uint64_t>(packed.size()) >> shift) & 0xff));
    }
    for (int shift = 0; shift != 64; shift += 8) {
        result.push_back(static_cast<char>((static_cast<std::uint64_t>(unpacked.size()) >> shift) & 0xff));
    }
    appendU16(result, 6);
    result.append(packed);
    return result;
}

Bytes makeManifest(std::uint32_t depot, std::uint32_t version) {
    Bytes manifest;
    appendU32(manifest, 3); // format
    appendU32(manifest, depot);
    appendU32(manifest, version);
    appendU32(manifest, 1); // one root node
    appendU32(manifest, 0); // no files
    appendU32(manifest, 0x8000);
    appendU32(manifest, 0); // binary size, patched below
    appendU32(manifest, 1); // one NUL in the string table
    appendU32(manifest, 0);
    appendU32(manifest, 0);
    appendU32(manifest, 0);
    appendU32(manifest, 0);
    appendU32(manifest, 0); // fingerprint
    appendU32(manifest, 0); // checksum, patched below

    appendU32(manifest, 0); // root name offset
    appendU32(manifest, 0);
    appendU32(manifest, 0);
    appendU32(manifest, 0);
    appendU32(manifest, 0xffffffffU);
    appendU32(manifest, 0xffffffffU);
    appendU32(manifest, 0xffffffffU);
    manifest.push_back('\0');

    setU32(manifest, 6 * 4, static_cast<std::uint32_t>(manifest.size()));
    const auto checksum = adler32(0, reinterpret_cast<const Bytef*>(manifest.constData()),
        static_cast<uInt>(manifest.size()));
    setU32(manifest, 13 * 4, checksum);
    return manifest;
}

Bytes makeDepotBlob(std::uint32_t depot, std::uint32_t version,
                    std::uint32_t parentCrc, std::uint32_t expectedDatSize) {
    const Bytes nested = makeBlob({{0, makeManifest(depot, version)}});
    return makeBlob({
        {0, scalar32(3)},
        {3, compressedBlob(nested)},
        {12, scalar32(parentCrc)},
        {13, scalar32(expectedDatSize)},
    });
}

QString crcText(std::uint32_t crc) {
    return QStringLiteral("%1").arg(crc, 8, 16, QLatin1Char('0'));
}

QString strictName(std::uint32_t depot, std::uint32_t version, std::uint32_t crc,
                   QChar hashCharacter, QStringView extension) {
    return QStringLiteral("%1_%2_%3_%4.%5")
        .arg(depot)
        .arg(version)
        .arg(crcText(crc), QString(64, hashCharacter), extension.toString());
}

QString writeFile(const QString& root, const QString& relative, const Bytes& bytes) {
    const QString path = QDir(root).filePath(relative);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        throw std::runtime_error("fixture directory creation failed");
    }
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) {
        throw std::runtime_error("fixture write failed");
    }
    output.close();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const s2gui::CatalogVersion& findVersion(
    const s2gui::Catalog& catalog, std::uint32_t depot, std::uint32_t version, std::uint32_t crc) {
    for (const auto& group : catalog.depots) {
        if (group.id != depot) {
            continue;
        }
        for (const auto& item : group.versions) {
            if (item.version == version && item.crc == crc) {
                return item;
            }
        }
    }
    throw std::runtime_error("catalog version was not found");
}

void testStrictRecursiveFilteringAndErrors() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory was not created");

    const QString validCorrupt = writeFile(directory.path(),
        QStringLiteral("deep/inside/") + strictName(kDepot, 7, 0x1234abcd, u'a', u"blob"),
        QByteArray("not a blob"));
    (void)writeFile(directory.path(), QStringLiteral("242_7_1234abcd_short.blob"), QByteArray("x"));
    (void)writeFile(directory.path(),
        QStringLiteral("deep/") + strictName(kDepot, 7, 0x1234abcd, u'b', u"blob") + QStringLiteral(".bak"),
        QByteArray("x"));
    (void)writeFile(directory.path(),
        QStringLiteral("deep/242_7_1234abcd_") + QString(64, u'c') + QStringLiteral(".BLOB"),
        QByteArray("x"));
    (void)writeFile(directory.path(),
        QStringLiteral("deep/") + strictName(kDepot, 7, 0x1234abcd, u'd', u"dat"),
        QByteArray("123"));
    const QString payloadDirectory = QDir(directory.path()).filePath(
        strictName(kDepot, 8, 0x87654321, u'e', u"blob"));
    require(QDir().mkpath(payloadDirectory), "payload-shaped directory was not created");

    QStringList progressPaths;
    const auto catalog = s2gui::scanLooseFiles({directory.path()},
        [&](qsizetype scanned, const QString& path) {
            require(scanned == progressPaths.size() + 1, "progress count was not monotonic");
            progressPaths.push_back(path);
        });

    require(catalog.scannedFiles == 5, "regular-file scan count was wrong");
    require(progressPaths.size() == catalog.scannedFiles, "progress did not cover every scanned file");
    require(catalog.blobCount == 1, "non-strict blob filename passed the filter");
    require(catalog.datCount == 1, "strict DAT filename was not counted");
    require(catalog.depots.size() == 1 && catalog.depots[0].versions.size() == 1,
        "corrupt strict blob was not preserved as one catalog item");
    const auto& item = catalog.depots[0].versions[0];
    require(item.topBlobPath == validCorrupt, "catalog did not preserve the exact blob path");
    require(item.readiness == s2gui::Readiness::CorruptBlob,
        "filename-only corrupt item was not marked corrupt");
    require(!catalog.errors.isEmpty() && catalog.errors.join(u'\n').contains(validCorrupt),
        "blob inspection error did not preserve its source path");
}

void testReadyAncestryAndDeterministicGrouping() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory was not created");

    const auto blob1Name = strictName(kDepot, 1, 0x11111111, u'a', u"blob");
    const auto blob2Name = strictName(kDepot, 2, 0x22222222, u'b', u"blob");
    const QString blob1 = writeFile(directory.path(), QStringLiteral("z/blobs/") + blob1Name,
        makeDepotBlob(kDepot, 1, 0, 3));
    const QString blob2 = writeFile(directory.path(), QStringLiteral("a/blobs/") + blob2Name,
        makeDepotBlob(kDepot, 2, 0x11111111, 5));
    const QString dat1 = writeFile(directory.path(),
        QStringLiteral("nested/dats/") + strictName(kDepot, 1, 0xaaaaaaaa, u'c', u"dat"),
        QByteArray("123"));
    const QString dat2 = writeFile(directory.path(),
        QStringLiteral("other/dats/") + strictName(kDepot, 2, 0xbbbbbbbb, u'd', u"dat"),
        QByteArray("12345"));

    const QString nestedRoot = QDir(directory.path()).filePath(QStringLiteral("nested"));
    const auto first = s2gui::scanLooseFiles({nestedRoot, directory.path()});
    const auto second = s2gui::scanLooseFiles({directory.path(), nestedRoot});

    require(first.scannedFiles == 4 && first.blobCount == 2 && first.datCount == 2,
        "overlapping roots were not deduplicated");
    require(first.depots.size() == 1 && first.depots[0].id == kDepot,
        "depot grouping was wrong");
    require(first.depots[0].versions.size() == 2 &&
            first.depots[0].versions[0].version == 1 && first.depots[0].versions[1].version == 2,
        "versions were not sorted deterministically");
    require(second.depots.size() == first.depots.size() &&
            second.depots[0].versions.size() == first.depots[0].versions.size(),
        "root order changed catalog grouping");

    const auto& top = findVersion(first, kDepot, 2, 0x22222222);
    require(top.readiness == s2gui::Readiness::Ready && top.status == QStringLiteral("Ready"),
        "complete ancestry did not become ready");
    require(top.parentCrc == 0x11111111 && top.manifestFileCount == 0,
        "top blob metadata was not exposed");
    require(top.compressedBytes == 8, "compressed chain byte total was wrong");
    require(top.topBlobPath == blob2, "top blob path was wrong");
    require(top.blobFiles == QStringList{blob1, blob2}, "blob ancestry was not oldest-to-newest");
    require(top.datFiles == QStringList{dat1, dat2}, "DAT ancestry was not oldest-to-newest");

    const auto& secondTop = findVersion(second, kDepot, 2, 0x22222222);
    require(secondTop.blobFiles == top.blobFiles && secondTop.datFiles == top.datFiles,
        "root order changed exact chain paths");
}

void testReadinessConstruction() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory was not created");

    // Missing exact-size DAT.
    (void)writeFile(directory.path(),
        QStringLiteral("missing/") + strictName(kDepot, 10, 0x10000000, u'a', u"blob"),
        makeDepotBlob(kDepot, 10, 0, 4));
    (void)writeFile(directory.path(),
        QStringLiteral("missing/") + strictName(kDepot, 10, 0xaaaaaaaa, u'b', u"dat"),
        QByteArray("bad"));

    // Two exact-size DAT candidates.
    (void)writeFile(directory.path(),
        QStringLiteral("ambiguous/") + strictName(kDepot, 11, 0x11000000, u'c', u"blob"),
        makeDepotBlob(kDepot, 11, 0, 2));
    (void)writeFile(directory.path(),
        QStringLiteral("ambiguous/") + strictName(kDepot, 11, 0x11111111, u'd', u"dat"),
        QByteArray("aa"));
    (void)writeFile(directory.path(),
        QStringLiteral("ambiguous/") + strictName(kDepot, 11, 0x22222222, u'e', u"dat"),
        QByteArray("bb"));

    // Missing parent must not be normalized into a root.
    (void)writeFile(directory.path(),
        QStringLiteral("parent/") + strictName(kDepot, 12, 0x12000000, u'f', u"blob"),
        makeDepotBlob(kDepot, 12, 0xdeadbeef, 1));

    // Duplicate identity: hashes and paths differ, depot/version/CRC identity does not.
    const Bytes duplicate = makeDepotBlob(kDepot, 13, 0, 1);
    (void)writeFile(directory.path(),
        QStringLiteral("duplicate/a/") + strictName(kDepot, 13, 0x13000000, u'a', u"blob"), duplicate);
    (void)writeFile(directory.path(),
        QStringLiteral("duplicate/b/") + strictName(kDepot, 13, 0x13000000, u'b', u"blob"), duplicate);

    // A fully paired unknown depot reaches the key check.
    constexpr std::uint32_t unknownDepot = 0xffffffffU;
    (void)writeFile(directory.path(),
        QStringLiteral("key/") + strictName(unknownDepot, 1, 0x14000000, u'c', u"blob"),
        makeDepotBlob(unknownDepot, 1, 0, 1));
    (void)writeFile(directory.path(),
        QStringLiteral("key/") + strictName(unknownDepot, 1, 0x33333333, u'd', u"dat"),
        QByteArray("x"));

    const auto catalog = s2gui::scanLooseFiles({directory.path()});
    require(findVersion(catalog, kDepot, 10, 0x10000000).readiness == s2gui::Readiness::MissingDat,
        "wrong-size DAT was accepted");
    require(findVersion(catalog, kDepot, 11, 0x11000000).readiness == s2gui::Readiness::AmbiguousDat,
        "multiple exact-size DATs were not marked ambiguous");
    const auto& missingParent = findVersion(catalog, kDepot, 12, 0x12000000);
    require(missingParent.readiness == s2gui::Readiness::MissingParent &&
            missingParent.status.contains(QStringLiteral("deadbeef")),
        "missing ancestry was silently normalized");
    require(findVersion(catalog, kDepot, 13, 0x13000000).readiness == s2gui::Readiness::DuplicateBlob,
        "duplicate blob identity was not detected");
    require(findVersion(catalog, unknownDepot, 1, 0x14000000).readiness == s2gui::Readiness::MissingKey,
        "unknown depot did not reach missing-key readiness");
    require(catalog.depots.size() == 2 && catalog.depots[0].id == kDepot &&
            catalog.depots[1].id == unknownDepot,
        "depots were not sorted numerically");
}

void testLabelsAndByteFormatting() {
    require(s2gui::knownDepotName(241) == QStringLiteral("Counter-Strike: Source Client"),
        "resource-backed CSS label was not loaded");
    require(s2gui::knownDepotName(0xffffffffU) == QStringLiteral("Depot 4294967295"),
        "unknown depot label fallback was wrong");
    require(s2gui::formatBytes(0) == QStringLiteral("0 B"), "zero byte formatting was wrong");
    require(s2gui::formatBytes(1024) == QStringLiteral("1.00 KiB"), "KiB formatting was wrong");
    require(s2gui::readinessText(s2gui::Readiness::AmbiguousDat) == QStringLiteral("Ambiguous DAT"),
        "readiness label was wrong");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    (void)application;
    if (argc > 1) {
        QStringList roots;
        for (int index = 1; index < argc; ++index) {
            roots.push_back(QString::fromLocal8Bit(argv[index]));
        }
        const auto catalog = s2gui::scanLooseFiles(roots);
        std::cout << "scanned=" << catalog.scannedFiles
                  << " blobs=" << catalog.blobCount
                  << " dats=" << catalog.datCount
                  << " depots=" << catalog.depots.size()
                  << " errors=" << catalog.errors.size() << '\n';
        for (const auto& depot : catalog.depots) {
            for (const auto& version : depot.versions) {
                if (version.readiness == s2gui::Readiness::Ready) {
                    std::cout << depot.id << '\t'
                              << version.version << '\t'
                              << (version.crc ? *version.crc : 0) << '\t'
                              << version.blobFiles.size() << '\t'
                              << version.datFiles.size() << '\t'
                              << version.compressedBytes << '\t'
                              << depot.name.toUtf8().constData() << '\n';
                }
            }
        }
        return 0;
    }

    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"strict recursive filtering and error preservation", testStrictRecursiveFilteringAndErrors},
        {"ready ancestry and deterministic grouping", testReadyAncestryAndDeterministicGrouping},
        {"readiness construction", testReadinessConstruction},
        {"labels and byte formatting", testLabelsAndByteFormatting},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
