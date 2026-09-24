// Copyright 2020 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.
#include <algorithm>  // for min, max
#include <cstddef>
#include <cstdint>
#include <memory>       // for make_shared
#include <string>       // for string, wstring
#include <string_view>  // for string_view
#include <utility>      // for move
#include <vector>       // for vector

#include "ftxui/dom/deprecated.hpp"   // for text, vtext
#include "ftxui/dom/elements.hpp"     // for Element, text, vtext
#include "ftxui/dom/node.hpp"         // for Node
#include "ftxui/dom/requirement.hpp"  // for Requirement
#include "ftxui/dom/selection.hpp"    // for Selection
#include "ftxui/screen/box.hpp"       // for Box
#include "ftxui/screen/screen.hpp"    // for Cell, Screen
#include "ftxui/screen/string.hpp"  // for string_width, Utf8ToGlyphs, to_string
#include "ftxui/screen/string_internal.hpp"  // for EatCodePoint, IsCombining, IsControl, IsFullWidth

namespace ftxui {

namespace {
using ftxui::Screen;

/// A text node storing the text itself instead of one string per glyph.
///
/// The render tree lives as long as the element is cached, so its size matters:
/// a glyph per std::string costs ~30-60x the displayed text, and a message list
/// builds thousands of such nodes. Here the node keeps:
/// - the plain text (one std::string),
/// - the byte offset where every line starts (empty for single line texts).
///
/// Everything else is recomputed from those two fields when the node is
/// rendered or selected. The line width is measured once in the constructor, so
/// ComputeRequirement() only resets the selection state.
class Text : public Node {
 public:
  explicit Text(std::string_view text) : text_(text) {
    // Measured once: the widest line and the number of lines.
    int    max_columns = 0;
    int    line_columns = 0;
    size_t pos = 0;
    while (pos < text_.size()) {
      size_t   glyph_end = 0;
      uint32_t codepoint = 0;
      if (!EatCodePoint(text_, pos, &glyph_end, &codepoint)) {
        pos = glyph_end;
        continue;
      }
      pos = glyph_end;
      if (codepoint == '\n') {
        max_columns = std::max(max_columns, line_columns);
        line_columns = 0;
        // Start of the line that follows. Kept empty while the text has a
        // single line, so short labels do not allocate.
        if (line_starts_.empty()) {
          line_starts_.push_back(0);
        }
        line_starts_.push_back(static_cast<int>(pos));
        continue;
      }
      line_columns += Columns(codepoint);
    }
    max_columns = std::max(max_columns, line_columns);

    requirement_.min_x = max_columns;
    requirement_.min_y = lineCount();
  }

  void ComputeRequirement() override {
    // The requirement was computed once in the constructor. This hook still
    // runs before every frame; use it to clear the selection, which Select()
    // re-populates while a selection is active.
    selection_rows_.clear();
  }

  void Select(Selection& selection) override {
    const Box selection_box = Box::Intersection(selection.GetBox(), box_);
    if (selection_box.IsEmpty()) {
      return;
    }

    // Only store the selected line range. Sizing per line would allocate one
    // entry per line of the whole text on every frame.
    const size_t lines_count = lineCount();
    const size_t first = selection_box.y_min - box_.y_min;
    const size_t last =
        std::min<size_t>(selection_box.y_max - box_.y_min + 1, lines_count);
    if (first >= last) {
      return;
    }
    selection_first_line_ = first;
    selection_rows_.assign(last - first, {-1, -1});

    for (size_t i = first; i < last; ++i) {
      const int y = box_.y_min + (int)i;
      const Box row_box{box_.x_min, box_.x_max, y, y};
      const Selection row_sel = selection.SaturateHorizontal(row_box);
      const int sel_start = row_sel.GetBox().x_min;
      const int sel_end = row_sel.GetBox().x_max;
      selection_rows_[i - first] = {sel_start, sel_end};

      selection.AddPart(lineText(i, sel_start, sel_end), y, sel_start, sel_end);
    }
  }

