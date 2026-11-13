//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// QML Connections maps a handler onFoo to the signal foo. A C++ signal that starts with an
// upper-case letter (DeleteTargetsFinished, StateChanged) gets no handler, and
// ignoreUnknownSignals hides the warning. These tests drive the production QML files
// (ImageActionsController, ShellSignals) through C++ fakes that
// have the production signal names, and check that every fake signal exists on the production
// class. A QML fake with lower-case signals would hide a handler that never runs in the
// application.

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QList>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <utility>

#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui::test {
namespace {

// LibraryMutationOperations: the delete result, emitted like OnDeleteImagesFinished.
class FakeLibraryMutations final : public QObject {
  Q_OBJECT

 public:
  void FinishDelete(const QVariantMap& result) {
    emit DeleteTargetsFinished(result);
    emit deleteTargetsFinished(result);
  }

 signals:
  void ElementsDeleted(const QVariantList& element_ids);
  void DeleteTargetsFinished(const QVariantMap& result);
  void deleteTargetsFinished(const QVariantMap& result);
};

// ImageController: the batch rating result and the focused image inspection.
class FakeImages final : public QObject {
  Q_OBJECT

 public:
  [[nodiscard]] auto      inspectionCount() const -> int { return inspection_count_; }

  Q_INVOKABLE QVariantMap GetFocusedImageInspection(uint element_id, uint image_id) {
    ++inspection_count_;
    return {{QStringLiteral("success"), true},
            {QStringLiteral("elementId"), element_id},
            {QStringLiteral("imageId"), image_id},
            {QStringLiteral("tiles"), QVariantList{}}};
  }

  void FinishRatings(const QVariantMap& result) {
    emit ImageRatingsFinished(result);
    emit imageRatingsFinished(result);
  }

 signals:
  void ImageRatingsFinished(const QVariantMap& result);
  void imageRatingsFinished(const QVariantMap& result);

 private:
  int inspection_count_ = 0;
};

// ImportExportHandler: the end of an export run.
class FakeImportExport final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool importRunning READ importRunning NOTIFY ImportStateChanged)
  Q_PROPERTY(int importCompleted READ importCompleted NOTIFY ImportStateChanged)

 public:
  [[nodiscard]] auto importRunning() const -> bool { return false; }
  [[nodiscard]] auto importCompleted() const -> int { return 0; }

  void               FinishExport(int succeeded, int failed, int skipped) {
    emit ExportFinished(succeeded, failed, skipped);
    emit exportFinished(succeeded, failed, skipped);
  }

 signals:
  void ImportStateChanged();
  void importStateChanged();
  void ExportFinished(int succeeded, int failed, int skipped);
  void exportFinished(int succeeded, int failed, int skipped);
};

// ProjectModule: acceleratorPreparing notifies through AcceleratorPreparationStateChanged.
class FakeProject final : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      bool acceleratorPreparing READ acceleratorPreparing NOTIFY AcceleratorPreparationStateChanged)
  Q_PROPERTY(QString serviceMessage READ serviceMessage NOTIFY ServiceStateChanged)
  Q_PROPERTY(bool projectEntered READ projectEntered NOTIFY ProjectEnteredChanged)

 public:
  [[nodiscard]] auto acceleratorPreparing() const -> bool { return accelerator_preparing_; }
  [[nodiscard]] auto serviceMessage() const -> QString { return {}; }
  [[nodiscard]] auto projectEntered() const -> bool { return false; }

  void               SetAcceleratorPreparing(bool preparing) {
    accelerator_preparing_ = preparing;
    emit AcceleratorPreparationStateChanged();
  }

 signals:
  void ServiceStateChanged();
  void AcceleratorPreparationStateChanged();
  void ProjectEnteredChanged();
  void ProjectLoadStateChanged();
  void ProjectChanged();
  void projectChanged();

 private:
  bool accelerator_preparing_ = false;
};

// Every signal of @p fake must exist with the same signature on @p production.
void ExpectSignalsOnProductionClass(const QMetaObject& fake, const QMetaObject& production) {
  for (int i = QObject::staticMetaObject.methodCount(); i < fake.methodCount(); ++i) {
    const QMetaMethod method = fake.method(i);
    if (method.methodType() != QMetaMethod::Signal) {
      continue;
    }
    EXPECT_GE(production.indexOfSignal(method.methodSignature().constData()), 0)
        << production.className() << " has no signal " << method.methodSignature().constData();
  }
}

