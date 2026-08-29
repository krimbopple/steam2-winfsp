#include "main_window.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>

#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>

#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTime>
#include <windows.h>
#include <shellapi.h>

#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <utility>

namespace s2gui {
namespace {

constexpr int depotIndexRole = Qt::UserRole;
constexpr int versionIndexRole = Qt::UserRole + 1;

QString depotTitle(std::uint32_t id) {
    const QString name = knownDepotName(id);
    return name.isEmpty()
        ? QStringLiteral("Depot %1").arg(static_cast<qulonglong>(id))
        : QStringLiteral("%1 (%2)").arg(name).arg(static_cast<qulonglong>(id));
}

QString crcText(const std::optional<std::uint32_t>& crc) {
    if (!crc) {
        return QStringLiteral("—");
    }
    return QStringLiteral("0x%1")
        .arg(static_cast<qulonglong>(*crc), 8, 16, QLatin1Char('0'))
        .toUpper();
}

QString processErrorText(QProcess::ProcessError error) {
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("failed to start");
    case QProcess::Crashed:
        return QStringLiteral("crashed");
    case QProcess::Timedout:
        return QStringLiteral("timed out");
    case QProcess::WriteError:
        return QStringLiteral("stdin write failed");
    case QProcess::ReadError:
        return QStringLiteral("output read failed");
    case QProcess::UnknownError:
        return QStringLiteral("unknown process error");
    }
    return QStringLiteral("unknown process error");
}
QString firstMountedExecutable(const QString& root) {
    struct Candidate {
        QString path;
        QString relative;
    };
    QVector<Candidate> candidates;
    QDirIterator iterator(
        root,
        QStringList{QStringLiteral("*.exe")},
        QDir::Files | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    const QDir rootDirectory(root);
    while (iterator.hasNext()) {
        const QString path = QDir::cleanPath(iterator.next());
        candidates.push_back({path, rootDirectory.relativeFilePath(path)});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        const auto depth = [](const QString& relative) {
            return relative.count(QLatin1Char('/')) + relative.count(QLatin1Char('\\'));
        };
        const int leftDepth = depth(left.relative);
        const int rightDepth = depth(right.relative);
        if (leftDepth != rightDepth) {
            return leftDepth < rightDepth;
        }
        return QString::compare(left.relative, right.relative, Qt::CaseInsensitive) < 0;
    });
    return candidates.isEmpty() ? QString{} : candidates.front().path;
}


} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    buildUi();
    loadSettings();

    unmountTimer_.setSingleShot(true);
    unmountTimer_.setInterval(5000);

    connect(&scanWatcher_, &QFutureWatcher<Catalog>::finished, this, [this] {
        scanFinished();
    });
    connect(&mountProcess_, &QProcess::readyRead, this, [this] {
        mountOutputReady();
    });
    connect(
        &mountProcess_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this](int exitCode, QProcess::ExitStatus exitStatus) {
            mountFinished(exitCode, exitStatus);
        });
    connect(&mountProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        mountError(error);
    });
    connect(&unmountTimer_, &QTimer::timeout, this, [this] {
        unmountTimedOut();
    });

    refreshActions();
}

