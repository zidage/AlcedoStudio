//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file simplified_chinese_catalog_test.cpp
/// @brief Audited zh_CN catalog coverage for the PR93-176 UI translation plan.
///
/// Reads the source-of-truth TS catalog with QXmlStreamReader and verifies the
/// 24 audited target contexts: presence, per-context message counts, finished
/// non-empty translations, Qt placeholder parity, and the approved Mask Group
/// terminology. A second section loads the compiled alcedo_main_zh_CN.qm with
/// QTranslator and proves QML and C++ contexts resolve through
/// QCoreApplication::translate.
///
/// The catalog test owns no production state and never writes the TS file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QXmlStreamReader>

namespace alcedo::ui::test {
namespace {

struct CatalogMessage {
  QString source;
  QString translation;
  bool    unfinished = false;
};

using CatalogMessages = QList<CatalogMessage>;
using Catalog         = QMap<QString, CatalogMessages>;

// Audited post-lupdate message counts per target context. lupdate removes
// obsolete keys (-no-obsolete), so these counts describe the live extraction,
// not the stale checked-in rows.
const QMap<QString, int>& ExpectedContextMessageCounts() {
  static const QMap<QString, int> counts = {
      {QStringLiteral("AdjustmentTransferDialog"), 7},
      {QStringLiteral("AdjustmentTransferItemPane"), 12},
      {QStringLiteral("AdjustmentTransferNodePane"), 4},
      {QStringLiteral("AdjustmentTransferVersionPane"), 4},
      {QStringLiteral("CollectionsPanel"), 17},
      {QStringLiteral("EditorAdjustmentHeader"), 2},
      {QStringLiteral("EditorAdjustmentStack"), 9},
      {QStringLiteral("EditorDetailPanel"), 6},
      {QStringLiteral("EditorEndpointNodeDelegate"), 2},
      {QStringLiteral("EditorGeometryPanel"), 14},
      {QStringLiteral("EditorMaskGroupDelegate"), 16},
      {QStringLiteral("EditorMaskGroupMaskRow"), 8},
      {QStringLiteral("EditorMaskGroupsPanel"), 10},
      {QStringLiteral("EditorMasksContextPanel"), 26},
      {QStringLiteral("EditorNodeDelegate"), 3},
      {QStringLiteral("EditorNodeMaskDrawer"), 3},
      {QStringLiteral("EditorNodeMaskTypeRow"), 3},
      {QStringLiteral("EditorNodePortDelegate"), 2},
      {QStringLiteral("EditorNodesPanel"), 14},
      {QStringLiteral("EditorWhiteBalanceSection"), 6},
      {QStringLiteral("EditorWorkspaceRail"), 10},
      {QStringLiteral("InspectorToggleButton"), 4},
      {QStringLiteral("TopToolbar"), 8},
      {QStringLiteral("alcedo::ui::EditorNodeController"), 24},
  };
  return counts;
}

constexpr int kExpectedTargetMessageTotal = 214;

/// Parses a Qt TS catalog into context -> messages. Numerus translations keep
/// their combined <numerusform> text so placeholder checks stay meaningful.
auto          ParseTsCatalog(const QString& path, QString* error) -> Catalog {
  Catalog catalog;
  QFile   file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
    return catalog;
  }

  QXmlStreamReader xml(&file);
  QString          current_context;
  CatalogMessage   current;
  bool             in_message        = false;
  bool             in_translation    = false;
  bool             have_source       = false;
  int              translation_depth = 0;

