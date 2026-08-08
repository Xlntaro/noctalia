#pragma once

#include <functional>
#include <string>
#include <string_view>

class Flex;
class Label;
class ScrollView;
struct CalendarSnapshot;

namespace calendar_view {

  struct Date {
    int year = 0;
    int month = -1;
    int day = -1;

    [[nodiscard]] bool valid() const noexcept { return year != 0 && month >= 0 && day > 0; }
    bool operator==(const Date&) const = default;
  };

  struct State {
    Date current;
    int displayYear = 0;
    int displayMonth = 0;
    int displayWeekday = 0;
    bool isCurrentMonth = false;
  };

  struct MonthLayout {
    float width = 0.0f;
    float weekdayHeight = 0.0f;
    float dayCellHeight = 0.0f;
    float dayButtonSize = 0.0f;
    float gap = 0.0f;
    float dotDiameter = 0.0f;
    float dotGap = 0.0f;
    float weekColumnWidth = 0.0f;
    float weekLaneInset = 0.0f;
    float weekDividerWidth = 0.0f;
    float weekDaysGap = 0.0f;
  };

  struct MonthBuildOptions {
    Flex& grid;
    Label& monthLabel;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    int monthOffset = 0;
    bool showWeekNumbers = false;
    float scale = 1.0f;
    MonthLayout layout;
    std::string fontFamily;
    std::function<void(Date date, int monthShift)> onDateSelected;
  };

  struct EventListBuildOptions {
    ScrollView& scroll;
    bool reserveScrollbarGutter = false;
    Label* title = nullptr;
    const CalendarSnapshot* snapshot = nullptr;
    Date selected;
    float scale = 1.0f;
    std::string_view dateFormat = "%A %e %B";
    std::string_view timeFormat = "%H:%M";
    std::string fontFamily;
  };

  [[nodiscard]] State stateForOffset(int monthOffset);
  [[nodiscard]] int dateKey(Date date) noexcept;
  void rebuildMonth(const MonthBuildOptions& options);
  void rebuildEventList(const EventListBuildOptions& options);

} // namespace calendar_view