MainWindow::~MainWindow() {
    saveSettings();
    if (scanWatcher_.isRunning()) {
        scanWatcher_.future().waitForFinished();
    }
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Steam2 Depot Manager"));
    resize(1180, 800);

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    auto* aboutAction = helpMenu->addAction(tr("About && Licenses…"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        showAbout();
    });

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    auto* topSplitter = new QSplitter(Qt::Horizontal, central);

    auto* libraryGroup = new QGroupBox(tr("Loose Steam2 libraries"), topSplitter);
    auto* libraryLayout = new QVBoxLayout(libraryGroup);
    libraryFolders_ = new QListWidget(libraryGroup);
    libraryFolders_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    libraryFolders_->setAlternatingRowColors(true);
    libraryLayout->addWidget(libraryFolders_);
    auto* libraryButtons = new QHBoxLayout;
    addFolderButton_ = new QPushButton(tr("Add…"), libraryGroup);
    removeFolderButton_ = new QPushButton(tr("Remove"), libraryGroup);
    scanButton_ = new QPushButton(tr("Scan"), libraryGroup);
    libraryButtons->addWidget(addFolderButton_);
    libraryButtons->addWidget(removeFolderButton_);
    libraryButtons->addStretch();
    libraryButtons->addWidget(scanButton_);
    libraryLayout->addLayout(libraryButtons);
    scanProgress_ = new QProgressBar(libraryGroup);
    scanProgress_->setRange(0, 1);
    scanProgress_->setValue(0);
    scanProgress_->setTextVisible(false);
    libraryLayout->addWidget(scanProgress_);
    scanStatus_ = new QLabel(tr("Add one or more folders, then scan."), libraryGroup);
    scanStatus_->setWordWrap(true);
    libraryLayout->addWidget(scanStatus_);

    auto* catalogGroup = new QGroupBox(tr("Depot revisions"), topSplitter);
    auto* catalogLayout = new QVBoxLayout(catalogGroup);
    catalogTree_ = new QTreeWidget(catalogGroup);
    catalogTree_->setColumnCount(6);
    catalogTree_->setHeaderLabels(
        {tr("Depot / version"), tr("Status"), tr("Files"), tr("Packed size"), tr("CRC"), tr("Top blob")});
    catalogTree_->setAlternatingRowColors(true);
    catalogTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    catalogTree_->setUniformRowHeights(true);
    catalogTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    catalogTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    catalogTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    catalogTree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    catalogTree_->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    catalogTree_->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    catalogLayout->addWidget(catalogTree_);
    addRevisionButton_ = new QPushButton(tr("Add ready revision →"), catalogGroup);
    catalogLayout->addWidget(addRevisionButton_);

    auto* compositionGroup = new QGroupBox(tr("Mount composition (bottom to top)"), topSplitter);
    auto* compositionLayout = new QVBoxLayout(compositionGroup);
    auto* compositionHelp = new QLabel(
        tr("Later entries overlay earlier entries when paths collide."), compositionGroup);
    compositionHelp->setWordWrap(true);
    compositionLayout->addWidget(compositionHelp);
    compositionList_ = new QListWidget(compositionGroup);
    compositionList_->setAlternatingRowColors(true);
    compositionLayout->addWidget(compositionList_);
    auto* compositionButtons = new QHBoxLayout;
    removeCompositionButton_ = new QPushButton(tr("Remove"), compositionGroup);
    moveUpButton_ = new QPushButton(tr("Up"), compositionGroup);
    moveDownButton_ = new QPushButton(tr("Down"), compositionGroup);
    compositionButtons->addWidget(removeCompositionButton_);
    compositionButtons->addStretch();
    compositionButtons->addWidget(moveUpButton_);
    compositionButtons->addWidget(moveDownButton_);
    compositionLayout->addLayout(compositionButtons);

    topSplitter->addWidget(libraryGroup);
    topSplitter->addWidget(catalogGroup);
    topSplitter->addWidget(compositionGroup);
    topSplitter->setStretchFactor(0, 1);
    topSplitter->setStretchFactor(1, 2);
    topSplitter->setStretchFactor(2, 1);
    rootLayout->addWidget(topSplitter, 3);

    auto* launchGroup = new QGroupBox(tr("Mount and launch"), central);
    auto* launchLayout = new QVBoxLayout(launchGroup);
    auto* form = new QFormLayout;
    auto* mountRow = new QWidget(launchGroup);
    auto* mountRowLayout = new QHBoxLayout(mountRow);
    mountRowLayout->setContentsMargins(0, 0, 0, 0);
    mountPoint_ = new QComboBox(mountRow);
    mountPoint_->setEditable(true);
    mountPoint_->setInsertPolicy(QComboBox::NoInsert);
    for (char drive = 'D'; drive <= 'Z'; ++drive) {
        mountPoint_->addItem(QString(QChar::fromLatin1(drive)) + QLatin1Char(':'));
    }
    auto* browseMountButton = new QPushButton(tr("Folder…"), mountRow);
    mountRowLayout->addWidget(mountPoint_, 1);
    mountRowLayout->addWidget(browseMountButton);
    form->addRow(tr("Mount point:"), mountRow);
    launchArguments_ = new QLineEdit(launchGroup);
    launchArguments_->setPlaceholderText(QStringLiteral("-game cstrike -steam"));
    form->addRow(tr("Executable arguments:"), launchArguments_);

    launchLayout->addLayout(form);

    auto* launchButtons = new QHBoxLayout;
    mountButton_ = new QPushButton(tr("Mount"), launchGroup);
    playButton_ = new QPushButton(tr("Play"), launchGroup);
    unmountButton_ = new QPushButton(tr("Unmount"), launchGroup);
    launchButtons->addStretch();
    launchButtons->addWidget(mountButton_);
    launchButtons->addWidget(playButton_);
    launchButtons->addWidget(unmountButton_);
    launchLayout->addLayout(launchButtons);
    rootLayout->addWidget(launchGroup);

    auto* logGroup = new QGroupBox(tr("Diagnostics"), central);
    auto* logLayout = new QVBoxLayout(logGroup);
    diagnosticLog_ = new QPlainTextEdit(logGroup);
    diagnosticLog_->setReadOnly(true);
    diagnosticLog_->setMaximumBlockCount(5000);
    diagnosticLog_->setPlaceholderText(tr("Scanner, mount, and launch diagnostics appear here."));
    logLayout->addWidget(diagnosticLog_);
    rootLayout->addWidget(logGroup, 2);

    setCentralWidget(central);
    statusBar()->showMessage(tr("Not mounted"));

    connect(addFolderButton_, &QPushButton::clicked, this, [this] { addLibraryFolder(); });
    connect(removeFolderButton_, &QPushButton::clicked, this, [this] { removeLibraryFolders(); });
    connect(scanButton_, &QPushButton::clicked, this, [this] { startScan(); });
    connect(libraryFolders_, &QListWidget::itemSelectionChanged, this, [this] { refreshActions(); });
    connect(catalogTree_, &QTreeWidget::itemSelectionChanged, this, [this] { refreshActions(); });
    connect(catalogTree_, &QTreeWidget::itemDoubleClicked, this, [this] { addSelectedRevision(); });
    connect(addRevisionButton_, &QPushButton::clicked, this, [this] { addSelectedRevision(); });
    connect(compositionList_, &QListWidget::currentRowChanged, this, [this] { refreshActions(); });
    connect(removeCompositionButton_, &QPushButton::clicked, this, [this] { removeCompositionEntry(); });
    connect(moveUpButton_, &QPushButton::clicked, this, [this] { moveCompositionEntry(-1); });
    connect(moveDownButton_, &QPushButton::clicked, this, [this] { moveCompositionEntry(1); });
    connect(browseMountButton, &QPushButton::clicked, this, [this] { chooseMountPoint(); });
    connect(mountPoint_->lineEdit(), &QLineEdit::textChanged, this, [this] { refreshActions(); });
    connect(mountButton_, &QPushButton::clicked, this, [this] { mountComposition(); });
    connect(playButton_, &QPushButton::clicked, this, [this] { playGame(); });
    connect(unmountButton_, &QPushButton::clicked, this, [this] { requestUnmount(); });
}

