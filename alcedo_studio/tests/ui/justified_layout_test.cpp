//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Justified-rows Library layout (issue 745): the row geometry of JustifiedRowLayout, the window
// of JustifiedLayoutModel over an AlbumThumbnailModel, and the stored layout choice of
// LibraryViewPreferences. Also the color label names of LibraryViewPreferences (issue 770).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTranslator>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/justified_layout_model.hpp"
#include "ui/alcedo_main/album_backend/justified_row_layout.hpp"
#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui::test {
namespace {

constexpr double kWidth   = 1000.0;
constexpr double kTarget  = 200.0;
constexpr double kSpacing = 8.0;
constexpr double kCaption = 22.0;

auto Geometry(double width = kWidth, double target = kTarget) -> JustifiedLayoutGeometry {
  return JustifiedLayoutGeometry{width, target, kSpacing, kCaption};
}

/// Landscape, portrait, square, and panorama photos in a repeating order.
auto MixedAspectRatios(int count) -> std::vector<double> {
  const std::vector<double> pattern{1.5, 2.0 / 3.0, 1.0, 16.0 / 9.0, 0.75, 3.0, 1.5, 4.0 / 5.0};
  std::vector<double>       out;
  for (int i = 0; i < count; ++i) {
    out.push_back(pattern[static_cast<size_t>(i) % pattern.size()]);
  }
  return out;
}

auto Items(const std::vector<double>& aspect_ratios, int first_id = 1) -> std::vector<AlbumItem> {
  std::vector<AlbumItem> items;
  for (size_t i = 0; i < aspect_ratios.size(); ++i) {
    AlbumItem item;
    item.element_id   = static_cast<sl_element_id_t>(first_id + static_cast<int>(i));
    item.file_id      = item.element_id;
    item.image_id     = static_cast<image_id_t>(1000 + first_id + static_cast<int>(i));
    item.file_name    = QStringLiteral("photo-%1.dng").arg(item.element_id);
    item.aspect_ratio = aspect_ratios[i];
    items.push_back(item);
  }
  return items;
}

/// Every complete row: one photo height for all its tiles, tiles from x 0 to the view width
/// with kSpacing gaps, and tile widths in the ratio of the photo aspect ratios.
void ExpectJustifiedRows(const JustifiedRowLayout& layout, const std::vector<double>& aspects) {
  ASSERT_FALSE(layout.rows().empty());
  for (const auto& row : layout.rows()) {
    double x = 0.0;
    for (int k = 0; k < row.count; ++k) {
      const int  index = row.first + k;
      const auto tile  = layout.TileRect(index);
      ASSERT_TRUE(tile.has_value()) << index;
      EXPECT_DOUBLE_EQ(tile->y, row.top) << index;
      EXPECT_DOUBLE_EQ(tile->height, row.photo_height + kCaption) << index;
      EXPECT_NEAR(tile->x, x, 1e-6) << index;
      x = tile->x + tile->width + kSpacing;
      if (row.complete) {
        EXPECT_NEAR(
            tile->width / row.photo_height,
            std::clamp(aspects[static_cast<size_t>(index)], JustifiedRowLayout::kMinAspectRatio,
                       JustifiedRowLayout::kMaxAspectRatio),
            1e-6)
            << index;
      }
    }
    if (row.complete) {
      EXPECT_NEAR(x - kSpacing, kWidth, 1e-6) << "row of photo " << row.first;
    }
  }
}

TEST(JustifiedRowLayoutTest, CompleteRowsFillTheViewWidthWithOneHeightAndAspectProportionalTiles) {
  const auto         aspects = MixedAspectRatios(40);
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry());
  layout.Reset(aspects);

