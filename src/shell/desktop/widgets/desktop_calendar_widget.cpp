#include "shell/desktop/widgets/desktop_calendar_widget.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kCalendarWidth = 340.0f;
  constexpr float kEventsWidth = 240.0f;
  constexpr float kWidgetHeight = 390.0f;
  constexpr float kSectionGap = Style::spaceLg;
  constexpr float kGridGap = Style::spaceXs;
  constexpr float kHeaderHeight = Style::controlHeight;
  constexpr float kWeekdayHeight = 20.0f;
  constexpr float kDayCellHeight = 43.0f;
  constexpr float kDayButtonSize = 34.0f;
  constexpr float kDotDiameter = 4.0f;
  constexpr float kWeekColumnWidth = 24.0f;

  struct CalendarState {
    int currentYear = 0;
    int currentMonth = 0;
    int today = 0;
    int displayYear = 0;
    int displayMonth = 0;
    int displayWeekday = 0;
    bool isCurrentMonth = false;
  };

  CalendarState calendarState(int monthOffset) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);

    CalendarState state;
    state.currentYear = local.tm_year + 1900;
    state.currentMonth = local.tm_mon;
    state.today = local.tm_mday;

    const auto currentMonth =
        std::chrono::year{state.currentYear} / std::chrono::month{static_cast<unsigned>(state.currentMonth + 1)};
    const auto displayDate =
        std::chrono::year_month_day((currentMonth + std::chrono::months{monthOffset}) / std::chrono::day{1});
    state.displayYear = static_cast<int>(static_cast<std::int32_t>(displayDate.year()));
    state.displayMonth = static_cast<int>(static_cast<unsigned>(displayDate.month())) - 1;
    state.displayWeekday = static_cast<int>(std::chrono::weekday(std::chrono::sys_days{displayDate}).c_encoding());
    state.isCurrentMonth = state.displayYear == state.currentYear && state.displayMonth == state.currentMonth;
    return state;
  }

  int daysInMonth(int year, int month0) {
    const auto last =
        std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month0 + 1)} / std::chrono::last;
    return static_cast<int>(static_cast<unsigned>(last.day()));
  }

  std::string monthName(int month0) {
    if (month0 < 0 || month0 > 11) {
      return {};
    }
    std::tm tm{};
    tm.tm_mon = month0;
    tm.tm_mday = 1;
    return formatStrftime("%B", tm);
  }

  int dateKey(int year, int month0, int day) { return year * 10000 + (month0 + 1) * 100 + day; }

  int localDateKey(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&raw, &tm);
    return dateKey(tm.tm_year + 1900, tm.tm_mon, tm.tm_mday);
  }

  std::pair<int, int> eventDayRange(const CalendarEvent& event) {
    const int start = localDateKey(event.start);
    auto endTime = event.end;
    if (event.allDay && event.end > event.start) {
      endTime -= std::chrono::hours{24};
    }
    return {start, std::max(start, localDateKey(endTime))};
  }

  ColorSpec eventColor(const CalendarEvent& event) {
    Color color;
    if (!event.colorHex.empty() && tryParseHexColor(event.colorHex, color)) {
      return fixedColorSpec(color);
    }
    if (const auto role = colorRoleFromToken(event.colorHex); role.has_value()) {
      return colorSpecFromRole(*role);
    }
    return colorSpecFromRole(ColorRole::Primary);
  }

  void applyFontFamily(Node* node, const std::string& family) {
    if (node == nullptr) {
      return;
    }
    if (auto* label = dynamic_cast<Label*>(node); label != nullptr) {
      label->setFontFamily(family);
    }
    for (const auto& child : node->children()) {
      applyFontFamily(child.get(), family);
    }
  }

} // namespace

DesktopCalendarWidget::DesktopCalendarWidget(ConfigService* config, CalendarService* calendar, Options options)
    : m_config(config), m_calendar(calendar), m_showEvents(options.showEvents),
      m_showWeekNumbers(options.showWeekNumbers) {}