void MainWindow::loadSettings() {
    QSettings settings;
    const QStringList folders = settings.value(QStringLiteral("libraryFolders")).toStringList();
    for (const QString& folder : folders) {
        if (!folder.trimmed().isEmpty()) {
            libraryFolders_->addItem(QDir::cleanPath(folder));
        }
    }
    mountPoint_->setCurrentText(settings.value(QStringLiteral("mountPoint"), QStringLiteral("M:")).toString());
    launchArguments_->setText(
        settings.value(QStringLiteral("launchArguments"), QStringLiteral("-game cstrike -steam")).toString());
}

void MainWindow::saveSettings() const {
    QSettings settings;
    QStringList folders;
    folders.reserve(libraryFolders_->count());
    for (int row = 0; row < libraryFolders_->count(); ++row) {
        folders.push_back(libraryFolders_->item(row)->text());
    }
    settings.setValue(QStringLiteral("libraryFolders"), folders);
    settings.setValue(QStringLiteral("mountPoint"), selectedMountPoint());
    settings.setValue(QStringLiteral("launchArguments"), launchArguments_->text());
}

void MainWindow::addLibraryFolder() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Add loose Steam2 library folder"), QDir::homePath());
    if (folder.isEmpty()) {
        return;
    }

    const QString clean = QDir::cleanPath(folder);
    for (int row = 0; row < libraryFolders_->count(); ++row) {
        if (QString::compare(libraryFolders_->item(row)->text(), clean, Qt::CaseInsensitive) == 0) {
            libraryFolders_->setCurrentRow(row);
            return;
        }
    }
    libraryFolders_->addItem(clean);
    libraryFolders_->setCurrentRow(libraryFolders_->count() - 1);
    saveSettings();
    refreshActions();
}

void MainWindow::removeLibraryFolders() {
    const auto selected = libraryFolders_->selectedItems();
    for (QListWidgetItem* item : selected) {
        delete libraryFolders_->takeItem(libraryFolders_->row(item));
    }
    saveSettings();
    refreshActions();
}

