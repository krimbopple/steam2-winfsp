#pragma once

#include "catalog.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QProcess>
#include <QTimer>
#include <QVector>

#include <memory>

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTemporaryFile;
class QTreeWidget;

namespace s2gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void loadSettings();
    void saveSettings() const;

    void addLibraryFolder();
    void removeLibraryFolders();
    void startScan();
    void scanFinished();
    void showCatalog();
    void updateScanProgress(qsizetype scanned, const QString& path);

    void addSelectedRevision();
    void removeCompositionEntry();
    void moveCompositionEntry(int offset);
    void refreshComposition();
    void refreshActions();

    void chooseMountPoint();
    void mountComposition();
    void requestUnmount();
    void mountOutputReady();
    void mountFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void mountError(QProcess::ProcessError error);
    void unmountTimedOut();
    void playGame();

    void appendLog(const QString& text);
    void showAbout();
    void maybeFinishClose();
    [[nodiscard]] QString selectedMountPoint() const;
    [[nodiscard]] const CatalogVersion* selectedCatalogVersion() const;

    QListWidget* libraryFolders_{};
    QPushButton* addFolderButton_{};
    QPushButton* removeFolderButton_{};
    QPushButton* scanButton_{};
    QProgressBar* scanProgress_{};
    QLabel* scanStatus_{};

    QTreeWidget* catalogTree_{};
    QPushButton* addRevisionButton_{};

    QListWidget* compositionList_{};
    QPushButton* removeCompositionButton_{};
    QPushButton* moveUpButton_{};
    QPushButton* moveDownButton_{};

    QComboBox* mountPoint_{};
    QLineEdit* launchArguments_{};

    QPushButton* mountButton_{};
    QPushButton* playButton_{};
    QPushButton* unmountButton_{};
    QPlainTextEdit* diagnosticLog_{};

    QFutureWatcher<Catalog> scanWatcher_;
    std::shared_ptr<const Catalog> catalog_;
    QVector<CatalogVersion> composition_;

    QProcess mountProcess_;
    QTimer unmountTimer_;
    std::unique_ptr<QTemporaryFile> buildFile_;
    QByteArray mountOutputProbe_;
    QString activeMountPoint_;
    bool mounted_{};
    bool unmountRequested_{};
    bool closePending_{};
};

} // namespace s2gui