DesktopCalendarWidget::~DesktopCalendarWidget() {
  if (m_calendar != nullptr && m_calendarCallbackId != 0) {
    m_calendar->removeChangeCallback(m_calendarCallbackId);
  }
}

void DesktopCalendarWidget::create() {
  auto root = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = kSectionGap * contentScale(),
  });

  auto calendarArea = std::make_unique<InputArea>();
  m_calendarArea = calendarArea.get();
  calendarArea->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps != 0.0f) {
      changeMonthBy(steps > 0.0f ? 1 : -1);
    }
  });

  auto calendarColumn = ui::column({
      .out = &m_calendarColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * contentScale(),
  });
  calendarColumn->addChild(
      ui::label({
          .out = &m_todayLabel,
          .text = formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x"),
          .fontSize = Style::fontSizeTitle * contentScale(),
          .fontWeight = FontWeight::Medium,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) { focusToday(); });
          },
      })
  );

  auto header = ui::row({
      .out = &m_header,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * contentScale(),
  });
  header->addChild(
      ui::button({
          .out = &m_previousButton,
          .glyph = "chevron-left",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { changeMonthBy(-1); },
      })
  );
  header->addChild(
      ui::label({
          .out = &m_monthLabel,
          .fontSize = (Style::fontSizeTitle + Style::spaceXs) * contentScale(),
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
          .flexGrow = 1.0f,
          .configure = [this](Label& label) {
            label.setHitTestVisible(true);
            label.setOnClick([this](const InputArea::PointerData&) { focusToday(); });
          },
      })
  );
  header->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = "chevron-right",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { changeMonthBy(1); },
      })
  );
  calendarColumn->addChild(std::move(header));

  auto grid = ui::column({.out = &m_grid, .align = FlexAlign::Stretch, .gap = kGridGap * contentScale()});
  calendarColumn->addChild(std::move(grid));
  calendarArea->addChild(std::move(calendarColumn));
  root->addChild(std::move(calendarArea));

  auto eventsColumn = ui::column({
      .out = &m_eventsColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * contentScale(),
      .visible = m_showEvents,
  });
  eventsColumn->addChild(
      ui::label({
          .out = &m_eventsTitle,
          .text = i18n::tr("control-center.calendar.events"),
          .fontSize = Style::fontSizeTitle * contentScale(),
          .fontWeight = FontWeight::Bold,
          .fontFamily = m_fontFamily,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
      })
  );
  eventsColumn->addChild(
      ui::scrollView({
          .out = &m_eventsScroll,
          .fillWidth = true,
          .fillHeight = true,
          .flexGrow = 1.0f,
      })
  );
  root->addChild(std::move(eventsColumn));

  setRoot(std::move(root));
  focusToday();

  if (m_calendar != nullptr) {
    m_calendarCallbackId = m_calendar->addChangeCallback([this]() { markDirty(); });
  }
}