void MainWindow::startScan() {
    if (scanWatcher_.isRunning()) {
        return;
    }

    QStringList roots;
    roots.reserve(libraryFolders_->count());
    for (int row = 0; row < libraryFolders_->count(); ++row) {
        roots.push_back(libraryFolders_->item(row)->text());
    }
    if (roots.isEmpty()) {
        QMessageBox::information(this, tr("No library folders"), tr("Add at least one folder before scanning."));
        return;
    }

    saveSettings();
    scanProgress_->setRange(0, 0);
    scanStatus_->setText(tr("Scanning…"));
    catalogTree_->clear();
    catalog_.reset();
    appendLog(tr("Scanning %1 folder(s).").arg(roots.size()));
    refreshActions();

    const QPointer<MainWindow> window(this);
    scanWatcher_.setFuture(QtConcurrent::run([roots = std::move(roots), window] {
        Catalog result;
        QElapsedTimer progressTimer;
        progressTimer.start();
        qsizetype lastReported = 0;
        try {
            result = scanLooseFiles(
                roots,
                [window, &progressTimer, &lastReported](qsizetype scanned, const QString& path) {
                    if (!window) {
                        return;
                    }
                    if (scanned != 1 && scanned - lastReported < 64
                        && progressTimer.elapsed() < 100) {
                        return;
                    }
                    lastReported = scanned;
                    progressTimer.restart();
                    QMetaObject::invokeMethod(
                        window.data(),
                        [window, scanned, path] {
                            if (window) {
                                window->updateScanProgress(scanned, path);
                            }
                        },
                        Qt::QueuedConnection);
                });
        } catch (const std::exception& error) {
            result.errors.push_back(
                MainWindow::tr("Scan failed: %1").arg(QString::fromLocal8Bit(error.what())));
        } catch (...) {
            result.errors.push_back(MainWindow::tr("Scan failed with an unknown error."));
        }
        return result;
    }));
    refreshActions();
}

void MainWindow::updateScanProgress(qsizetype scanned, const QString& path) {
    if (scanWatcher_.isRunning()) {
        scanStatus_->setText(
            tr("Scanned %1 files\n%2").arg(scanned).arg(QDir::toNativeSeparators(path)));
    }
}

void MainWindow::scanFinished() {
    try {
        catalog_ = std::make_shared<Catalog>(scanWatcher_.result());
    } catch (const std::exception& error) {
        auto failed = std::make_shared<Catalog>();
        failed->errors.push_back(tr("Scan worker failed: %1").arg(QString::fromLocal8Bit(error.what())));
        catalog_ = std::move(failed);
    } catch (...) {
        auto failed = std::make_shared<Catalog>();
        failed->errors.push_back(tr("Scan worker failed with an unknown error."));
        catalog_ = std::move(failed);
    }

    scanProgress_->setRange(0, 1);
    scanProgress_->setValue(1);
    showCatalog();
    scanStatus_->setText(
        tr("Scanned %1 files: %2 blobs, %3 DATs, %4 depots.")
            .arg(catalog_->scannedFiles)
            .arg(catalog_->blobCount)
            .arg(catalog_->datCount)
            .arg(catalog_->depots.size()));
    appendLog(scanStatus_->text());
    for (const QString& error : catalog_->errors) {
        appendLog(tr("Scan diagnostic: %1").arg(error));
    }
    refreshActions();
    maybeFinishClose();
}

void MainWindow::showCatalog() {
    catalogTree_->clear();
    if (!catalog_) {
        return;
    }

    for (qsizetype depotIndex = 0; depotIndex < catalog_->depots.size(); ++depotIndex) {
        const CatalogDepot& depot = catalog_->depots.at(depotIndex);
        auto* depotItem = new QTreeWidgetItem(catalogTree_);
        depotItem->setText(0, depotTitle(depot.id));
        depotItem->setData(0, depotIndexRole, depotIndex);
        depotItem->setData(0, versionIndexRole, -1);
        depotItem->setFirstColumnSpanned(true);
        depotItem->setExpanded(true);

        for (qsizetype versionIndex = 0; versionIndex < depot.versions.size(); ++versionIndex) {
            const CatalogVersion& version = depot.versions.at(versionIndex);
            auto* versionItem = new QTreeWidgetItem(depotItem);
            versionItem->setText(0, tr("Version %1").arg(static_cast<qulonglong>(version.version)));
            versionItem->setText(
                1, version.status.isEmpty() ? readinessText(version.readiness) : version.status);
            versionItem->setText(2, QString::number(version.manifestFileCount));
            versionItem->setText(3, formatBytes(version.compressedBytes));
            versionItem->setText(4, crcText(version.crc));
            versionItem->setText(5, QDir::toNativeSeparators(version.topBlobPath));
            versionItem->setData(0, depotIndexRole, depotIndex);
            versionItem->setData(0, versionIndexRole, versionIndex);
            versionItem->setToolTip(0, version.status);
            versionItem->setToolTip(5, QDir::toNativeSeparators(version.topBlobPath));
            if (version.readiness != Readiness::Ready) {
                versionItem->setForeground(1, QColor(Qt::darkRed));
            }
        }
    }
}