  void Render(Screen& screen) override {
    const auto visible_box = Box::Intersection(screen.stencil, box_);
    if (visible_box.IsEmpty()) {
      return;
    }

    int y = visible_box.y_min;

    const size_t first_line = visible_box.y_min - box_.y_min;
    const size_t last_line =
        std::min<size_t>(visible_box.y_max - box_.y_min + 1, lineCount());

    for (size_t line = first_line; line < last_line; ++line, ++y) {
      int sel_start = -1;
      int sel_end = -1;
      const size_t sel_index = line - selection_first_line_;
      if (sel_index < selection_rows_.size()) {
        sel_start = selection_rows_[sel_index].first;
        sel_end = selection_rows_[sel_index].second;
      }
      renderLine(screen, line, y, sel_start, sel_end);
    }
  }

 private:
  /// Columns used by one codepoint. Control characters are dropped, combining
  /// characters join the previous cell.
  static int Columns(uint32_t codepoint) {
    if (IsControl(codepoint) || IsCombining(codepoint)) {
      return 0;
    }
    return IsFullWidth(codepoint) ? 2 : 1;
  }

  size_t lineCount() const {
    return line_starts_.empty() ? 1 : line_starts_.size();
  }

  /// Byte offset where the line starts.
  size_t lineBegin(size_t line) const {
    return line_starts_.empty() ? 0 : static_cast<size_t>(line_starts_[line]);
  }

  /// Byte offset where the line content ends (the '\n' itself is excluded).
  size_t lineEnd(size_t line) const {
    if (line_starts_.empty()) {
      return text_.size();
    }
    return (line + 1 < line_starts_.size())
               ? static_cast<size_t>(line_starts_[line + 1]) - 1
               : text_.size();
  }

  /// Text of the columns [sel_start, sel_end] of one line. A fullwidth
  /// character owns the column where it is drawn; the column it reserves holds
  /// no text of its own.
  std::string lineText(size_t line, int sel_start, int sel_end) const {
    std::string  part;
    int          x = box_.x_min;
    int          last_column = -1;  // column of the previous glyph
    const size_t begin = lineBegin(line);
    const size_t end = lineEnd(line);
    size_t       pos = begin;
    while (pos < end) {
      size_t       glyph_begin = pos;
      size_t       glyph_end = 0;
      uint32_t     codepoint = 0;
      if (!EatCodePoint(text_, pos, &glyph_end, &codepoint)) {
        pos = glyph_end;
        continue;
      }
      pos = glyph_end;
      const int columns = Columns(codepoint);
      if (columns == 0) {
        // Combining characters belong to the previous glyph (control
        // characters have no glyph at all).
        if (IsCombining(codepoint) && last_column >= 0 && sel_start <= last_column
            && last_column <= sel_end) {
          part.append(text_, glyph_begin, pos - glyph_begin);
        }
        continue;
      }
      if (sel_start <= x && x <= sel_end) {
        part.append(text_, glyph_begin, pos - glyph_begin);
      }
      last_column = x;
      x += columns;
    }
    return part;
  }

  /// Draw one line of text at the screen row [y].
  void renderLine(
      Screen& screen,
      size_t  line,
      int     y,
      int     sel_start,
      int     sel_end
  ) const {
    int          x = box_.x_min;
    int          last_column = -1;  // cell the next combining character joins
    const size_t begin = lineBegin(line);
    const size_t end = lineEnd(line);
    size_t       pos = begin;
    while (pos < end) {
      size_t   glyph_begin = pos;
      size_t   glyph_end = 0;
      uint32_t codepoint = 0;
      if (!EatCodePoint(text_, pos, &glyph_end, &codepoint)) {
        pos = glyph_end;
        continue;
      }
      pos = glyph_end;
      if (IsControl(codepoint)) {
        continue;  // Control characters are not drawn and use no column.
      }
      if (IsCombining(codepoint)) {
        // Combining characters are merged into the cell they modify.
        if (last_column >= 0) {
          screen.CellAt(last_column, y)
              .character.append(text_, glyph_begin, pos - glyph_begin);
        }
        continue;
      }
      if (x > box_.x_max) {
        break;
      }
      const bool full_width = IsFullWidth(codepoint);
      writeCell(
          screen,
          x,
          y,
          sel_start,
          sel_end,
          text_.data() + glyph_begin,
          pos - glyph_begin
      );
      if (full_width) {
        // Fullwidth characters take two cells; the second is only reserved.
        writeCell(screen, x + 1, y, sel_start, sel_end, nullptr, 0);
        last_column = x + 1;
        x += 2;
      } else {
        last_column = x;
        x += 1;
      }
    }
  }

