#pragma once

#include "hal/IDisplay.h"
#include "os/AppSettings.h"
#include "ui/StoryMetadata.h"
#include <deque>
#include <vector>
#include <string>
#include <TextBlock.h>
#include <runner.h>

class InkStoryManager;

// Shared scrollback limits (wrapped lines, ~27 chars/line at 448px width).
// Single source of truth for InkEngine (turn-build trimming) and
// InkDisplayManager/InkInputHandler (trim-on-scroll). Keep in sync.
#if defined(BOARD_HAS_PSRAM)
static constexpr size_t kHistoryTargetLines = 300;
static constexpr size_t kHistoryHardMaxLines = 500;
static constexpr size_t kHistoryAbsoluteMaxLines = 700;
#elif defined(PLATFORM_NATIVE)
static constexpr size_t kHistoryTargetLines = 800;
static constexpr size_t kHistoryHardMaxLines = 1200;
static constexpr size_t kHistoryAbsoluteMaxLines = 1600;
#else
// No-PSRAM (X4/C3): target 100 ≈ 5 screens at 39px line height.
// Current turn is NEVER trimmed to target — it is preserved fully so the
// user always sees its beginning; only scrolled-past lines are evicted
// (trimScrolledAbove) as the user reads. Absolute max 300 is an OOM
// failsafe for buggy stories (longest real LW turn ≈ 154 lines + combat).
static constexpr size_t kHistoryTargetLines = 100;
static constexpr size_t kHistoryHardMaxLines = 150;
static constexpr size_t kHistoryAbsoluteMaxLines = 300;
#endif

struct WrappedLine {
    TextBlock block;
    bool isOld = false;
    bool isImage = false;
    std::string imagePath = "";
    int imageHeight = 0;
    bool endOfParagraph = false;
    int getHeight(class GfxRenderer* renderer) const;
};

class InkDisplayManager {
public:
    InkDisplayManager(IDisplay& display);
    ~InkDisplayManager();

    void applySettings(const AppSettings& settings);
    void resolveAndApplyFont(const StoryMetadata& meta, const char* storyBase, const char* storyDir);

    void clearHistory();
    void trimHistory(size_t targetLines, size_t hardMaxLines = 0);
    // Evict only old (previous-turn) lines down to targetLines.
    // Never touches current-turn lines — safe to call mid-turn while
    // new content is still being appended.
    void trimOldHistory(size_t targetLines);
    // Evict fully-scrolled-past lines (above viewport) while over target.
    // Called after scrolling down / revealing choices. Preserves unread
    // content: only lines the user has scrolled past disappear.
    void trimScrolledAbove();
    void addWrappedLine(const WrappedLine& line);
    void popOldestLine();
    size_t getHistorySize() const { return _wrappedLines.size(); }
    std::deque<WrappedLine>& getHistory() { return _wrappedLines; }
    void markHistoryOld();
    void clearFontCache();
    void unloadStreamingFonts();

    void collectChoices(ink::runtime::runner& runner);
    void clearChoices();
    int getChoicesHeight() const;

    int getScrollY() const { return _scrollY; }
    void setScrollY(int scrollY) { _scrollY = scrollY; }
    int getMaxScrollY() const { return _maxScrollY; }
    void doAutoScroll(int newLinesCount, bool showChoices);

    void setScrollToBottom();
    void scrollToSelectedChoice();

    bool isChoicesVisible() const;
    // True when any choice overlaps the viewport (stays true while paging
    // through a long choice list). Unlike isChoicesVisible() which is only
    // true at the very bottom (down-triangle indicator).
    bool isAnyChoiceVisible() const;
    bool isChoicesRevealed() const { return _choicesRevealed; }
    void setChoicesRevealed(bool revealed) { _choicesRevealed = revealed; }
    void revealChoices();
    void updateMaxScrollY();
    int getIndicatorHeight() const;

    void redraw(InkStoryManager& storyManager, AppSettings& settings, int& refreshCount);

    struct ChoiceLayout {
        int marginY;
        int choiceLineHeight;
        int choicePadding;
        int minChoiceHeight;
    };
    ChoiceLayout getChoiceLayout(class GfxRenderer* renderer) const;
    int getChoiceBlockHeight(const ChoiceLayout& l, int numLines) const;

    int getNumChoices() const { return _numChoices; }
    void setSelectedChoice(int index) { _selectedChoice = index; }
    int getSelectedChoice() const { return _selectedChoice; }
    void setBlinkingChoice(int index) { _blinkingChoice = index; }
    int getBlinkingChoice() const { return _blinkingChoice; }
    void flashChoiceActivation(InkStoryManager& storyManager, AppSettings& settings, int choiceIdx);
    
    // Y-coordinate mapping for touch interactions
    int getChoiceIndexAtY(int y) const;
    void setupStoryEndedChoices();
#ifdef PIO_UNIT_TESTING
    void setupTestChoices(const std::vector<std::string>& choices);
#endif

private:
    IDisplay& _display;
    AppSettings _settings = AppSettings::defaults();
    class StreamingEpdFontFamily* _streamingFamily = nullptr;
    
    std::deque<WrappedLine> _wrappedLines;
    
    std::vector<std::string> _choiceText;
    std::vector<std::vector<TextBlock>> _wrappedChoices;
    int   _numChoices       = 0;
    int   _selectedChoice   = 0;
    int   _blinkingChoice   = -1;

    int _scrollY = 0;
    int _maxScrollY = 0;

    bool _choicesRevealed = true;
};