const CatalogVersion* MainWindow::selectedCatalogVersion() const {
    if (!catalog_) {
        return nullptr;
    }
    QTreeWidgetItem* item = catalogTree_->currentItem();
    if (!item) {
        return nullptr;
    }
    const qsizetype depotIndex = item->data(0, depotIndexRole).toLongLong();
    const qsizetype versionIndex = item->data(0, versionIndexRole).toLongLong();
    if (depotIndex < 0 || depotIndex >= catalog_->depots.size()) {
        return nullptr;
    }
    const CatalogDepot& depot = catalog_->depots.at(depotIndex);
    if (versionIndex < 0 || versionIndex >= depot.versions.size()) {
        return nullptr;
    }
    return &depot.versions.at(versionIndex);
}

void MainWindow::addSelectedRevision() {
    const CatalogVersion* selected = selectedCatalogVersion();
    if (!selected || selected->readiness != Readiness::Ready) {
        return;
    }

    const auto duplicate = std::find_if(
        composition_.cbegin(), composition_.cend(), [selected](const CatalogVersion& existing) {
            return existing.depot == selected->depot && existing.version == selected->version
                && existing.crc == selected->crc && existing.topBlobPath == selected->topBlobPath;
        });
    if (duplicate != composition_.cend()) {
        compositionList_->setCurrentRow(static_cast<int>(std::distance(composition_.cbegin(), duplicate)));
        return;
    }

    composition_.push_back(*selected);
    refreshComposition();
    compositionList_->setCurrentRow(compositionList_->count() - 1);
    refreshActions();
}

void MainWindow::removeCompositionEntry() {
    const int row = compositionList_->currentRow();
    if (row < 0 || row >= composition_.size()) {
        return;
    }
    composition_.removeAt(row);
    refreshComposition();
    if (!composition_.isEmpty()) {
        compositionList_->setCurrentRow(std::min(row, static_cast<int>(composition_.size() - 1)));
    }
    refreshActions();
}

void MainWindow::moveCompositionEntry(int offset) {
    const int row = compositionList_->currentRow();
    const int destination = row + offset;
    if (row < 0 || destination < 0 || destination >= composition_.size()) {
        return;
    }
    composition_.move(row, destination);
    refreshComposition();
    compositionList_->setCurrentRow(destination);
    refreshActions();
}

void MainWindow::refreshComposition() {
    const int oldRow = compositionList_->currentRow();
    compositionList_->clear();
    for (qsizetype index = 0; index < composition_.size(); ++index) {
        const CatalogVersion& version = composition_.at(index);
        auto* item = new QListWidgetItem(
            tr("%1. %2 — version %3\n%4 files, %5, %6")
                .arg(index + 1)
                .arg(depotTitle(version.depot))
                .arg(static_cast<qulonglong>(version.version))
                .arg(version.manifestFileCount)
                .arg(formatBytes(version.compressedBytes))
                .arg(crcText(version.crc)),
            compositionList_);
        item->setToolTip(
            tr("Top blob: %1\nBlob chain: %2 file(s)\nDAT chain: %3 file(s)")
                .arg(QDir::toNativeSeparators(version.topBlobPath))
                .arg(version.blobFiles.size())
                .arg(version.datFiles.size()));
    }
    if (oldRow >= 0 && oldRow < compositionList_->count()) {
        compositionList_->setCurrentRow(oldRow);
    }
}