  ExpectJustifiedRows(layout, aspects);
  int laid_out = 0;
  for (const auto& row : layout.rows()) {
    laid_out += row.count;
    EXPECT_GE(row.photo_height, kTarget * JustifiedRowLayout::kMinRowHeightRatio);
    EXPECT_LE(row.photo_height, kTarget * JustifiedRowLayout::kMaxRowHeightRatio);
  }
  EXPECT_EQ(laid_out, 40);
  // Rows follow each other with one gap.
  for (size_t r = 1; r < layout.rows().size(); ++r) {
    const auto& previous = layout.rows()[r - 1];
    EXPECT_DOUBLE_EQ(layout.rows()[r].top,
                     previous.top + previous.photo_height + kCaption + kSpacing);
  }
  // The last row continues with the next page: target height, from the left edge.
  EXPECT_FALSE(layout.rows().back().complete);
  EXPECT_DOUBLE_EQ(layout.rows().back().photo_height, kTarget);
}

TEST(JustifiedRowLayoutTest, AppendedPageLaysOutOnlyTheLastRowAndTheRowsAfterIt) {
  const auto         aspects = MixedAspectRatios(60);
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry());
  layout.Reset(std::vector<double>(aspects.begin(), aspects.begin() + 30));
  const auto rows_before = layout.rows();
  ASSERT_GE(rows_before.size(), 2U);

  layout.Append(std::vector<double>(aspects.begin() + 30, aspects.end()));
  EXPECT_EQ(layout.first_changed_row(), static_cast<int>(rows_before.size()) - 1);
  for (size_t r = 0; r + 1 < rows_before.size(); ++r) {
    EXPECT_EQ(layout.rows()[r].first, rows_before[r].first);
    EXPECT_EQ(layout.rows()[r].count, rows_before[r].count);
    EXPECT_DOUBLE_EQ(layout.rows()[r].photo_height, rows_before[r].photo_height);
  }

  // The same photos in one page give the same rows.
  JustifiedRowLayout whole;
  whole.SetGeometry(Geometry());
  whole.Reset(aspects);
  ASSERT_EQ(whole.rows().size(), layout.rows().size());
  for (size_t r = 0; r < whole.rows().size(); ++r) {
    EXPECT_EQ(whole.rows()[r].first, layout.rows()[r].first);
    EXPECT_DOUBLE_EQ(whole.rows()[r].photo_height, layout.rows()[r].photo_height);
  }
}

TEST(JustifiedRowLayoutTest, InsertLaysOutFromTheRowOfTheInsertedPhoto) {
  const auto         aspects = MixedAspectRatios(40);
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry());
  layout.Reset(aspects);
  const auto rows_before = layout.rows();
  const int  row         = layout.RowOfIndex(20);
  ASSERT_GT(row, 0);

  layout.Insert(20, {1.0, 1.0});
  EXPECT_EQ(layout.first_changed_row(), row);
  for (int r = 0; r < row; ++r) {
    EXPECT_EQ(layout.rows()[static_cast<size_t>(r)].first,
              rows_before[static_cast<size_t>(r)].first);
  }
  EXPECT_EQ(layout.ItemCount(), 42);
  layout.Remove(20, 2);
  ASSERT_EQ(layout.rows().size(), rows_before.size());
  for (size_t r = 0; r < rows_before.size(); ++r) {
    EXPECT_EQ(layout.rows()[r].first, rows_before[r].first);
    EXPECT_DOUBLE_EQ(layout.rows()[r].photo_height, rows_before[r].photo_height);
  }
}

TEST(JustifiedRowLayoutTest, WidePanoramaRowKeepsTheMinimumHeightAndTheViewWidth) {
  // A narrow view: a 5:1 panorama at the view width would be 80 px high, under the 100 px minimum.
  constexpr double   kNarrow = 400.0;
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry(kNarrow));
  layout.Reset({5.0, 5.0, 1.5});
  ASSERT_GE(layout.rows().size(), 2U);
  const auto& first = layout.rows().front();
  EXPECT_EQ(first.count, 1);
  EXPECT_TRUE(first.complete);
  EXPECT_DOUBLE_EQ(first.photo_height, kTarget * JustifiedRowLayout::kMinRowHeightRatio);
  EXPECT_NEAR(layout.TileRect(0)->width, kNarrow, 1e-6);
}