constexpr char kModulesQml[]         = R"(
import QtQuick

QtObject {
    property QtObject editorSession: null
    property QtObject workspaceRouter: QtObject { property string workspace: "library" }
    property QtObject library: null
    property QtObject images: null
    property QtObject libraryMutations: null
    property QtObject importExport: null
    property QtObject project: null
    property QtObject interactionPolicy: QtObject {
        property int focusedElementId: 0
        property var pendingDeleteTargets: []
    }
    property QtObject folders: QtObject { property int currentFolderId: 0 }
    property QtObject adjustmentTransfer: QtObject {}
}
)";

constexpr char kHostQml[]            = R"(
import QtQuick

QtObject {
    property var snackbarMessages: []
    property int startupPreviewRequests: 0
    property var pendingDeleteTargets: []
    property var pendingRatingTarget: ({})
    function showSnackbar(message) {
        snackbarMessages = snackbarMessages.concat([String(message)])
    }
    function requestStartupPreview() { startupPreviewRequests += 1 }
    function updateWelcomeDialogVisibility() {}
    function setFocusedImage(item) {}
}
)";

// The ImageActionsController part that ShellSignals calls.
constexpr char kImageActionsQml[]    = R"(
import QtQuick

QtObject {
    property int inspectionRefreshCount: 0
    function refreshFocusedImageInspection() { inspectionRefreshCount += 1 }
}
)";

constexpr char kLanguageManagerQml[] = R"(
import QtQuick

QtObject { property string effectiveLanguageCode: "en" }
)";

auto           QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds = 30) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto AsVariant(QObject* object) -> QVariant { return QVariant::fromValue(object); }

auto SnackbarMessages(QObject* host) -> QStringList {
  return host->property("snackbarMessages").toStringList();
}

// Loads one production QML file with appModules, languageManager, and a host.
class ShellQmlHarness {
 public:
  explicit ShellQmlHarness(const QString& file_name) {
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    modules_          = Create(kModulesQml, QStringLiteral("file:///FakeModules.qml"));
    host_             = Create(kHostQml, QStringLiteral("file:///FakeHost.qml"));
    language_manager_ = Create(kLanguageManagerQml, QStringLiteral("file:///FakeLanguage.qml"));
    image_actions_    = Create(kImageActionsQml, QStringLiteral("file:///FakeImageActions.qml"));
    if (!modules_ || !host_ || !language_manager_ || !image_actions_) {
      return;
    }
    modules_->setProperty("images", AsVariant(&images_));
    modules_->setProperty("libraryMutations", AsVariant(&mutations_));
    modules_->setProperty("importExport", AsVariant(&import_export_));
    modules_->setProperty("project", AsVariant(&project_));
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), modules_.get());
    engine_.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                              language_manager_.get());
    QQmlComponent component(&engine_, QUrl::fromLocalFile(QmlDirectory() + "/" + file_name));
    object_.reset(component.create());
    for (const auto& error : component.errors()) warnings_.push_back(error.toString());
    if (object_) {
      object_->setProperty("host", AsVariant(host_.get()));
      if (file_name == QStringLiteral("ShellSignals.qml")) {
        object_->setProperty("imageActionsController", AsVariant(image_actions_.get()));
      }
    }
    ProcessEvents();
  }

  // Members are destroyed in reverse order: the QML objects go before the engine, and the
  // engine before the C++ fakes that the QML objects reference.
  FakeImages               images_;
  FakeLibraryMutations     mutations_;
  FakeImportExport         import_export_;
  FakeProject              project_;
  QQmlEngine               engine_;
  QStringList              warnings_;
  std::unique_ptr<QObject> modules_;
  std::unique_ptr<QObject> host_;
  std::unique_ptr<QObject> language_manager_;
  std::unique_ptr<QObject> image_actions_;
  std::unique_ptr<QObject> object_;

 private:
  [[nodiscard]] auto Create(const char* source, const QString& url) -> std::unique_ptr<QObject> {
    QQmlComponent component(&engine_);
    component.setData(QByteArray{source}, QUrl(url));
    std::unique_ptr<QObject> object(component.create());
    for (const auto& error : component.errors()) warnings_.push_back(error.toString());
    return object;
  }
};

