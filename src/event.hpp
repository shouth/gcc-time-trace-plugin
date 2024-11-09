// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 Shota Minami

#pragma once

#include <algorithm>
#include <chrono>
#include <forward_list>
#include <string>
#include <unordered_map>
#include <utility>

#include <gcc-plugin.h>

enum class UnitEventKind
{
  Start,
  End,
};

struct UnitEvent
{
  UnitEventKind kind;
};

enum class IncludeEventKind
{
  Enter,
  Leave,
};

struct IncludeEvent
{
  IncludeEventKind kind;
  std::string filename;
};

enum class ParseEventKind
{
  Start,
  PreGenericize,
  Finish,
};

struct ParseEvent
{
  ParseEventKind kind;
  tree decl;
  unsigned int uid;
};

enum class PassEventKind
{
  Start,
  End,
};

struct PassEvent
{
  PassEventKind kind;
  std::string name;
  tree decl;
  unsigned int uid;
};

using EventClock = std::chrono::steady_clock;
using EventDuration = EventClock::duration;
using EventTimePoint = EventClock::time_point;

template <typename Event>
struct EventRecord
{
  EventTimePoint timestamp;
  Event event;

  EventRecord(Event event)
    : event(std::move(event))
    , timestamp(EventClock::now())
  {
  }
};

template <typename Callback>
class EventTracker
{
  template <typename E>
  struct DefaultMatchCallback
  {
    Callback &cb;
    EventRecord<E> &end;

    auto operator()(EventRecord<E> *start) -> void
    {
      if (start) {
        cb.on_match(std::move(*start), std::move(end));
      } else {
        cb.on_mismatch(std::move(end));
      }
    }
  };

  template <typename E>
  using Stack = std::forward_list<EventRecord<E>>;

  template <typename K, typename E>
  using StackMap = std::unordered_map<K, Stack<E>>;

  Stack<UnitEvent> _unit_events;
  Stack<IncludeEvent> _include_events;
  StackMap<unsigned int, ParseEvent> _parse_events;
  StackMap<unsigned int, ParseEvent> _genericize_events;
  StackMap<std::string, PassEvent> _pass_events;

  Callback &_cb;

public:
  EventTracker(Callback &callback)
    : _cb(callback)
  {
  }

  ~EventTracker()
  {
    finish();
  }

  auto push_event(EventRecord<UnitEvent> record) -> void
  {
    switch (record.event.kind) {
    case UnitEventKind::Start:
      _unit_events.push_front(std::move(record));
      break;

    case UnitEventKind::End:
      try_match_stack(_unit_events, { _cb, record });
      break;
    }
  }

  auto push_event(EventRecord<IncludeEvent> record) -> void
  {
    switch (record.event.kind) {
    case IncludeEventKind::Enter:
      _include_events.push_front(std::move(record));
      break;

    case IncludeEventKind::Leave:
      try_match_stack(_include_events, { _cb, record });
      break;
    }
  }

  auto push_event(EventRecord<ParseEvent> record) -> void
  {
    switch (record.event.kind) {
    case ParseEventKind::Start:
      _parse_events[record.event.uid].push_front(std::move(record));
      break;

    case ParseEventKind::PreGenericize:
      _genericize_events[record.event.uid].push_front(record);
      try_match_map(_parse_events, record.event.uid, [&](EventRecord<ParseEvent> *start) {
        if (start) {
          _cb.on_match(std::move(*start), std::move(record));
        }
      });
      break;

    case ParseEventKind::Finish:
      try_match_map(_parse_events, record.event.uid, [&](EventRecord<ParseEvent> *start) {
        if (start) {
          _cb.on_match(std::move(*start), std::move(record));
        } else {
          try_match_map(_genericize_events, record.event.uid, { _cb, record });
        }
      });
      break;
    }
  }

  auto push_event(EventRecord<PassEvent> record) -> void
  {
    switch (record.event.kind) {
    case PassEventKind::Start:
      _pass_events[record.event.name].push_front(std::move(record));
      break;

    case PassEventKind::End:
      try_match_map(_pass_events, record.event.name, { _cb, record });
      break;
    }
  }

  auto finish() -> void
  {
    finish_stack(_unit_events);
    finish_stack(_include_events);
    finish_map(_parse_events);
    finish_map(_genericize_events);
    finish_map(_pass_events);
  }

private:
  template <typename E, typename CB = DefaultMatchCallback<E>>
  static auto try_match_stack(Stack<E> &stack, CB &&cb) -> void
  {
    if (not stack.empty()) {
      std::forward<CB>(cb)(&stack.front());
      stack.pop_front();
    } else {
      std::forward<CB>(cb)(nullptr);
    }
  }

  template <typename K, typename E, typename CB = DefaultMatchCallback<E>>
  static auto try_match_map(StackMap<K, E> &stack_dict, const K &key, CB &&cb) -> void
  {
    auto it = stack_dict.find(key);
    if (it != stack_dict.end()) {
      try_match_stack(it->second, std::forward<CB>(cb));
      if (it->second.empty()) {
        stack_dict.erase(it);
      }
    } else {
      std::forward<CB>(cb)(nullptr);
    }
  }

  template <typename E>
  auto finish_stack(Stack<E> &stack) -> void
  {
    for (auto &entry : stack) {
      _cb.on_mismatch(std::move(entry));
    }
    stack.clear();
  }

  template <typename K, typename E>
  auto finish_map(StackMap<K, E> &stack_dict) -> void
  {
    for (auto &entry : stack_dict) {
      finish_stack(entry.second);
    }
    stack_dict.clear();
  }
};