TEST(JustifiedRowLayoutTest, UnknownAspectRatioUsesTheClassicThreeByTwoBox) {
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry(kWidth, 100.0));
  layout.Reset({0.0, -1.0, 1.5});
  const auto tile = layout.TileRect(0);
  ASSERT_TRUE(tile.has_value());
  // The last row keeps the target height: width = 1.5 * 100.
  EXPECT_DOUBLE_EQ(tile->width, 150.0);
  EXPECT_DOUBLE_EQ(layout.TileRect(1)->width, 150.0);
}

TEST(JustifiedRowLayoutTest, HitTestsFindTheTileAndTheGapsFindNone) {
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry());
  layout.Reset(MixedAspectRatios(30));
  const auto tile = *layout.TileRect(5);
  EXPECT_EQ(layout.IndexAt(tile.x + 1, tile.y + 1), 5);
  EXPECT_EQ(layout.IndexAt(tile.x + tile.width + kSpacing / 2, tile.y + 1), -1);
  EXPECT_EQ(layout.NearestIndex(tile.x + tile.width + kSpacing / 4, tile.y + 1), 5);
  const auto inside = layout.IndicesInRect(tile.x + 1, tile.y + 1, tile.x + 2, tile.y + 2);
  ASSERT_EQ(inside.size(), 1U);
  EXPECT_EQ(inside.front(), 5);
}

TEST(JustifiedRowLayoutTest, EstimatedHeightCoversThePhotosThatAreNotLoaded) {
  JustifiedRowLayout layout;
  layout.SetGeometry(Geometry());
  layout.Reset(std::vector<double>(50, 1.5));
  const double loaded = layout.LoadedHeight();
  EXPECT_DOUBLE_EQ(layout.EstimatedHeight(50), loaded);
  // 1000 px rows of 1.5 x 200 px photos with 8 px gaps hold 3 photos.
  const double pitch = kTarget + kCaption + kSpacing;
  EXPECT_DOUBLE_EQ(layout.EstimatedHeight(56), loaded + 2 * pitch);
  EXPECT_DOUBLE_EQ(layout.EstimatedRowTop(53), loaded + kSpacing + pitch);
  EXPECT_EQ(layout.EstimatedIndexAtY(loaded + kSpacing + pitch + 1), 53);
}

class JustifiedLayoutModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    model_.setSource(&source_);
    model_.setGeometry(kWidth, kTarget, kSpacing, kCaption);
    model_.setViewportHeight(500);
  }

  AlbumThumbnailModel  source_;
  JustifiedLayoutModel model_;
};

TEST_F(JustifiedLayoutModelTest, WindowHoldsTheRowsNearTheViewportAndForwardsPhotoRoles) {
  source_.resetModel(Items(MixedAspectRatios(200)), 200);
  ASSERT_GT(model_.count(), 0);
  EXPECT_EQ(model_.firstIndex(), 0);
  const auto& layout = model_.layout();
  const auto  range  = layout.IndexRangeForY(0, 500);
  EXPECT_EQ(model_.count(), range.last - range.first + 1);

  const auto roles = model_.roleNames();
  EXPECT_EQ(roles.value(AlbumThumbnailModel::FileName), QByteArray("fileName"));
  EXPECT_EQ(roles.value(JustifiedLayoutModel::TileWidth), QByteArray("tileWidth"));
  const auto index = model_.index(3, 0);
  EXPECT_EQ(model_.data(index, AlbumThumbnailModel::FileName).toString(),
            QStringLiteral("photo-4.dng"));
  EXPECT_EQ(model_.data(index, JustifiedLayoutModel::PhotoIndex).toInt(), 3);
  EXPECT_DOUBLE_EQ(model_.data(index, JustifiedLayoutModel::TileWidth).toDouble(),
                   layout.TileRect(3)->width);
  EXPECT_DOUBLE_EQ(model_.contentHeight(), layout.LoadedHeight());
}