void MainWindow::refreshActions() {
    const bool scanning = scanWatcher_.isRunning();
    const bool processRunning = mountProcess_.state() != QProcess::NotRunning;
    const CatalogVersion* selected = selectedCatalogVersion();
    const int compositionRow = compositionList_->currentRow();

    addFolderButton_->setEnabled(!scanning);
    removeFolderButton_->setEnabled(!scanning && !libraryFolders_->selectedItems().isEmpty());
    scanButton_->setEnabled(!scanning && libraryFolders_->count() > 0);
    addRevisionButton_->setEnabled(
        !processRunning && selected && selected->readiness == Readiness::Ready);
    compositionList_->setEnabled(!processRunning);
    removeCompositionButton_->setEnabled(!processRunning && compositionRow >= 0);
    moveUpButton_->setEnabled(!processRunning && compositionRow > 0);
    moveDownButton_->setEnabled(
        !processRunning && compositionRow >= 0 && compositionRow + 1 < composition_.size());
    mountPoint_->setEnabled(!processRunning);
    mountButton_->setEnabled(
        !processRunning && !composition_.isEmpty() && !selectedMountPoint().isEmpty());
    playButton_->setEnabled(processRunning && mounted_);
    unmountButton_->setEnabled(processRunning && !unmountRequested_);
}

void MainWindow::chooseMountPoint() {
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Choose an empty mount folder"), QDir::homePath());
    if (!folder.isEmpty()) {
        mountPoint_->setCurrentText(QDir::toNativeSeparators(QDir::cleanPath(folder)));
        saveSettings();
    }
}

QString MainWindow::selectedMountPoint() const {
    QString point = mountPoint_->currentText().trimmed();
    if (point.size() == 2 && point.at(0).isLetter() && point.at(1) == QLatin1Char(':')) {
        point[0] = point.at(0).toUpper();
        return point;
    }
    return point.isEmpty() ? QString{} : QDir::cleanPath(point);
}

void MainWindow::mountComposition() {
    if (mountProcess_.state() != QProcess::NotRunning || composition_.isEmpty()) {
        return;
    }

    const QString mount = selectedMountPoint();
    if (mount.isEmpty()) {
        QMessageBox::warning(this, tr("Mount point required"), tr("Choose a drive letter or mount folder."));
        return;
    }

    const QString executable =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("steam2fs.exe"));
    if (!QFileInfo::exists(executable)) {
        const QString message = tr("steam2fs.exe was not found beside this application:\n%1")
                                    .arg(QDir::toNativeSeparators(executable));
        appendLog(message);
        QMessageBox::critical(this, tr("Cannot mount"), message);
        return;
    }

    auto buildFile = std::make_unique<QTemporaryFile>(
        QDir::temp().filePath(QStringLiteral("steam2fs-build-XXXXXX.json")));
    buildFile->setAutoRemove(true);
    if (!buildFile->open()) {
        const QString message = tr("Could not create a temporary build definition: %1")
                                    .arg(buildFile->errorString());
        appendLog(message);
        QMessageBox::critical(this, tr("Cannot mount"), message);
        return;
    }

    QJsonArray depots;
    for (const CatalogVersion& version : std::as_const(composition_)) {
        QJsonArray blobFiles;
        for (const QString& path : version.blobFiles) {
            blobFiles.push_back(path);
        }
        QJsonArray datFiles;
        for (const QString& path : version.datFiles) {
            datFiles.push_back(path);
        }

        QJsonObject depot;
        depot.insert(QStringLiteral("id"), static_cast<qint64>(version.depot));
        depot.insert(QStringLiteral("version"), static_cast<qint64>(version.version));
        depot.insert(
            QStringLiteral("blob_crc"),
            version.crc ? QJsonValue(static_cast<qint64>(*version.crc)) : QJsonValue(QJsonValue::Null));
        depot.insert(QStringLiteral("blob_files"), blobFiles);
        depot.insert(QStringLiteral("dat_files"), datFiles);
        depot.insert(QStringLiteral("mount_prefix"), QString{});
        depot.insert(QStringLiteral("required"), true);
        depots.push_back(depot);
    }

    QJsonObject build;
    build.insert(QStringLiteral("blob_directory"), QStringLiteral("."));
    build.insert(QStringLiteral("dat_directory"), QStringLiteral("."));
    build.insert(QStringLiteral("depots"), depots);
    const QByteArray json = QJsonDocument(build).toJson(QJsonDocument::Compact);
    if (buildFile->write(json) != json.size() || !buildFile->flush()) {
        const QString message = tr("Could not write the temporary build definition: %1")
                                    .arg(buildFile->errorString());
        appendLog(message);
        QMessageBox::critical(this, tr("Cannot mount"), message);
        return;
    }
    buildFile->close();

    buildFile_ = std::move(buildFile);
    mounted_ = false;
    unmountRequested_ = false;
    activeMountPoint_.clear();
    mountOutputProbe_.clear();
    mountProcess_.setProcessChannelMode(QProcess::MergedChannels);
    mountProcess_.setWorkingDirectory(QCoreApplication::applicationDirPath());
    mountProcess_.setProgram(executable);
    mountProcess_.setArguments(
        {QStringLiteral("--build"), buildFile_->fileName(), QStringLiteral("--mount"), mount,
         QStringLiteral("--wait-stdin")});
    appendLog(
        tr("Starting steam2fs.exe for %1 with %2 overlay(s).")
            .arg(QDir::toNativeSeparators(mount))
            .arg(composition_.size()));
    statusBar()->showMessage(tr("Starting mount at %1…").arg(QDir::toNativeSeparators(mount)));
    mountProcess_.start();
    saveSettings();
    refreshActions();
}