  void writeCell(
      Screen&     screen,
      int         x,
      int         y,
      int         sel_start,
      int         sel_end,
      const char* data,
      size_t      size
  ) const {
    auto& cell = screen.CellAt(x, y);
    cell.character.assign(data == nullptr ? "" : data, data == nullptr ? 0 : size);
    if (sel_start >= 0 && x >= sel_start && x <= sel_end) {
      screen.GetSelectionStyle()(cell);
    }
  }

  std::string text_;
  /// Byte offset of every line start; empty while the text has a single line.
  std::vector<int> line_starts_;
  // Selection state for the line range [selection_first_line_,
  // selection_first_line_ + selection_rows_.size()).
  size_t selection_first_line_ = 0;
  std::vector<std::pair<int, int>> selection_rows_;
};

class VText : public Node {
 public:
  explicit VText(std::string_view text) : glyphs_(Utf8ToGlyphs(text)) {
    for (const auto& g : glyphs_) {
      if (g != "\n") {
        width_ = 1;
        break;
      }
    }
  }

  void ComputeRequirement() override {
    int max_height = 0;
    int current_height = 0;
    int columns = 1;

    for (const auto& cell : glyphs_) {
      if (cell == "\n") {
        max_height = std::max(max_height, current_height);
        current_height = 0;
        columns++;
      } else {
        current_height++;
      }
    }
    max_height = std::max(max_height, current_height);

    requirement_.min_x = width_ * columns;
    requirement_.min_y = max_height;
  }

  void Render(Screen& screen) override {
    int x = box_.x_min;
    int y = box_.y_min;
    if (x + width_ - 1 > box_.x_max) {
      return;
    }
    for (const auto& it : glyphs_) {
      if (it == "\n") {
        x += width_;
        y = box_.y_min;
        if (x + width_ - 1 > box_.x_max) {
          return;
        }
        continue;
      }
      if (y > box_.y_max) {
        continue;
      }
      screen.CellAt(x, y).character = it;
      y += 1;
    }
  }

 private:
  std::vector<std::string> glyphs_;
  int width_ = 0;
};

}  // namespace

/// @brief Display a piece of UTF8 encoded unicode text.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = text("Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// Hello world!
/// ```
Element text(std::string_view text) {
  return std::make_shared<Text>(text);
}

/// @brief Display a piece of unicode text.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = text(L"Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// Hello world!
/// ```
Element text(std::wstring_view text) {
  return ftxui::text(to_string(text));
}

/// @brief Display a piece of unicode text vertically.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = vtext("Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// H
/// e
/// l
/// l
/// o
///
/// w
/// o
/// r
/// l
/// d
/// !
/// ```
Element vtext(std::string_view text) {
  return std::make_shared<VText>(text);
}

/// @brief Display a piece unicode text vertically.
/// @ingroup dom
/// @see ftxui::to_wstring
///
/// ### Example
///
/// ```cpp
/// Element document = vtext(L"Hello world!");
/// ```
///
/// ### Output
///
/// ```bash
/// H
/// e
/// l
/// l
/// o
///
/// w
/// o
/// r
/// l
/// d
/// !
/// ```
Element vtext(std::wstring_view text) {  // NOLINT
  return vtext(to_string(text));
}

}  // namespace ftxui
