// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 Shota Minami

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "event.hpp"

#include <gcc-plugin.h>

#include <coretypes.h>
#include <langhooks.h>
#include <tree.h>

class TraceWriter
{
  std::FILE *_file;
  EventTimePoint _epoch;
  int _decl_verbosity;

  std::size_t _slice_count;
  std::unordered_map<unsigned int, std::string> _decl_name_cache;

  class SliceWriter
  {
    TraceWriter &_writer;

  public:
    SliceWriter(TraceWriter &writer, const std::string &name, EventTimePoint start, EventTimePoint end)
      : _writer(writer)
    {
      auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(start - _writer._epoch).count();
      auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

      if (_writer._slice_count++ > 0) {
        std::fprintf(_writer._file, ",");
      }
      std::fprintf(_writer._file, "{\"name\":\"%s\",\"ts\":%ld.%03ld,", name.c_str(), ts / 1000, ts % 1000);
      if (dur > 0) {
        std::fprintf(_writer._file, "\"ph\":\"X\",\"dur\":%ld.%03ld,", dur / 1000, dur % 1000);
      } else {
        std::fprintf(_writer._file, "\"ph\":\"i\",");
      }
      std::fprintf(_writer._file, "\"pid\":0,\"tid\":0");
    }

    ~SliceWriter()
    {
      std::fprintf(_writer._file, "}");
    }
  };

  class ArgWriter
  {
    TraceWriter &_writer;

  public:
    ArgWriter(TraceWriter &writer)
      : _writer(writer)
    {
      std::fprintf(_writer._file, ",\"args\":{");
    }

    ~ArgWriter()
    {
      std::fprintf(_writer._file, "}");
    }
  };

public:
  TraceWriter(const TraceWriter &) = delete;
  TraceWriter(TraceWriter &&) = delete;

  TraceWriter(std::FILE *file, EventTimePoint epoch, int decl_verbosity)
    : _file(file)
    , _epoch(epoch)
    , _decl_verbosity(decl_verbosity)
    , _slice_count(0)
  {
    std::fprintf(_file, "[");
  }

  ~TraceWriter()
  {
    std::fprintf(_file, "]");
  }

  auto write_slice(EventRecord<UnitEvent> start, EventRecord<UnitEvent> end) -> void
  {
    SliceWriter slice { *this, "unit", start.timestamp, end.timestamp };
  }

  auto write_slice(EventRecord<IncludeEvent> start, EventRecord<IncludeEvent> end) -> void
  {
    SliceWriter slice { *this, "include", start.timestamp, end.timestamp };
    ArgWriter arg { *this };
    std::fprintf(_file, "\"file\":\"%s\"", start.event.filename.c_str());
  }

  auto write_slice(EventRecord<ParseEvent> start, EventRecord<ParseEvent> end) -> void
  {
    auto name = start.event.kind == ParseEventKind::Start ? "parse" : "genericize";
    SliceWriter slice { *this, name, start.timestamp, end.timestamp };
    ArgWriter arg { *this };
    std::fprintf(_file, "\"function\":\"%s\"", get_decl_name(start.event.decl).c_str());
  }

  auto write_slice(EventRecord<PassEvent> start, EventRecord<PassEvent> end) -> void
  {
    SliceWriter slice { *this, start.event.name, start.timestamp, end.timestamp };
    if (start.event.decl) {
      ArgWriter arg { *this };
      std::fprintf(_file, "\"function\":\"%s\"", get_decl_name(start.event.decl).c_str());
    }
  }

  auto write_slice(EventRecord<UnitEvent> end) -> void
  {
    auto name = end.event.kind == UnitEventKind::Start ? "unit (start)" : "unit (end)";
    SliceWriter slice { *this, name, end.timestamp, end.timestamp };
  }

  auto write_slice(EventRecord<IncludeEvent> end) -> void
  {
    auto name = end.event.kind == IncludeEventKind::Enter ? "include (enter)" : "include (leave)";
    SliceWriter slice { *this, name, end.timestamp, end.timestamp };
    if (end.event.kind == IncludeEventKind::Enter) {
      ArgWriter arg { *this };
      std::fprintf(_file, "\"file\":\"%s\"", end.event.filename.c_str());
    }
  }

  auto write_slice(EventRecord<ParseEvent> end) -> void
  {
    auto name = "";
    switch (end.event.kind) {
    case ParseEventKind::Start:
      return;
    case ParseEventKind::PreGenericize:
      name = "genericize (start)";
      break;
    case ParseEventKind::Finish:
      name = "parse (finish)";
      break;
    }

    SliceWriter slice { *this, name, end.timestamp, end.timestamp };
    if (end.event.decl) {
      ArgWriter arg { *this };
      std::fprintf(_file, "\"function\":\"%s\"", get_decl_name(end.event.decl).c_str());
    }
  }

  auto write_slice(EventRecord<PassEvent> end) -> void
  {
    auto name = end.event.name;
    name += end.event.kind == PassEventKind::Start ? " (start)" : " (cancelled)";
    SliceWriter slice { *this, name, end.timestamp, end.timestamp };
    if (end.event.decl) {
      ArgWriter arg { *this };
      std::fprintf(_file, "\"function\":\"%s\"", get_decl_name(end.event.decl).c_str());
    }
  }

  auto write_slice(std::string name, EventTimePoint start, EventTimePoint end) -> void
  {
    SliceWriter slice { *this, name, start, end };
  }

private:
  auto get_decl_name(tree decl) -> const std::string &
  {
    auto uid = DECL_PT_UID(decl);
    auto it = _decl_name_cache.find(uid);
    if (it == _decl_name_cache.end()) {
      std::string escaped;
      auto name = ::lang_hooks.decl_printable_name(decl, _decl_verbosity);
      std::size_t len;
      for (; len = std::strcspn(name, "\""), name[len]; name += len + 1) {
        escaped.append(name, len);
        escaped += "\\\"";
      }
      escaped.append(name, len);
      it = _decl_name_cache.emplace(uid, escaped).first;
    }
    return it->second;
  }
};

struct WriteCallback
{
  TraceWriter &writer;

  template <typename E>
  auto on_match(EventRecord<E> start, EventRecord<E> end) -> void
  {
    writer.write_slice(std::move(start), std::move(end));
  }

  template <typename E>
  auto on_mismatch(EventRecord<E> end) -> void
  {
    writer.write_slice(std::move(end));
  }
};