TEST_F(JustifiedLayoutModelTest, ScrollMovesTheWindowEndsWithoutAReset) {
  source_.resetModel(Items(MixedAspectRatios(300)), 300);
  QSignalSpy  resets(&model_, &QAbstractItemModel::modelReset);
  QSignalSpy  inserted(&model_, &QAbstractItemModel::rowsInserted);
  QSignalSpy  removed(&model_, &QAbstractItemModel::rowsRemoved);

  const auto& rows = model_.layout().rows();
  ASSERT_GT(rows.size(), 4U);
  // One row down: the first row leaves, a new row enters at the end.
  model_.setViewportY(rows[1].top);
  EXPECT_EQ(resets.count(), 0);
  EXPECT_GE(removed.count(), 1);
  EXPECT_GE(inserted.count(), 1);
  EXPECT_EQ(model_.firstIndex(), rows[1].first);
  // The photo at the top of the window is the first photo of row 1.
  EXPECT_EQ(model_.data(model_.index(0, 0), JustifiedLayoutModel::PhotoIndex).toInt(),
            rows[1].first);
}

TEST_F(JustifiedLayoutModelTest, AnchoredZoomKeepsThePointOfTheAnchorTileAtItsViewportHeight) {
  source_.resetModel(Items(MixedAspectRatios(300)), 300);
  model_.setViewportY(900);
  const int  anchor = model_.indexAt(400, 900 + 250);
  const auto before = model_.tileRect(anchor);
  ASSERT_GE(anchor, 0);
  const double fraction = (900 + 250 - before.y()) / before.height();

  const double y =
      model_.setGeometryAnchored(kWidth, 140, kSpacing, kCaption, anchor, fraction, 250);
  const auto after = model_.tileRect(anchor);
  EXPECT_DOUBLE_EQ(model_.viewportY(), y);
  EXPECT_NEAR(after.y() + fraction * after.height() - y, 250.0, 1e-6);
  EXPECT_GE(model_.count(), 1);
  EXPECT_LE(model_.firstIndex(), anchor);
  EXPECT_GE(model_.firstIndex() + model_.count() - 1, anchor);
}

TEST_F(JustifiedLayoutModelTest, SourceInsertInsideTheWindowInsertsRowsAndKeepsTheOthers) {
  source_.resetModel(Items(MixedAspectRatios(100)), 100);
  const int  count_before = model_.count();
  const auto third        = model_.data(model_.index(2, 0), AlbumThumbnailModel::ElementId);
  QSignalSpy resets(&model_, &QAbstractItemModel::modelReset);
  QSignalSpy inserted(&model_, &QAbstractItemModel::rowsInserted);

  // Photos 1 and 2 stay; a new photo goes before the old third photo.
  std::vector<sl_element_id_t>                   order;
  std::unordered_map<sl_element_id_t, AlbumItem> added;
  for (const auto& item : source_.items()) {
    order.push_back(item.element_id);
  }
  const auto extra = Items({1.0}, 500);
  order.insert(order.begin() + 2, extra.front().element_id);
  added.emplace(extra.front().element_id, extra.front());
  source_.applyOrder(order, added, 101, false);

  EXPECT_EQ(resets.count(), 0);
  ASSERT_GE(inserted.count(), 1);
  EXPECT_EQ(inserted.first().at(1).toInt(), 2);
  EXPECT_EQ(model_.data(model_.index(2, 0), AlbumThumbnailModel::ElementId).toUInt(), 500U);
  EXPECT_EQ(model_.data(model_.index(3, 0), AlbumThumbnailModel::ElementId), third);
  EXPECT_GE(model_.count(), count_before - 8);
  EXPECT_EQ(model_.layout().ItemCount(), 101);
}

