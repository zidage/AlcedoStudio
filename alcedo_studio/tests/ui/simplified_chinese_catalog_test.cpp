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
      {QStringLiteral("AdjustmentTransferItemPane"), 14},
      {QStringLiteral("AdjustmentTransferNodePane"), 4},
      {QStringLiteral("AdjustmentTransferVersionPane"), 4},
      {QStringLiteral("CollectionsPanel"), 17},
      {QStringLiteral("EditorAdjustmentHeader"), 2},
      {QStringLiteral("EditorAdjustmentStack"), 11},
      {QStringLiteral("EditorEndpointNodeDelegate"), 2},
      {QStringLiteral("EditorGeometryPanel"), 14},
      {QStringLiteral("EditorMaskGroupDelegate"), 16},
      {QStringLiteral("EditorMaskGroupMaskRow"), 12},
      {QStringLiteral("EditorMaskGroupsPanel"), 10},
      {QStringLiteral("EditorMasksContextPanel"), 29},
      {QStringLiteral("EditorNodeDelegate"), 3},
      {QStringLiteral("EditorNodeMaskDrawer"), 3},
      {QStringLiteral("EditorNodeMaskTypeRow"), 3},
      {QStringLiteral("EditorNodePortDelegate"), 2},
      {QStringLiteral("EditorNodesPanel"), 14},
      {QStringLiteral("EditorPostProcessPanel"), 9},
      {QStringLiteral("EditorWhiteBalanceSection"), 5},
      {QStringLiteral("EditorWorkspaceRail"), 18},
      {QStringLiteral("InspectorToggleButton"), 4},
      {QStringLiteral("TopToolbar"), 8},
      {QStringLiteral("alcedo::ui::EditorNodeController"), 24},
  };
  return counts;
}

constexpr int kExpectedTargetMessageTotal = 235;

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