void MainWindow::mountOutputReady() {
    QByteArray bytes = mountProcess_.readAll();
    if (bytes.isEmpty()) {
        return;
    }
    bytes.removeIf([](char byte) { return byte == '\0'; });
    const QString text = QString::fromLocal8Bit(bytes);
    if (!text.trimmed().isEmpty()) {
        appendLog(text.trimmed());
    }

    mountOutputProbe_.append(bytes);
    constexpr qsizetype probeLimit = 16384;
    if (mountOutputProbe_.size() > probeLimit) {
        mountOutputProbe_.remove(0, mountOutputProbe_.size() - probeLimit);
    }
    if (!mounted_
        && QString::fromLocal8Bit(mountOutputProbe_).contains(
            QStringLiteral("mounted at"), Qt::CaseInsensitive)) {
        mounted_ = true;
        activeMountPoint_ = selectedMountPoint();
        statusBar()->showMessage(
            tr("Mounted at %1").arg(QDir::toNativeSeparators(activeMountPoint_)));
        appendLog(tr("Mount is ready."));
        refreshActions();
    }
}

void MainWindow::requestUnmount() {
    if (mountProcess_.state() == QProcess::NotRunning) {
        mounted_ = false;
        refreshActions();
        maybeFinishClose();
        return;
    }
    if (unmountRequested_) {
        return;
    }

    unmountRequested_ = true;
    mounted_ = false;
    appendLog(tr("Requesting unmount…"));
    statusBar()->showMessage(tr("Unmounting…"));
    const qint64 written = mountProcess_.write("unmount\n");
    if (written < 0) {
        appendLog(tr("Could not write the unmount request: %1").arg(mountProcess_.errorString()));
    }
    unmountTimer_.start();
    refreshActions();
}

void MainWindow::unmountTimedOut() {
    if (mountProcess_.state() == QProcess::NotRunning) {
        return;
    }
    appendLog(tr("steam2fs.exe did not exit after the unmount request; terminating it."));
    statusBar()->showMessage(tr("Unmount timed out; terminating steam2fs.exe…"));
    mountProcess_.terminate();
}

void MainWindow::mountFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    mountOutputReady();
    unmountTimer_.stop();
    const bool expected = unmountRequested_ && exitStatus == QProcess::NormalExit && exitCode == 0;
    mounted_ = false;
    activeMountPoint_.clear();
    buildFile_.reset();

    if (expected) {
        appendLog(tr("Unmount complete."));
        statusBar()->showMessage(tr("Not mounted"));
    } else {
        const QString message = tr("steam2fs.exe exited with code %1 (%2).")
                                    .arg(exitCode)
                                    .arg(exitStatus == QProcess::CrashExit ? tr("crashed") : tr("normal exit"));
        appendLog(message);
        statusBar()->showMessage(message);
        if (!closePending_) {
            QMessageBox::warning(this, tr("Mount ended"), message);
        }
    }
    unmountRequested_ = false;
    refreshActions();
    maybeFinishClose();
}

void MainWindow::mountError(QProcess::ProcessError error) {
    const QString message = tr("steam2fs.exe %1: %2")
                                .arg(processErrorText(error), mountProcess_.errorString());
    appendLog(message);
    statusBar()->showMessage(message);
    if (error == QProcess::FailedToStart) {
        mounted_ = false;
        unmountTimer_.stop();
        buildFile_.reset();
        QMessageBox::critical(this, tr("Cannot mount"), message);
    }
    refreshActions();
    maybeFinishClose();
}