TEST_F(JustifiedLayoutModelTest, AppendedPageExtendsTheLayoutAndTheEstimate) {
  source_.resetModel(Items(MixedAspectRatios(40)), 120);
  const double estimated = model_.contentHeight();
  EXPECT_GT(estimated, model_.loadedHeight());
  source_.appendPage(Items(MixedAspectRatios(40), 41));
  EXPECT_EQ(model_.layout().ItemCount(), 80);
  EXPECT_GT(model_.loadedHeight(), 0.0);
  EXPECT_GT(model_.contentHeight(), model_.loadedHeight());
}

class LibraryViewPreferencesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QCoreApplication::setOrganizationName("PuerhLabTest");
    QCoreApplication::setApplicationName("LibraryViewPreferencesTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QSettings settings;
    settings.clear();
    settings.sync();
  }

  QTemporaryDir settings_dir_;
};

TEST_F(LibraryViewPreferencesTest, GridLayoutDefaultsToClassic) {
  LibraryViewPreferences preferences;
  EXPECT_EQ(preferences.grid_layout(), QStringLiteral("classic"));
}

TEST_F(LibraryViewPreferencesTest, GridLayoutIsStoredAndReadByANewInstance) {
  LibraryViewPreferences first;
  QSignalSpy             changed(&first, &LibraryViewPreferences::gridLayoutChanged);
  EXPECT_TRUE(first.setGridLayout(QStringLiteral("justified")));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(first.setGridLayout(QStringLiteral("justified")));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(QSettings{}.value("library/gridLayout").toString(), QStringLiteral("justified"));

  LibraryViewPreferences second;
  EXPECT_EQ(second.grid_layout(), QStringLiteral("justified"));
}

TEST_F(LibraryViewPreferencesTest, UnknownGridLayoutIsRejectedAndUnknownStoredValueReadsClassic) {
  LibraryViewPreferences preferences;
  QSignalSpy             changed(&preferences, &LibraryViewPreferences::gridLayoutChanged);
  EXPECT_FALSE(preferences.setGridLayout(QStringLiteral("masonry")));
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(preferences.grid_layout(), QStringLiteral("classic"));

  QSettings{}.setValue("library/gridLayout", QStringLiteral("masonry"));
  LibraryViewPreferences reread;
  EXPECT_EQ(reread.grid_layout(), QStringLiteral("classic"));
}

// ── Color label names (#770) ────────────────────────────────────────────────

auto ColorNames() -> QStringList {
  return {QStringLiteral("Red"),    QStringLiteral("Orange"), QStringLiteral("Yellow"),
          QStringLiteral("Green"),  QStringLiteral("Blue"),   QStringLiteral("Purple"),
          QStringLiteral("Gray")};
}

/// Translates the color names of LibraryViewPreferences, like an installed language catalog.
class ColorNameTranslator final : public QTranslator {
 public:
  auto translate(const char* context, const char* source_text, const char* disambiguation,
                 int n) const -> QString override {
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);
    if (QLatin1String(context) == QLatin1String("alcedo::ui::LibraryViewPreferences") &&
        QLatin1String(source_text) == QLatin1String("Red")) {
      return QStringLiteral("红色");
    }
    return {};
  }
  [[nodiscard]] auto isEmpty() const -> bool override { return false; }
};

TEST_F(LibraryViewPreferencesTest, ColorLabelNamesDefaultToTheColorNames) {
  LibraryViewPreferences preferences;
  EXPECT_EQ(preferences.color_label_names(), ColorNames());
  EXPECT_EQ(preferences.color_label_default_names(), ColorNames());
  EXPECT_EQ(preferences.color_label_user_names(), QStringList(7, QString()));
}