bool DesktopCalendarWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "show_events") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showEvents = *enabled;
      if (m_eventsColumn != nullptr) {
        m_eventsColumn->setVisible(m_showEvents);
      }
      m_dirty = true;
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "show_week_numbers") {
    if (const auto* enabled = std::get_if<bool>(&value)) {
      m_showWeekNumbers = *enabled;
      m_dirty = true;
      layout(renderer);
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopCalendarWidget::doLayout(Renderer& renderer) {
  if (m_rootLayout == nullptr || m_calendarArea == nullptr || m_calendarColumn == nullptr || m_grid == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float calendarWidth = (kCalendarWidth + (m_showWeekNumbers ? kWeekColumnWidth + kGridGap : 0.0f)) * scale;
  const float eventsWidth = kEventsWidth * scale;
  const float height = kWidgetHeight * scale;
  const float totalWidth = calendarWidth + (m_showEvents ? (kSectionGap * scale + eventsWidth) : 0.0f);

  m_rootLayout->setGap(kSectionGap * scale);
  m_rootLayout->setSize(totalWidth, height);
  m_calendarArea->setSize(calendarWidth, height);
  m_calendarColumn->setGap(Style::spaceSm * scale);
  m_calendarColumn->setSize(calendarWidth, height);
  if (m_header != nullptr) {
    m_header->setGap(Style::spaceSm * scale);
    m_header->setSize(calendarWidth, kHeaderHeight * scale);
  }
  for (Button* button : {m_previousButton, m_nextButton}) {
    if (button == nullptr) {
      continue;
    }
    button->setMinWidth(kHeaderHeight * scale);
    button->setMinHeight(kHeaderHeight * scale);
    button->setGlyphSize(Style::fontSizeBody * scale);
    button->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
    button->setRadius(Style::scaledRadiusMd(scale));
  }
  if (m_todayLabel != nullptr) {
    m_todayLabel->setFontSize(Style::fontSizeTitle * scale);
    m_todayLabel->setMaxWidth(calendarWidth);
  }
  if (m_monthLabel != nullptr) {
    m_monthLabel->setFontSize((Style::fontSizeTitle + Style::spaceXs) * scale);
    m_monthLabel->setMaxWidth(std::max(1.0f, calendarWidth - 2.0f * kHeaderHeight * scale));
  }
  if (m_eventsColumn != nullptr) {
    m_eventsColumn->setVisible(m_showEvents);
    m_eventsColumn->setGap(Style::spaceSm * scale);
    m_eventsColumn->setSize(eventsWidth, height);
  }
  if (m_eventsTitle != nullptr) {
    m_eventsTitle->setFontSize(Style::fontSizeTitle * scale);
    m_eventsTitle->setMaxWidth(eventsWidth);
  }
  if (m_eventsScroll != nullptr) {
    m_eventsScroll->setSize(eventsWidth, std::max(1.0f, height - 40.0f * scale));
    m_eventsScroll->setViewportPaddingH(Style::spaceXs * scale);
    m_eventsScroll->setViewportPaddingV(Style::spaceXs * scale);
  }

  rebuildCalendar();
  if (m_showEvents) {
    rebuildEventList();
  }
  applyFontFamily(root(), m_fontFamily);
  m_rootLayout->layout(renderer);
  m_dirty = false;
}

void DesktopCalendarWidget::doUpdate(Renderer& /*renderer*/) {
  const CalendarState state = calendarState(m_monthOffset);
  const int todayKey = dateKey(state.currentYear, state.currentMonth, state.today);
  if (todayKey != m_lastTodayKey) {
    m_lastTodayKey = todayKey;
    m_dirty = true;
  }
  if (m_todayLabel != nullptr) {
    m_todayLabel->setText(
        formatLocalTime(m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x")
    );
  }
  if (m_dirty && !isLayingOut()) {
    requestLayout();
  }
}

void DesktopCalendarWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  applyFontFamily(root(), family);
}

void DesktopCalendarWidget::changeMonthBy(int delta) {
  if (delta == 0) {
    return;
  }
  m_monthOffset += delta;
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::focusToday() {
  const CalendarState state = calendarState(0);
  m_monthOffset = 0;
  m_selectedYear = state.currentYear;
  m_selectedMonth = state.currentMonth;
  m_selectedDay = state.today;
  m_lastTodayKey = dateKey(state.currentYear, state.currentMonth, state.today);
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::markDirty() {
  m_dirty = true;
  requestLayout();
}

void DesktopCalendarWidget::rebuildCalendar() {
  uiAssertNotRendering("DesktopCalendarWidget::rebuildCalendar");
  if (m_grid == nullptr || m_monthLabel == nullptr) {
    return;
  }
  while (!m_grid->children().empty()) {
    m_grid->removeChild(m_grid->children().front().get());
  }

  const float scale = contentScale();
  const float gap = kGridGap * scale;
  const float calendarWidth = (kCalendarWidth + (m_showWeekNumbers ? kWeekColumnWidth + kGridGap : 0.0f)) * scale;
  const float weekWidth = m_showWeekNumbers ? kWeekColumnWidth * scale : 0.0f;
  const float dayGridWidth = calendarWidth - (m_showWeekNumbers ? weekWidth + gap : 0.0f);
  const float dayColumnWidth = std::max(1.0f, (dayGridWidth - 6.0f * gap) / 7.0f);
  const float buttonSize = std::min(kDayButtonSize * scale, dayColumnWidth);
  const float cellHeight = kDayCellHeight * scale;
  const float weekdayHeight = kWeekdayHeight * scale;
  const float dotDiameter = kDotDiameter * scale;

  const CalendarState state = calendarState(m_monthOffset);
  const int year = state.displayYear;
  const int month = state.displayMonth;
  m_monthLabel->setText(monthName(month) + " " + std::to_string(year));
  const bool focusedOnToday = m_monthOffset == 0
      && m_selectedYear == state.currentYear
      && m_selectedMonth == state.currentMonth
      && m_selectedDay == state.today;
  if (focusedOnToday) {
    m_monthLabel->clearTooltip();
  } else {
    m_monthLabel->setTooltip(i18n::tr("control-center.calendar.today"));
  }

  const int firstDayOfWeek = localeFirstDayOfWeek();
  std::array<std::string, 7> weekdays;
  for (int i = 0; i < 7; ++i) {
    std::tm tm{};
    tm.tm_wday = (firstDayOfWeek + i) % 7;
    tm.tm_mday = 1;
    weekdays[static_cast<std::size_t>(i)] = formatStrftime("%a", tm);
  }

  auto weekdayRow = std::make_unique<GridView>();
  weekdayRow->setColumns(7);
  weekdayRow->setColumnGap(gap);
  weekdayRow->setStretchItems(true);
  weekdayRow->setSize(dayGridWidth, weekdayHeight);
  weekdayRow->setMinCellHeight(weekdayHeight);
  for (std::size_t i = 0; i < weekdays.size(); ++i) {
    auto tile = std::make_unique<GridTile>();
    tile->setDirection(FlexDirection::Vertical);
    tile->setAlign(FlexAlign::Center);
    tile->setJustify(FlexJustify::Center);
    const int weekday = (firstDayOfWeek + static_cast<int>(i)) % 7;
    tile->addChild(
        ui::label({
            .text = weekdays[i],
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Medium,
            .fontFamily = m_fontFamily,
            .color =
                colorSpecFromRole(weekday == 0 || weekday == 6 ? ColorRole::Secondary : ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    weekdayRow->addChild(std::move(tile));
  }

  const int firstWeekdayOffset = (state.displayWeekday - firstDayOfWeek + 7) % 7;
  const int previousMonth = month == 0 ? 11 : month - 1;
  const int previousYear = month == 0 ? year - 1 : year;
  const int previousDays = daysInMonth(previousYear, previousMonth);
  const int monthDays = daysInMonth(year, month);
  const int nextMonth = month == 11 ? 0 : month + 1;
  const int nextYear = month == 11 ? year + 1 : year;

  std::array<std::vector<ColorSpec>, 32> eventDots;
  if (m_calendar != nullptr) {
    const int firstKey = dateKey(year, month, 1);
    const int lastKey = dateKey(year, month, monthDays);
    for (const CalendarEvent& event : m_calendar->snapshot().events) {
      const auto [eventStart, eventEnd] = eventDayRange(event);
      if (eventEnd < firstKey || eventStart > lastKey) {
        continue;
      }
      for (int day = 1; day <= monthDays; ++day) {
        const int key = dateKey(year, month, day);
        auto& dots = eventDots[static_cast<std::size_t>(day)];
        if (key >= eventStart && key <= eventEnd && dots.size() < 3) {
          dots.push_back(eventColor(event));
        }
      }
    }
  }

  auto dayGrid = std::make_unique<GridView>();
  dayGrid->setColumns(7);
  dayGrid->setColumnGap(gap);
  dayGrid->setStretchItems(true);
  dayGrid->setSize(dayGridWidth, 6.0f * cellHeight + 5.0f * gap);
  dayGrid->setMinCellHeight(cellHeight);

  int inMonthDay = 1;
  int trailingDay = 1;
  for (int index = 0; index < 42; ++index) {
    int cellYear = year;
    int cellMonth = month;
    int cellDay = 0;
    int monthShift = 0;
    bool inMonth = false;

    if (index < firstWeekdayOffset) {
      cellDay = previousDays - firstWeekdayOffset + index + 1;
      cellYear = previousYear;
      cellMonth = previousMonth;
      monthShift = -1;
    } else if (inMonthDay > monthDays) {
      cellDay = trailingDay++;
      cellYear = nextYear;
      cellMonth = nextMonth;
      monthShift = 1;
    } else {
      cellDay = inMonthDay++;
      inMonth = true;
    }

    auto tile = std::make_unique<GridTile>();
    tile->setDirection(FlexDirection::Vertical);
    tile->setAlign(FlexAlign::Center);
    tile->setJustify(FlexJustify::Center);
    tile->setGap(1.0f * scale);

    auto button = ui::button({
        .text = std::to_string(cellDay),
        .fontSize = Style::fontSizeBody * scale,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Ghost,
        .minWidth = buttonSize,
        .minHeight = buttonSize,
        .padding = 0.0f,
        .radius = Style::scaledRadiusMd(scale),
        .width = buttonSize,
        .height = buttonSize,
    });
    if (button->label() != nullptr) {
      button->label()->setFontFamily(m_fontFamily);
    }

    if (!inMonth) {
      Button::ButtonPalette muted = Button::defaultPalette(ButtonVariant::Ghost);
      muted.normal.label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75f);
      button->setCustomPalette(muted);
    } else {
      const bool selected = m_selectedYear == year && m_selectedMonth == month && m_selectedDay == cellDay;
      if (selected) {
        button->setVariant(ButtonVariant::Primary);
      } else {
        if (state.isCurrentMonth && cellDay == state.today) {
          Button::ButtonPalette today = Button::defaultPalette(ButtonVariant::Ghost);
          today.normal.label = colorSpecFromRole(ColorRole::Primary);
          button->setCustomPalette(today);
        }
        if (button->label() != nullptr) {
          button->label()->setFontWeight(FontWeight::Bold);
        }
      }
    }

    auto selectDay = [this, cellYear, cellMonth, cellDay, monthShift]() {
      m_selectedYear = cellYear;
      m_selectedMonth = cellMonth;
      m_selectedDay = cellDay;
      m_monthOffset += monthShift;
      m_dirty = true;
      requestLayout();
    };
    button->setOnClick(selectDay);
    tile->addChild(std::move(button));

    auto dots = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 2.0f * scale});
    dots->setSize(buttonSize, dotDiameter);
    if (inMonth) {
      for (const ColorSpec& color : eventDots[static_cast<std::size_t>(cellDay)]) {
        dots->addChild(
            ui::box({
                .fill = color,
                .radius = dotDiameter * 0.5f,
                .width = dotDiameter,
                .height = dotDiameter,
            })
        );
      }
    }
    auto dotArea = std::make_unique<InputArea>();
    dotArea->setSize(buttonSize, dotDiameter);
    dotArea->setOnClick([selectDay](const InputArea::PointerData&) { selectDay(); });
    dotArea->addChild(std::move(dots));
    tile->addChild(std::move(dotArea));
    dayGrid->addChild(std::move(tile));
  }

  const float gridHeight = weekdayHeight + gap + 6.0f * cellHeight + 5.0f * gap;
  auto days = ui::column({.gap = gap});
  days->setSize(dayGridWidth, gridHeight);
  days->addChild(std::move(weekdayRow));
  days->addChild(std::move(dayGrid));

  if (m_showWeekNumbers) {
    auto weekColumn = ui::column({.align = FlexAlign::Center, .gap = gap});
    auto weekdaySpacer = ui::column({});
    weekdaySpacer->setSize(weekWidth, weekdayHeight);
    weekColumn->addChild(std::move(weekdaySpacer));

    const int thursdayColumn = (4 - firstDayOfWeek + 7) % 7;
    const auto firstThursday =
        std::chrono::sys_days(std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month + 1)} / 1)
        - std::chrono::days{firstWeekdayOffset}
        + std::chrono::days{thursdayColumn};
    for (int row = 0; row < 6; ++row) {
      auto cell = ui::column({.align = FlexAlign::Center, .justify = FlexJustify::Center});
      cell->setSize(weekWidth, cellHeight);
      cell->addChild(
          ui::label({
              .text = std::format("{:%V}", firstThursday + std::chrono::days{row * 7}),
              .fontSize = Style::fontSizeCaption * scale,
              .fontFamily = m_fontFamily,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7f),
              .maxLines = 1,
          })
      );
      weekColumn->addChild(std::move(cell));
    }
    weekColumn->setSize(weekWidth, gridHeight);

    auto row = ui::row({.gap = gap});
    row->setSize(calendarWidth, gridHeight);
    row->addChild(std::move(weekColumn));
    row->addChild(std::move(days));
    m_grid->addChild(std::move(row));
  } else {
    m_grid->addChild(std::move(days));
  }
  m_grid->setSize(calendarWidth, gridHeight);
}

void DesktopCalendarWidget::rebuildEventList() {
  if (m_eventsScroll == nullptr || m_eventsScroll->content() == nullptr) {
    return;
  }
  const float scale = contentScale();
  Flex* content = m_eventsScroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceSm * scale);
  while (!content->children().empty()) {
    content->removeChild(content->children().front().get());
  }

  std::tm selected{};
  selected.tm_year = m_selectedYear - 1900;
  selected.tm_mon = m_selectedMonth;
  selected.tm_mday = m_selectedDay;
  selected.tm_isdst = -1;
  const std::time_t selectedRaw = std::mktime(&selected);
  if (m_eventsTitle != nullptr) {
    const char* format =
        m_config != nullptr ? m_config->config().controlCenter.calendarTab.eventDateFormat.c_str() : "%A %e %B";
    m_eventsTitle->setText(formatLocalUnixTime(static_cast<std::int64_t>(selectedRaw), format));
  }

  std::vector<const CalendarEvent*> events;
  const int selectedKey = dateKey(m_selectedYear, m_selectedMonth, m_selectedDay);
  if (m_calendar != nullptr) {
    for (const CalendarEvent& event : m_calendar->snapshot().events) {
      const auto [start, end] = eventDayRange(event);
      if (selectedKey >= start && selectedKey <= end) {
        events.push_back(&event);
      }
    }
  }

  if (events.empty()) {
    content->addChild(
        ui::label({
            .text = i18n::tr("control-center.calendar.no-events"),
            .fontSize = Style::fontSizeBody * scale,
            .fontFamily = m_fontFamily,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    return;
  }

  const float dotWidth = Style::spaceXs * scale;
  const float rowGap = Style::spaceSm * scale;
  const float textWidth = std::max(40.0f, (kEventsWidth - Style::spaceXs * 2.0f) * scale - dotWidth - rowGap);
  for (const CalendarEvent* event : events) {
    std::string timeText;
    if (event->allDay) {
      timeText = i18n::tr("control-center.calendar.all-day");
    } else {
      const std::time_t raw = std::chrono::system_clock::to_time_t(event->start);
      const char* format =
          m_config != nullptr ? m_config->config().controlCenter.calendarTab.eventTimeFormat.c_str() : "%H:%M";
      timeText = formatLocalUnixTime(static_cast<std::int64_t>(raw), format);
    }

    auto details = ui::column(
        {.align = FlexAlign::Start, .gap = Style::spaceXs * 0.5f * scale, .flexGrow = 1.0f},
        ui::label({
            .text = event->title.empty() ? i18n::tr("control-center.calendar.events") : event->title,
            .fontSize = Style::fontSizeBody * scale,
            .fontFamily = m_fontFamily,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxWidth = textWidth,
            .maxLines = 3,
        }),
        ui::label({
            .text = timeText,
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = m_fontFamily,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = textWidth,
            .maxLines = 1,
        })
    );
    content->addChild(
        ui::row(
            {.align = FlexAlign::Stretch, .gap = rowGap},
            ui::box({
                .fill = eventColor(*event),
                .radius = dotWidth * 0.5f,
                .width = dotWidth,
                .flexGrow = 0.0f,
            }),
            std::move(details)
        )
    );
  }
}