void MainWindow::playGame() {
    if (!mounted_ || activeMountPoint_.isEmpty()) {
        return;
    }

    QString root = activeMountPoint_;
    if (root.size() == 2 && root.at(1) == QLatin1Char(':')) {
        root += QLatin1Char('/');
    }
    const QString executable = firstMountedExecutable(root);
    if (executable.isEmpty()) {
        const QString message = tr("The mounted composition does not contain an executable.");
        appendLog(message);
        QMessageBox::critical(this, tr("Cannot play"), message);
        return;
    }

    const std::wstring nativeExecutable =
        QDir::toNativeSeparators(executable).toStdWString();
    const std::wstring nativeArguments = launchArguments_->text().toStdWString();
    const std::wstring nativeDirectory =
        QDir::toNativeSeparators(QFileInfo(executable).absolutePath()).toStdWString();
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.hwnd = reinterpret_cast<HWND>(winId());
    launch.lpVerb = L"open";
    launch.lpFile = nativeExecutable.c_str();
    launch.lpParameters = nativeArguments.empty() ? nullptr : nativeArguments.c_str();
    launch.lpDirectory = nativeDirectory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        const QString message = tr("Could not launch %1 through the Windows shell: error %2")
                                    .arg(QFileInfo(executable).fileName())
                                    .arg(GetLastError());
        appendLog(message);
        QMessageBox::critical(this, tr("Cannot play"), message);
        return;
    }

    const DWORD processId = launch.hProcess ? GetProcessId(launch.hProcess) : 0;
    if (launch.hProcess) {
        CloseHandle(launch.hProcess);
    }
    saveSettings();
    appendLog(
        tr("Launched %1 through the Windows shell (PID %2). The game process is not managed by this application.")
            .arg(QFileInfo(executable).fileName())
            .arg(processId));
}

void MainWindow::appendLog(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    diagnosticLog_->appendPlainText(QTime::currentTime().toString(QStringLiteral("[HH:mm:ss] ")) + trimmed);
}

void MainWindow::showAbout() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About Steam2 Depot Manager"));
    dialog.resize(680, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QTextBrowser(&dialog);
    text->setOpenExternalLinks(true);
    text->setHtml(tr(
        "<h2>Steam2 Depot Manager</h2>"
        "<p>A Qt Widgets front end for composing and mounting loose Steam2 depot revisions. "
        "Mounted writes are ephemeral and use the worker's 512 MiB default RAM quota.</p>"
        "<h3>Project license</h3>"
        "<p>steam2-winfsp is licensed under the "
        "<a href=\"https://www.gnu.org/licenses/lgpl-3.0.html\">GNU Lesser General Public License "
        "version 3.0 or later (LGPL-3.0-or-later)</a>.</p>"
        "<h3>Qt</h3>"
        "<p>This application dynamically links to Qt 6 and uses Qt under the GNU Lesser General "
        "Public License version 3. Qt is Copyright &copy; The Qt Company Ltd. and other contributors. "
        "The dynamically linked Qt libraries may be replaced with compatible builds. "
        "See <a href=\"https://www.qt.io/licensing/open-source-lgpl-obligations\">Qt open-source "
        "licensing</a>.</p>"
        "<h3>WinFsp</h3>"
        "<p><b>WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos</b></p>"
        "<p>Repository: <a href=\"https://github.com/winfsp/winfsp\">"
        "https://github.com/winfsp/winfsp</a></p>"
        "<p>WinFsp is licensed under GPLv3 with its FLOSS linking exception. Full project and "
        "third-party terms are provided in LICENSE and THIRD_PARTY_NOTICES.txt with the source "
        "distribution.</p>"));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    if (scanWatcher_.isRunning() || mountProcess_.state() != QProcess::NotRunning) {
        closePending_ = true;
        if (mountProcess_.state() != QProcess::NotRunning) {
            requestUnmount();
        }
        if (scanWatcher_.isRunning()) {
            scanStatus_->setText(tr("Finishing the current scan before closing…"));
        }
        event->ignore();
        return;
    }
    event->accept();
}

void MainWindow::maybeFinishClose() {
    if (!closePending_ || scanWatcher_.isRunning()
        || mountProcess_.state() != QProcess::NotRunning) {
        return;
    }
    closePending_ = false;
    QTimer::singleShot(0, this, [this] { close(); });
}

} // namespace s2gui
