//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// lut.list through the command registry of an in-process headless session with a temporary LUT
// library root.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "app/lut_library_test_support.hpp"
#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::automation {
namespace {

using test::InProcessAutomationSession;

constexpr std::string_view kShaperOnlyCube = "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n";

class AutomationLutCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    root_ = std::filesystem::weakly_canonical(temp_dir_) / "luts";
    alcedo::test::WriteBytes(root_ / "user" / "Teal Orange.cube",
                             alcedo::test::UserCube("Teal Orange"));
    alcedo::test::WriteBytes(root_ / "user" / "shaper.cube", kShaperOnlyCube);
    alcedo::test::WriteBytes(root_ / "films" / "kodak_vision3_250d_5207.cube",
                             alcedo::test::OfficialCube());
    preferences_ = std::make_shared<alcedo::test::PreferenceStore>();
  }

  auto LutServices() -> ui::ApplicationModuleHost::PackageServiceFactories {
    ui::ApplicationModuleHost::PackageServiceFactories factories;
    factories.library_options = [this] {
      LutLibraryServiceOptions options;
      options.preferences       = std::make_unique<alcedo::test::MemoryPreferences>(preferences_);
      options.default_root      = root_;
      options.scan_worker_count = 1;
      options.open_url          = [](const QUrl&) { return true; };
      return options;
    };
    return factories;
  }

  static auto Names(const QJsonObject& result) -> QStringList {
    QStringList names;
    for (const QJsonValue& lut : result.value("luts").toArray()) {
      names.push_back(lut.toObject().value("name").toString());
    }
    return names;
  }

  static auto FindByPath(const QJsonObject& result, const QString& path) -> QJsonObject {
    for (const QJsonValue& lut : result.value("luts").toArray()) {
      if (lut.toObject().value("path").toString() == path) {
        return lut.toObject();
      }
    }
    return {};
  }

  std::filesystem::path                          root_;
  std::shared_ptr<alcedo::test::PreferenceStore> preferences_;
};

TEST_F(AutomationLutCommandsTest, ListsTheLibraryInBrowserOrderWithEditorValues) {
  InProcessAutomationSession session(QStringLiteral("headless"), LutServices());

  // The first call can arrive while the library loads its inventory; it waits for the load.
  const QJsonObject          all = session.CallResult(QStringLiteral("lut.list"));
  EXPECT_EQ(all.value("root").toString(), ui::test::PathToQString(root_));
  EXPECT_EQ(all.value("inventory_complete"), QJsonValue(true));
  EXPECT_EQ(all.value("total").toInt(), 3);
  EXPECT_EQ(all.value("has_more"), QJsonValue(false));
  EXPECT_EQ(Names(all), (QStringList{"Kodak Vision3 250D 5207", "shaper", "Teal Orange"}));

  const QJsonObject film = FindByPath(all, "films/kodak_vision3_250d_5207.cube");
  EXPECT_EQ(film.value("category").toString(), QStringLiteral("film_simulation"));
  EXPECT_EQ(film.value("official"), QJsonValue(true));
  EXPECT_EQ(film.value("film").toObject().value("brand").toString(), QStringLiteral("Kodak"));
  EXPECT_EQ(film.value("input_space").toString(), QStringLiteral("ACEScc"));
  EXPECT_EQ(film.value("lut_3d_size").toInt(), 2);
  EXPECT_FALSE(film.contains("package")) << "a loose official file has no package";

  // editor_value is the value that the LUT browser applies: a library reference, the display
  // name, and the default encodings.
  const QJsonObject editor_value = film.value("editor_value").toObject();
  EXPECT_EQ(editor_value.value("name").toString(), QStringLiteral("Kodak Vision3 250D 5207"));
  EXPECT_EQ(editor_value.value("input_encoding").toString(), QStringLiteral("acescc"));
  EXPECT_EQ(editor_value.value("output_encoding").toString(), QStringLiteral("acescc"));
  const auto reference = TaggedLutReferenceFromJson(nlohmann::json::parse(
      QJsonDocument(editor_value.value("reference").toObject()).toJson().toStdString()));
  EXPECT_EQ(reference, LutReference(LibraryLutReference{"films/kodak_vision3_250d_5207.cube"}));
  EXPECT_EQ(film.value("reference"), editor_value.value("reference"));

  const QJsonObject shaper = FindByPath(all, "user/shaper.cube");
  EXPECT_EQ(shaper.value("applicable"), QJsonValue(false));
  EXPECT_EQ(shaper.value("lut_1d_size").toInt(), 2);
  EXPECT_FALSE(shaper.contains("editor_value"));

  const QJsonObject user = FindByPath(all, "user/Teal Orange.cube");
  EXPECT_EQ(user.value("category").toString(), QStringLiteral("general"));
  EXPECT_EQ(user.value("title").toString(), QStringLiteral("Teal Orange"));
  EXPECT_EQ(user.value("applicable"), QJsonValue(true));
}

TEST_F(AutomationLutCommandsTest, FiltersAndPages) {
  InProcessAutomationSession session(QStringLiteral("headless"), LutServices());

  EXPECT_EQ(
      Names(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"query", "vision3 250d"}})),
      QStringList{"Kodak Vision3 250D 5207"});
  EXPECT_EQ(Names(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"query", "teal"}})),
            QStringList{"Teal Orange"});
  EXPECT_EQ(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"query", "zzzz"}})
                .value("total")
                .toInt(),
            0);
  EXPECT_EQ(Names(session.CallResult(QStringLiteral("lut.list"),
                                     QJsonObject{{"category", "film_simulation"}})),
            QStringList{"Kodak Vision3 250D 5207"});
  EXPECT_EQ(
      Names(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"category", "general"}})),
      (QStringList{"shaper", "Teal Orange"}));
  EXPECT_EQ(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"package", ""}})
                .value("total")
                .toInt(),
            3);
  EXPECT_EQ(
      session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"package", "spectral_film_lut"}})
          .value("total")
          .toInt(),
      0);
  EXPECT_EQ(session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"favorites", true}})
                .value("total")
                .toInt(),
            0);

  const QJsonObject page =
      session.CallResult(QStringLiteral("lut.list"), QJsonObject{{"offset", 1}, {"limit", 1}});
  EXPECT_EQ(page.value("total").toInt(), 3);
  EXPECT_EQ(page.value("offset").toInt(), 1);
  EXPECT_EQ(page.value("has_more"), QJsonValue(true));
  EXPECT_EQ(Names(page), QStringList{"shaper"});
}

TEST_F(AutomationLutCommandsTest, RejectsInvalidParameters) {
  InProcessAutomationSession session(QStringLiteral("headless"), LutServices());
  for (const QJsonObject& params : {QJsonObject{{"category", "film"}}, QJsonObject{{"limit", 0}},
                                    QJsonObject{{"limit", 501}}, QJsonObject{{"unknown", true}}}) {
    const auto response = session.Call(QStringLiteral("lut.list"), params);
    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->error.has_value())
        << QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(response->error->code, AutomationErrorCode::InvalidParams);
  }
}

}  // namespace
}  // namespace alcedo::automation