TEST_F(LibraryViewPreferencesTest, ColorLabelNameIsStoredAndReadByANewInstance) {
  LibraryViewPreferences first;
  QSignalSpy changed(&first, &LibraryViewPreferences::colorLabelNamesChanged);
  EXPECT_TRUE(first.SetColorLabelName(0, QStringLiteral("  Client picks  ")));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(first.SetColorLabelName(0, QStringLiteral("Client picks")));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(first.color_label_names().at(0), QStringLiteral("Client picks"));
  EXPECT_EQ(first.color_label_user_names().at(0), QStringLiteral("Client picks"));
  EXPECT_EQ(QSettings{}.value("library/colorLabels/0/name").toString(),
            QStringLiteral("Client picks"));

  LibraryViewPreferences second;
  QStringList            expected = ColorNames();
  expected[0]                     = QStringLiteral("Client picks");
  EXPECT_EQ(second.color_label_names(), expected);
}

TEST_F(LibraryViewPreferencesTest, EmptyColorLabelNameReturnsTheColorName) {
  QSettings{}.setValue("library/colorLabels/3/name", QStringLiteral("Portfolio"));
  LibraryViewPreferences preferences;
  EXPECT_EQ(preferences.color_label_names().at(3), QStringLiteral("Portfolio"));

  QSignalSpy changed(&preferences, &LibraryViewPreferences::colorLabelNamesChanged);
  EXPECT_TRUE(preferences.SetColorLabelName(3, QStringLiteral("   ")));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(preferences.color_label_names(), ColorNames());
  EXPECT_TRUE(preferences.color_label_user_names().at(3).isEmpty());
  EXPECT_FALSE(QSettings{}.contains("library/colorLabels/3/name"));

  // A blank stored name also reads as the color name.
  QSettings{}.setValue("library/colorLabels/5/name", QStringLiteral(" "));
  LibraryViewPreferences reread;
  EXPECT_EQ(reread.color_label_names(), ColorNames());
}

TEST_F(LibraryViewPreferencesTest, ColorLabelNameOutsideTheCodesIsRejectedAndLongNamesAreCut) {
  LibraryViewPreferences preferences;
  QSignalSpy changed(&preferences, &LibraryViewPreferences::colorLabelNamesChanged);
  EXPECT_FALSE(preferences.SetColorLabelName(-1, QStringLiteral("x")));
  EXPECT_FALSE(preferences.SetColorLabelName(7, QStringLiteral("x")));
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(preferences.color_label_names(), ColorNames());

  EXPECT_TRUE(preferences.SetColorLabelName(6, QString(60, QLatin1Char('a'))));
  EXPECT_EQ(preferences.color_label_names().at(6).size(),
            LibraryViewPreferences::kColorLabelNameMaxLength);
}

// Issue 780: the cut counted UTF-16 units and kept the first half of an emoji at character 40.
TEST_F(LibraryViewPreferencesTest, EmojiAtCharacter40StaysWholeInTheAnswerTheStoreAndARestart) {
  const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");  // U+1F600, a surrogate pair
  ASSERT_EQ(emoji.size(), 2);
  const QString x39(39, QLatin1Char('x'));
  {
    LibraryViewPreferences preferences;
    ASSERT_TRUE(preferences.SetColorLabelName(4, x39 + emoji + QStringLiteral("tail")));
    // 39 x and the whole emoji: 40 characters, 41 UTF-16 units.
    EXPECT_EQ(preferences.color_label_names().at(4), x39 + emoji);
    EXPECT_FALSE(preferences.color_label_names().at(4).back().isHighSurrogate());
    EXPECT_EQ(QSettings{}.value("library/colorLabels/4/name").toString(), x39 + emoji);
  }
  LibraryViewPreferences restarted;
  EXPECT_EQ(restarted.color_label_names().at(4), x39 + emoji);

  // 40 emoji are 40 characters; the 41st goes.
  QString forty_one;
  for (int i = 0; i < 41; ++i) forty_one += emoji;
  ASSERT_TRUE(restarted.SetColorLabelName(5, forty_one));
  EXPECT_EQ(restarted.color_label_names().at(5), forty_one.left(80));

  // A lone half of a pair is not text and goes.
  ASSERT_TRUE(restarted.SetColorLabelName(
      6, QStringLiteral("ab") + QChar(0xD83D) + QStringLiteral("c") + QChar(0xDE00)));
  EXPECT_EQ(restarted.color_label_names().at(6), QStringLiteral("abc"));
}