TEST(UppercaseSignalQmlHandlersTest, FakesUseProductionSignalNames) {
  ExpectSignalsOnProductionClass(FakeLibraryMutations::staticMetaObject,
                                 LibraryMutationOperations::staticMetaObject);
  ExpectSignalsOnProductionClass(FakeImages::staticMetaObject, ImageController::staticMetaObject);
  ExpectSignalsOnProductionClass(FakeImportExport::staticMetaObject,
                                 ImportExportHandler::staticMetaObject);
  ExpectSignalsOnProductionClass(FakeProject::staticMetaObject, ProjectModule::staticMetaObject);
}

TEST(UppercaseSignalQmlHandlersTest, DeleteFailureShowsMessageAndClearsDeletedFocusedImage) {
  ShellQmlHarness harness(QStringLiteral("ImageActionsController.qml"));
  ASSERT_NE(harness.object_, nullptr) << harness.warnings_.join('\n').toStdString();
  harness.object_->setProperty("focusedImageTarget", QVariantMap{{QStringLiteral("elementId"), 7},
                                                                 {QStringLiteral("fileId"), 7},
                                                                 {QStringLiteral("imageId"), 70}});

  harness.mutations_.FinishDelete(
      {{QStringLiteral("success"), false},
       {QStringLiteral("message"), QStringLiteral("2 photos could not be deleted.")},
       {QStringLiteral("deletedElementIds"), QVariantList{7, 8}}});
  ProcessEvents();

  EXPECT_EQ(SnackbarMessages(harness.host_.get()),
            QStringList{QStringLiteral("2 photos could not be deleted.")});
  EXPECT_FALSE(
      harness.object_->property("focusedImageTarget").toMap().contains(QStringLiteral("imageId")));
}

TEST(UppercaseSignalQmlHandlersTest, BatchRatingResultShowsMessageAndRefreshesFocusedImage) {
  ShellQmlHarness harness(QStringLiteral("ImageActionsController.qml"));
  ASSERT_NE(harness.object_, nullptr) << harness.warnings_.join('\n').toStdString();
  harness.object_->setProperty("focusedImageTarget", QVariantMap{{QStringLiteral("elementId"), 7},
                                                                 {QStringLiteral("fileId"), 7},
                                                                 {QStringLiteral("imageId"), 70}});

  harness.images_.FinishRatings({{QStringLiteral("success"), true},
                                 {QStringLiteral("message"), QStringLiteral("Rated 2 images.")},
                                 {QStringLiteral("ratedElementIds"), QVariantList{7, 8}},
                                 {QStringLiteral("failedElementIds"), QVariantList{}}});
  ProcessEvents();

  EXPECT_EQ(harness.images_.inspectionCount(), 1);
  EXPECT_EQ(SnackbarMessages(harness.host_.get()), QStringList{QStringLiteral("Rated 2 images.")});
}

TEST(UppercaseSignalQmlHandlersTest, ExportEndShowsExportedCount) {
  ShellQmlHarness harness(QStringLiteral("ShellSignals.qml"));
  ASSERT_NE(harness.object_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.import_export_.FinishExport(3, 1, 0);
  ProcessEvents();

  EXPECT_EQ(SnackbarMessages(harness.host_.get()),
            QStringList{QStringLiteral("Exported 3 image(s).")});
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

// ProjectLaunchController.requestStartupPreview returns while the OpenCL kernels compile; the
// end of the preparation requests the preview again.
TEST(UppercaseSignalQmlHandlersTest, AcceleratorPreparationEndRequestsStartupPreview) {
  ShellQmlHarness harness(QStringLiteral("ShellSignals.qml"));
  ASSERT_NE(harness.object_, nullptr) << harness.warnings_.join('\n').toStdString();

  harness.project_.SetAcceleratorPreparing(true);
  ProcessEvents();
  EXPECT_EQ(harness.host_->property("startupPreviewRequests").toInt(), 0);

  harness.project_.SetAcceleratorPreparing(false);
  ProcessEvents();
  EXPECT_EQ(harness.host_->property("startupPreviewRequests").toInt(), 1);
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test

#include "uppercase_signal_qml_handlers_test.moc"
