#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <sstream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long formattingToggleMs = 500;
// New constant for double click speed
constexpr unsigned long doubleClickMs = 300;

// Global state for the Help Overlay and Night Mode
static bool showHelpOverlay = false;
static bool isNightMode = false;

constexpr int statusBarMargin = 19;
constexpr int progressBarMarginTop = 1;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

// Enum for cleaner alignment logic
enum class BoxAlign { LEFT, RIGHT, CENTER };

// Helper to draw multi-line text cleanly
void drawHelpBox(const GfxRenderer& renderer, int x, int y, const char* text, BoxAlign align, int32_t fontId,
                 int lineHeight) {
  // Split text into lines
  std::vector<std::string> lines;
  std::stringstream ss(text);
  std::string line;
  int maxWidth = 0;

  while (std::getline(ss, line, '\n')) {
    lines.push_back(line);
    int w = renderer.getTextWidth(fontId, line.c_str());
    if (w > maxWidth) maxWidth = w;
  }

  // Padding
  int padding = 16;
  int boxWidth = maxWidth + padding;
  int boxHeight = (lines.size() * lineHeight) + padding;

  int drawX = x;
  if (align == BoxAlign::RIGHT) {
    drawX = x - boxWidth;
  } else if (align == BoxAlign::CENTER) {
    drawX = x - (boxWidth / 2);
  }

  // Ensure we don't draw off the bottom edge
  if (y + boxHeight > renderer.getScreenHeight()) {
    y = renderer.getScreenHeight() - boxHeight - 5;
  }

  // Fill White (Clear background)
  renderer.fillRect(drawX, y, boxWidth, boxHeight, false);
  // Draw Border Black (Thickness: 4 for Bold)
  renderer.drawRect(drawX, y, boxWidth, boxHeight, 4, true);

  // Draw each line
  for (size_t i = 0; i < lines.size(); i++) {
    // ALWAYS center text horizontally within the box
    int lineWidth = renderer.getTextWidth(fontId, lines[i].c_str());
    int lineX_centered = drawX + (boxWidth - lineWidth) / 2;

    renderer.drawText(fontId, lineX_centered, y + (padding / 2) + (i * lineHeight), lines[i].c_str());
  }
}

}  // namespace

void EpubReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Reset help overlay state when entering a book
  showHelpOverlay = false;
  // Reset Night Mode on entry
  isNightMode = false;

  if (!epub) {
    return;
  }

  applyReaderOrientation(renderer, SETTINGS.orientation);

  renderingMutex = xSemaphoreCreateMutex();

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      Serial.printf("[%lu] [ERS] Loaded cache: %d, %d\n", millis(), currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      Serial.printf("[%lu] [ERS] Opened for first time, navigating to text reference at index %d\n", millis(),
                    textSpineIndex);
    }
  }

  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  updateRequired = true;

  xTaskCreate(&EpubReaderActivity::taskTrampoline, "EpubReaderActivityTask", 8192, this, 1, &displayTaskHandle);
}

void EpubReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();
}