TEST_F(LibraryViewPreferencesTest, ControlCharactersLeaveTheNameAndLineBreaksBecomeSpaces) {
  LibraryViewPreferences preferences;
  QString                with_controls = QStringLiteral("a");
  with_controls += QChar(0x0000);
  with_controls += QStringLiteral("b");
  with_controls += QChar(0x202E);  // right-to-left override
  with_controls += QStringLiteral("c");
  with_controls += QChar(0x2066);  // left-to-right isolate
  with_controls += QChar(0x007F);
  with_controls += QChar(0x0090);
  with_controls += QStringLiteral("d");
  ASSERT_TRUE(preferences.SetColorLabelName(0, with_controls));
  EXPECT_EQ(preferences.color_label_names().at(0), QStringLiteral("abcd"));
  EXPECT_EQ(QSettings{}.value("library/colorLabels/0/name").toString(), QStringLiteral("abcd"));

  ASSERT_TRUE(preferences.SetColorLabelName(1, QStringLiteral("\nClient\npicks\r\n\t")));
  EXPECT_EQ(preferences.color_label_names().at(1), QStringLiteral("Client picks"));
  ASSERT_TRUE(preferences.SetColorLabelName(
      2, QStringLiteral("Print") + QChar(0x2028) + QStringLiteral("30x40")));
  EXPECT_EQ(preferences.color_label_names().at(2), QStringLiteral("Print 30x40"));

  // A name of control characters only is empty, so the code shows its color name.
  ASSERT_TRUE(preferences.SetColorLabelName(3, QString(QChar(0x202E)) + QChar(0x0001)));
  EXPECT_EQ(preferences.color_label_names().at(3), ColorNames().at(3));
  EXPECT_FALSE(QSettings{}.contains("library/colorLabels/3/name"));

  // A stored name from an older build reads with the same rule.
  QSettings{}.setValue("library/colorLabels/5/name",
                       QStringLiteral("old") + QChar(0x0000) + QStringLiteral("\nname"));
  LibraryViewPreferences reread;
  EXPECT_EQ(reread.color_label_names().at(5), QStringLiteral("old name"));

  // The text of the Settings field keeps the spaces at its ends while the user types.
  EXPECT_EQ(LibraryViewPreferences::EditableColorLabelNameText(QStringLiteral(" a\nb ")),
            QStringLiteral(" a b "));
  EXPECT_EQ(reread.EditableColorLabelName(QString(45, QLatin1Char('y'))),
            QString(40, QLatin1Char('y')));
}

TEST_F(LibraryViewPreferencesTest, DefaultColorLabelNamesFollowTheLanguage) {
  LibraryViewPreferences preferences;
  ASSERT_TRUE(preferences.SetColorLabelName(1, QStringLiteral("Client picks")));
  QSignalSpy changed(&preferences, &LibraryViewPreferences::colorLabelNamesChanged);

  ColorNameTranslator translator;
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));
  i18n::TranslationNotifier::Instance().NotifyLanguageChanged();
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(preferences.color_label_names().at(0), QStringLiteral("红色"));
  EXPECT_EQ(preferences.color_label_default_names().at(0), QStringLiteral("红色"));
  EXPECT_EQ(preferences.color_label_names().at(1), QStringLiteral("Client picks"));

  ASSERT_TRUE(QCoreApplication::removeTranslator(&translator));
  EXPECT_EQ(preferences.color_label_names().at(0), QStringLiteral("Red"));
}

}  // namespace
}  // namespace alcedo::ui::test