/// The rail label of the editor workspace is the short noun 图层 (commit e73e0227a): the rail
/// has no room for the parenthetical. Every other Mask Group text keeps the approved phrase.
auto IsShortMaskGroupRailLabel(const QString& context, const CatalogMessage& message) -> bool {
  return context == QLatin1String("EditorWorkspaceRail") &&
         message.source == QLatin1String("Mask Groups") &&
         message.translation == QStringLiteral("图层");
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
          if (IsShortMaskGroupRailLabel(ctx.key(), message)) {
            continue;
          }
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
      if (IsShortMaskGroupRailLabel(ctx.key(), message)) {
        continue;
      }
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

TEST(SimplifiedChineseCatalogTest, CompiledCatalogTranslatesTheSkyTrainingDataNotice) {
  const QString qm_path = QStringLiteral(ALCEDO_ZH_CN_QM_FILE);
  QTranslator   translator;
  ASSERT_TRUE(translator.load(qm_path)) << "QTranslator failed to load " << qm_path.toStdString();
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

  // SM3.E (#537): the notice of the AI Mask panel, in the wording of the AI Models plan
  // (section 4, decision A7): the user decides; there is no ban on commercial use.
  EXPECT_EQ(
      QCoreApplication::translate("EditorAiMaskPanel",
                                  "Sky: the license of the Sky model's training data is not "
                                  "clear. Decide yourself before commercial use; see "
                                  "Settings > AI Models."),
      QStringLiteral("天空：天空模型训练数据的许可证不明确，商业使用前请自行判断，详见“设置 > "
                     "AI 模型”。"));

  ASSERT_TRUE(QCoreApplication::removeTranslator(&translator));
}

TEST(SimplifiedChineseCatalogTest, CompiledCatalogTranslatesTheAiMaskModelSetStates) {
  const QString qm_path = QStringLiteral(ALCEDO_ZH_CN_QM_FILE);
  QTranslator   translator;
  ASSERT_TRUE(translator.load(qm_path)) << "QTranslator failed to load " << qm_path.toStdString();
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

  // AI Models plan, AM5: the AI Mask panel without a usable installed model set.
  EXPECT_EQ(QCoreApplication::translate("EditorAiMaskPanel",
                                        "The AI Mask models are not installed. Download them on "
                                        "the AI Models page in Settings."),
            QStringLiteral("AI 蒙版模型尚未安装。请在“设置 > AI 模型”页面下载。"));
  EXPECT_EQ(QCoreApplication::translate("EditorAiMaskPanel",
                                        "The installed AI Mask models are too old for this "
                                        "version of Alcedo Studio. Update them on the AI Models "
                                        "page in Settings."),
            QStringLiteral("已安装的 AI 蒙版模型版本过旧，此版本的 Alcedo Studio "
                           "无法使用。请在“设置 > AI 模型”页面更新。"));
  EXPECT_EQ(QCoreApplication::translate("EditorAiMaskPanel",
                                        "The AI Mask models need a newer version of Alcedo "
                                        "Studio. Update Alcedo Studio to use AI Mask."),
            QStringLiteral("AI 蒙版模型需要更新版本的 Alcedo Studio。请更新 Alcedo Studio "
                           "后使用 AI 蒙版。"));
  EXPECT_EQ(QCoreApplication::translate("EditorAiMaskPanel",
                                        "The installed AI Mask models are incomplete. Repair "
                                        "them on the AI Models page in Settings."),
            QStringLiteral("已安装的 AI 蒙版模型不完整。请在“设置 > AI 模型”页面修复。"));
  EXPECT_EQ(QCoreApplication::translate("EditorAiMaskPanel", "Open AI Models"),
            QStringLiteral("打开 AI 模型"));

  ASSERT_TRUE(QCoreApplication::removeTranslator(&translator));
}

TEST(SimplifiedChineseCatalogTest, CompiledCatalogTranslatesTheAiModelDownloadStatement) {
  const QString qm_path = QStringLiteral(ALCEDO_ZH_CN_QM_FILE);
  QTranslator   translator;
  ASSERT_TRUE(translator.load(qm_path)) << "QTranslator failed to load " << qm_path.toStdString();
  ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

  // AI Models plan, section 4: the download statement (version 1) and the Settings page.
  EXPECT_EQ(QCoreApplication::translate("AiModelDownloadStatementDialog", "Before you download"),
            QStringLiteral("下载前请阅读"));
  EXPECT_EQ(
      QCoreApplication::translate(
          "AiModelDownloadStatementDialog",
          "The Sky model and the people and sky locator are trained by Alcedo Studio. The Sky "
          "model is trained on the MaSS13K dataset (%1), which does not state a clear license. "
          "For commercial use of the Sky model or its output, contact the authors of the dataset. "
          "Alcedo Studio does not guarantee that the models it trains can be used for commercial "
          "work. The Subject model (S3OD) is trained on generated images and on labels from other "
          "datasets; see the NOTICE file of the package."),
      QStringLiteral(
          "天空模型与人物/天空定位模型由 Alcedo Studio 训练。天空模型使用 MaSS13K "
          "数据集（%1）训练，该数据集未明确声明许可证。如需将天空模型或其输出用于商业用途"
          "，请联系数据集作者。Alcedo Studio 不保证其训练的模型可用于商业活动。主体模型（S3OD"
          "）使用生成图像和来自其他数据集的标签训练，详见模型包内的 NOTICE 文件。"));
  EXPECT_EQ(QCoreApplication::translate(
                "AiModelDownloadStatementDialog",
                "Select \"Accept and Download\" to confirm that you have read this statement."),
            QStringLiteral("点击“同意并下载”即表示您已阅读本声明。"));
  EXPECT_EQ(QCoreApplication::translate("AiModelDownloadStatementDialog", "Accept and Download"),
            QStringLiteral("同意并下载"));
  EXPECT_EQ(QCoreApplication::translate("SettingDialog", "AI Models"), QStringLiteral("AI 模型"));
  // The cards say what the models do.
  EXPECT_EQ(QCoreApplication::translate(
                "AiModelsSettingsPanel",
                "SAM 2.1 tiny: selects the object that you click, box, or draw over."),
            QStringLiteral("SAM 2.1 tiny：选择你点击、框选或涂抹经过的对象。"));
  EXPECT_EQ(QCoreApplication::translate("AiModelsSettingsPanel",
                                        "Recognizes the content of photos, so you can find them "
                                        "with a text search in many languages."),
            QStringLiteral("识别照片内容，以便用多种语言的文本搜索照片。"));

  ASSERT_TRUE(QCoreApplication::removeTranslator(&translator));
}

}  // namespace
}  // namespace alcedo::ui::test