void EpubReaderActivity::loop() {
  // --- POPUP AUTO-DISMISS ---
  static unsigned long clearPopupTimer = 0;
  if (clearPopupTimer > 0 && millis() > clearPopupTimer) {
    clearPopupTimer = 0;
    updateRequired = true;
  }

  // --- HELP OVERLAY INTERCEPTION ---
if (showHelpOverlay) {
    const int w = renderer.getScreenWidth();
    const int h = renderer.getScreenHeight();

    int32_t overlayFontId = SMALL_FONT_ID;
    int overlayLineHeight = 18;

    int dismissY = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) ? 500 : 300;
    int dismissX = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) ? w / 2 : w / 2 + 25;

    drawHelpBox(renderer, dismissX, dismissY, "PRESS ANY KEY
TO DISMISS", BoxAlign::CENTER, overlayFontId,
                overlayLineHeight);

    if (SETTINGS.orientation == CrossPointSettings::ORIENTATION::PORTRAIT) {
      if (SETTINGS.swapPortraitControls == 1) {
        drawHelpBox(renderer, 10, h - 80, "2x: Dark", BoxAlign::LEFT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h / 2 - 70, "1x: Text size –
Hold: Spacing
2x: Alignment", BoxAlign::RIGHT,
                    overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h / 2 + 10, "1x: Text size +
Hold: Rotate
2x: Bold", BoxAlign::RIGHT,
                    overlayFontId, overlayLineHeight);
      } else {
        drawHelpBox(renderer, 10, h - 80, "2x: Dark", BoxAlign::LEFT, overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 145, h - 80, "1x: Text size –
Hold: Spacing
2x: Alignment", BoxAlign::RIGHT,
                    overlayFontId, overlayLineHeight);
        drawHelpBox(renderer, w - 10, h - 80, "1x: Text size +
Hold: Rotate
2x: Bold", BoxAlign::RIGHT, overlayFontId,
                    overlayLineHeight);
      }
    } else {
      drawHelpBox(renderer, w - 10, h - 40, "2x: Dark", BoxAlign::RIGHT, overlayFontId, overlayLineHeight);
      drawHelpBox(renderer, w / 2 + 20, 20, "1x: Text size –
Hold: Spacing
2x: Alignment", BoxAlign::RIGHT,
                  overlayFontId, overlayLineHeight);
      drawHelpBox(renderer, w / 2 + 30, 20, "1x: Text size +
Hold: Rotate
2x: Bold", BoxAlign::LEFT, overlayFontId,
                  overlayLineHeight);
    }
  }

  // --- STANDARD REFRESH ---
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  renderer.storeBwBuffer();

  if (SETTINGS.textAntiAliasing && !showHelpOverlay && !isNightMode) {  // Don't anti-alias the help overlay
    renderer.clearScreen(0x00);

    // TURN ON BOLD FOR GRAYSCALE PASSES
    EpdFontFamily::globalForceBold = useBold;

    // --- LSB (Light Grays) Pass ---
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);

    // --- MSB (Dark Grays) Pass ---
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);
    renderer.copyGrayscaleMsbBuffers();

    // TURN BOLD OFF BEFORE FINAL FLUSH
    EpdFontFamily::globalForceBold = false;

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  renderer.restoreBwBuffer();
}

void EpubReaderActivity::renderStatusBar(const int orientedMarginRight, const int orientedMarginBottom,
                                         const int orientedMarginLeft) const {
  auto metrics = UITheme::getInstance().getMetrics();

  const bool showProgressPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL;
  const bool showBookProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                   SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR;
  const bool showChapterProgressBar = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showProgressText = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR;
  const bool showBookPercentage = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBattery = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                           SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showChapterTitle = SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::FULL ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR ||
                                SETTINGS.statusBar == CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR;
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  const auto screenHeight = renderer.getScreenHeight();
  const auto textY = screenHeight - orientedMarginBottom - 4;
  int progressTextWidth = 0;

  const float sectionChapterProg = static_cast<float>(section->currentPage) / section->pageCount;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  if (showProgressText || showProgressPercentage || showBookPercentage) {
    char progressStr[32];

    if (showProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", section->currentPage + 1, section->pageCount,
               bookProgress);
    } else if (showBookPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", section->currentPage + 1, section->pageCount);
    }

    progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - orientedMarginRight - progressTextWidth, textY,
                      progressStr);
  }

  if (showBookProgressBar) {
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(bookProgress));
  }

  if (showChapterProgressBar) {
    const float chapterProgress =
        (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100 : 0;
    GUI.drawReadingProgressBar(renderer, static_cast<size_t>(chapterProgress));
  }

  if (showBattery) {
    GUI.drawBattery(renderer, Rect{orientedMarginLeft + 1, textY, metrics.batteryWidth, metrics.batteryHeight},
                    showBatteryPercentage);
  }

  if (showChapterTitle) {
    const int rendererableScreenWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;

    const int batterySize = showBattery ? (showBatteryPercentage ? 50 : 20) : 0;
    const int titleMarginLeft = batterySize + 30;
    const int titleMarginRight = progressTextWidth + 30;

    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

    std::string title;
    int titleWidth;
    if (tocIndex == -1) {
      title = "Unnamed";
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, "Unnamed");
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      if (titleWidth > availableTitleSpace) {
        availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
        titleMarginLeftAdjusted = titleMarginLeft;
      }
      if (titleWidth > availableTitleSpace) {
        title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
        titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
      }
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + orientedMarginLeft + (availableTitleSpace - titleWidth) / 2, textY,
                      title.c_str());
  }
}