  while (!xml.atEnd()) {
    const auto token = xml.readNext();
    if (token == QXmlStreamReader::StartElement) {
      const auto name = xml.name();
      if (name == QLatin1String("context")) {
        current_context.clear();
      } else if (name == QLatin1String("name") && current_context.isEmpty()) {
        current_context = xml.readElementText();
      } else if (name == QLatin1String("message")) {
        current     = CatalogMessage{};
        in_message  = true;
        have_source = false;
      } else if (in_message && name == QLatin1String("source")) {
        current.source = xml.readElementText();
        have_source    = true;
      } else if (in_message && name == QLatin1String("translation")) {
        in_translation    = true;
        translation_depth = 1;
        const auto attrs  = xml.attributes();
        current.unfinished =
            attrs.value(QLatin1String("type")).toString() == QLatin1String("unfinished");
      } else if (in_translation) {
        ++translation_depth;
      }
    } else if (token == QXmlStreamReader::Characters && in_translation && !xml.isWhitespace()) {
      current.translation += xml.text();
    } else if (token == QXmlStreamReader::EndElement) {
      const auto name = xml.name();
      if (in_translation) {
        --translation_depth;
        if (translation_depth == 0) {
          in_translation = false;
        }
      } else if (in_message && name == QLatin1String("message")) {
        if (have_source) {
          catalog[current_context].push_back(current);
        }
        in_message = false;
      }
    }
  }
  if (xml.hasError()) {
    *error = QStringLiteral("XML error in %1: %2").arg(path, xml.errorString());
    return {};
  }
  return catalog;
}

auto PlaceholderMultiset(const QString& text) -> QMap<QString, int> {
  static const QRegularExpression re(QStringLiteral("%\\d+|%n"));
  QMap<QString, int>              multiset;
  auto                            it = re.globalMatch(text);
  while (it.hasNext()) {
    const auto match = it.next().captured();
    multiset[match]  = multiset.value(match) + 1;
  }
  return multiset;
}

TEST(SimplifiedChineseCatalogTest, CatalogContainsTheAuditedTargetContextAndMessageCounts) {
  QString       error;
  const Catalog catalog = ParseTsCatalog(QStringLiteral(ALCEDO_ZH_CN_TS_FILE), &error);
  ASSERT_TRUE(error.isEmpty()) << error.toStdString();

  const auto& expected = ExpectedContextMessageCounts();
  for (auto it = expected.begin(); it != expected.end(); ++it) {
    ASSERT_TRUE(catalog.contains(it.key())) << "missing audited context " << it.key().toStdString();
    EXPECT_EQ(catalog.value(it.key()).size(), it.value()) << "context " << it.key().toStdString();
  }

  int total = 0;
  for (auto it = expected.begin(); it != expected.end(); ++it) {
    total += catalog.value(it.key()).size();
  }
  EXPECT_EQ(total, kExpectedTargetMessageTotal);
}

TEST(SimplifiedChineseCatalogTest, TargetContextsContainOnlyFinishedNonEmptyTranslations) {
  QString       error;
  const Catalog catalog = ParseTsCatalog(QStringLiteral(ALCEDO_ZH_CN_TS_FILE), &error);
  ASSERT_TRUE(error.isEmpty()) << error.toStdString();

  for (auto it = ExpectedContextMessageCounts().begin(); it != ExpectedContextMessageCounts().end();
       ++it) {
    ASSERT_TRUE(catalog.contains(it.key())) << it.key().toStdString();
    for (const auto& message : catalog.value(it.key())) {
      EXPECT_FALSE(message.unfinished)
          << it.key().toStdString() << " | " << message.source.toStdString();
      EXPECT_FALSE(message.translation.isEmpty())
          << it.key().toStdString() << " | " << message.source.toStdString();
    }
  }
}

TEST(SimplifiedChineseCatalogTest, TargetTranslationsPreserveEveryQtPlaceholder) {
  QString       error;
  const Catalog catalog = ParseTsCatalog(QStringLiteral(ALCEDO_ZH_CN_TS_FILE), &error);
  ASSERT_TRUE(error.isEmpty()) << error.toStdString();

  for (auto it = ExpectedContextMessageCounts().begin(); it != ExpectedContextMessageCounts().end();
       ++it) {
    ASSERT_TRUE(catalog.contains(it.key())) << it.key().toStdString();
    for (const auto& message : catalog.value(it.key())) {
      const auto source_ph      = PlaceholderMultiset(message.source);
      const auto translation_ph = PlaceholderMultiset(message.translation);
      EXPECT_EQ(source_ph, translation_ph)
          << it.key().toStdString() << " | " << message.source.toStdString() << " -> "
          << message.translation.toStdString();
    }
  }
}

TEST(SimplifiedChineseCatalogTest, MaskGroupTermsUseLayerWithMaskGroupParenthetical) {
  QString       error;
  const Catalog catalog = ParseTsCatalog(QStringLiteral(ALCEDO_ZH_CN_TS_FILE), &error);
  ASSERT_TRUE(error.isEmpty()) << error.toStdString();

  const QString                approved           = QStringLiteral("图层（蒙版组）");
  const QMap<QString, QString> required_compounds = {
      {QStringLiteral("Mask Group"), QStringLiteral("图层（蒙版组）")},
      {QStringLiteral("Mask Groups"), QStringLiteral("图层（蒙版组）")},
      {QStringLiteral("Add Mask Group"), QStringLiteral("添加图层（蒙版组）")},
      {QStringLiteral("Loading Mask Groups"), QStringLiteral("正在加载图层（蒙版组）")},
      {QStringLiteral("No Mask Groups"), QStringLiteral("暂无图层（蒙版组）")},
      {QStringLiteral("Open an image to edit Mask Groups"),
       QStringLiteral("打开图像以编辑图层（蒙版组）")},
      {QStringLiteral("Select an image to edit Mask Groups"),
       QStringLiteral("选择图像以编辑图层（蒙版组）")},
      {QStringLiteral("Finish the node graph before changing Mask Groups"),
       QStringLiteral("请先完成节点图，再更改图层（蒙版组）")},
      {QStringLiteral("Only a Color Grade Mask Group can be removed"),
       QStringLiteral("只能移除色彩分级图层（蒙版组）")},
      {QStringLiteral("Only a Color Grade Mask Group can be moved"),
       QStringLiteral("只能移动色彩分级图层（蒙版组）")},
      {QStringLiteral("That Mask Group is not in the committed node graph"),
       QStringLiteral("该图层（蒙版组）不在已提交的节点图中")},
      {QStringLiteral("The Mask Group move did not produce a valid node graph"),
       QStringLiteral("移动图层（蒙版组）后未生成有效的节点图")},
  };

  // Every audited compound keeps the exact approved noun phrase.
  for (auto it = required_compounds.begin(); it != required_compounds.end(); ++it) {
    bool found = false;
    for (auto ctx = catalog.begin(); ctx != catalog.end(); ++ctx) {
      for (const auto& message : ctx.value()) {
        if (message.source == it.key()) {
          found = true;
          EXPECT_EQ(message.translation, it.value())
              << ctx.key().toStdString() << " | " << it.key().toStdString();
        }
      }
    }
    EXPECT_TRUE(found) << "required Mask Group source missing: " << it.key().toStdString();
  }

  for (auto ctx = catalog.begin(); ctx != catalog.end(); ++ctx) {
    if (!ExpectedContextMessageCounts().contains(ctx.key())) {
      continue;
    }
    for (const auto& message : ctx.value()) {
      // Any source that names the Mask Group concept must carry the approved
      // phrase inside the Chinese text.
      if (message.source.contains(QStringLiteral("Mask Group"))) {
        EXPECT_TRUE(message.translation.contains(approved))
            << ctx.key().toStdString() << " | " << message.source.toStdString() << " -> "
            << message.translation.toStdString();
      }
      // Banned substitutes must not appear anywhere in target translations.
      const QString residue = QString(message.translation).replace(approved, QString());
      EXPECT_FALSE(residue.contains(QStringLiteral("蒙版组")))
          << ctx.key().toStdString() << " | " << message.translation.toStdString();
      EXPECT_FALSE(residue.contains(QStringLiteral("遮罩")))
          << ctx.key().toStdString() << " | " << message.translation.toStdString();
      EXPECT_FALSE(residue.contains(QStringLiteral("图层 (蒙版组)")))
          << ctx.key().toStdString() << " | " << message.translation.toStdString();
      EXPECT_FALSE(residue.contains(QStringLiteral("图层/")))
          << ctx.key().toStdString() << " | " << message.translation.toStdString();
      EXPECT_FALSE(residue.contains(QStringLiteral("图层")))
          << ctx.key().toStdString() << " | " << message.translation.toStdString();
    }
  }
}

TEST(SimplifiedChineseCatalogTest, CompiledCatalogTranslatesQmlAndCppContexts) {
  const QString qm_path = QStringLiteral(ALCEDO_ZH_CN_QM_FILE);
  ASSERT_TRUE(QFile::exists(qm_path)) << "compiled catalog missing: " << qm_path.toStdString();

  QTranslator translator;
  ASSERT_TRUE(translator.load(qm_path)) << "QTranslator failed to load " << qm_path.toStdString();
  ASSERT_FALSE(translator.isEmpty());
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

  // One QML context (EditorMaskGroupsPanel) and one C++ context
  // (alcedo::ui::EditorNodeController) resolve through the installed catalog.
  EXPECT_EQ(QCoreApplication::translate("EditorMaskGroupsPanel", "Mask Groups"),
            QStringLiteral("图层（蒙版组）"));
  EXPECT_EQ(QCoreApplication::translate("alcedo::ui::EditorNodeController",
                                        "Only a Color Grade Mask Group can be removed"),
            QStringLiteral("只能移除色彩分级图层（蒙版组）"));

  ASSERT_TRUE(QCoreApplication::removeTranslator(&translator));

  // Without the catalog the source English text returns.
  EXPECT_EQ(QCoreApplication::translate("EditorMaskGroupsPanel", "Mask Groups"),
            QStringLiteral("Mask Groups"));
}

}  // namespace
}  // namespace alcedo::ui::test
