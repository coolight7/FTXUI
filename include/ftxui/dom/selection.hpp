// Copyright 2024 Arthur Sonzogni. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#ifndef FTXUI_DOM_SELECTION_HPP
#define FTXUI_DOM_SELECTION_HPP

#include <functional>

#include <sstream>
#include "ftxui/screen/box.hpp"   // for Box
#include "ftxui/screen/cell.hpp"  // for Cell
#include "ftxui/util/export.hpp"  // for FTXUI_EXPORT

namespace ftxui {

/// @brief Represents a selection in a terminal user interface.
///
/// Selection is a class that represents the two endpoints of a selection in a
/// terminal user interface.
///
/// @ingroup dom
class FTXUI_EXPORT(DOM) Selection {
 public:
  Selection();  // Empty selection.
  Selection(int start_x, int start_y, int end_x, int end_y);

  const Box& GetBox() const;

  Selection SaturateHorizontal(Box box);
  Selection SaturateVertical(Box box);
  bool IsEmpty() const { return empty_; }

  /// @brief The root selection (the one the user dragged).
  ///
  /// Containers pass a copy of the selection to their children after
  /// clamping the endpoints to their own box ([SaturateHorizontal] /
  /// [SaturateVertical]); those copies keep a pointer to the root.
  ///
  /// Nodes that emit text line by line (ftxui::Text / markdown::FlowText /
  /// markdown::FlowCodeBlock) read the endpoints and the bounding box from the
  /// root instead: a clamped endpoint that sits on a blank column of a row
  /// (the hanging indent of a wrapped list item, panel padding) is treated as
  /// "the whole row is selected" and copies text the user did not select, and
  /// a clamped box is narrowed, which drops the other nodes of that row.
  const Selection& Root() const { return *parent_; }

  /// @brief Range of columns selected on line `y` (text-flow semantics).
  ///
  /// - line of the start point (end point on another line): start column to
  ///   the end of the line (reversed selection ends at the start column)
  /// - line of the end point: beginning of the line to the end column
  ///   (reversed selection starts at the end column)
  /// - lines in between: the whole line
  ///
  /// Unbounded sides are reported as [kUnboundedMin] / [kUnboundedMax];
  /// intersect the range with the node's own columns to know which columns of
  /// the node take part in the selection (empty intersection means none).
  ///
  /// @param y The screen line.
  /// @param lo The first selected column of the line.
  /// @param hi The last selected column of the line.
  /// @return True when `y` is inside the selection; false otherwise (the
  ///         output parameters are left untouched).
  bool RowRange(int y, int& lo, int& hi) const;

  /// Bounds reported by [RowRange] for an unbounded side.
  static constexpr int kUnboundedMin = -(1 << 30);
  static constexpr int kUnboundedMax = (1 << 30);

  void AddPart(std::string_view part, int y, int left, int right);
  std::string GetParts() { return parts_.str(); }

 private:
  Selection(int start_x, int start_y, int end_x, int end_y, Selection* parent);

  const int start_x_ = 0;
  const int start_y_ = 0;
  const int end_x_ = 0;
  const int end_y_ = 0;
  const Box box_ = {};
  Selection* const parent_ = this;
  const bool empty_ = true;
  std::stringstream parts_;

  // The position of the last inserted part.
  int x_ = 0;
  int y_ = 0;
};

}  // namespace ftxui

#endif /* end of include guard: FTXUI_DOM_SELECTION_HPP */
