// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 Shota Minami

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <forward_list>
#include <string>
#include <utility>
#include <vector>

#include "event.hpp"
#include "writer.hpp"

#include <gcc-plugin.h>

#include <c-family/c-pragma.h>
#include <context.h>
#include <coretypes.h>
#include <diagnostic-core.h>
#include <dumpfile.h>
#include <input.h>
#include <intl.h>
#include <line-map.h>
#include <options.h>
#include <pass_manager.h>
#include <plugin-version.h>
#include <plugin.h>
#include <timevar.h>
#include <toplev.h>
#include <tree-core.h>
#include <tree-pass.h>
#include <tree.h>

int plugin_is_GPL_compatible;

enum class TimeTracePassKind
{
  Single,
  StartList,
  EndList,
};

class TimeTracePass final : public opt_pass
{
public:
  TimeTracePassKind trace_kind;
  std::string trace_name;

  TimeTracePass(opt_pass_type type, TimeTracePassKind kind, std::string name)
    : opt_pass({ type, "*time_trace", OPTGROUP_ALL, TV_NONE }, ::g)
    , trace_kind(kind)
    , trace_name(std::move(name))
  {
  }

private:
  auto clone() -> opt_pass * final override
  {
    return new TimeTracePass { type, trace_kind, trace_name };
  }

  auto gate(function *) -> bool final override
  {
    return false;
  }
};

int decl_verbosity;
bool version_check;
void (*old_cb_file_change)(cpp_reader *, const line_map_ordinary *);

std::forward_list<EventRecord<UnitEvent>> trace_unit;
std::forward_list<EventRecord<IncludeEvent>> trace_include;
std::forward_list<EventRecord<ParseEvent>> trace_parse;
std::forward_list<EventRecord<PassEvent>> trace_pass;

static auto cb_file_change(cpp_reader *parse_in, const line_map_ordinary *line_map) -> void
{
  if (line_map) {
    if (line_map->reason == LC_ENTER) {
      trace_include.push_front(IncludeEvent { IncludeEventKind::Enter, ORDINARY_MAP_FILE_NAME(line_map) });
    } else if (line_map->reason == LC_LEAVE) {
      trace_include.push_front(IncludeEvent { IncludeEventKind::Leave });
    }
  }
  if (old_cb_file_change) {
    old_cb_file_change(parse_in, line_map);
  }
}

static auto start_unit_callback(void *, void *) -> void
{
  auto cb = cpp_get_callbacks(parse_in);
  old_cb_file_change = cb->file_change;
  cb->file_change = &cb_file_change;
  trace_unit.push_front({ { UnitEventKind::Start } });
}

static auto finish_unit_callback(void *, void *) -> void
{
  trace_unit.push_front({ { UnitEventKind::End } });
}

static auto start_parse_function_callback(void *event_data, void *) -> void
{
  auto fndecl = static_cast<tree>(event_data);
  trace_parse.push_front({ { ParseEventKind::Start, fndecl, DECL_PT_UID(fndecl) } });
}

static auto pre_genericize_callback(void *event_data, void *) -> void
{
  auto fndecl = static_cast<tree>(event_data);
  trace_parse.push_front({ { ParseEventKind::PreGenericize, fndecl, DECL_PT_UID(fndecl) } });
}

static auto finish_parse_function_callback(void *event_data, void *) -> void
{
  auto fndecl = static_cast<tree>(event_data);
  trace_parse.push_front({ { ParseEventKind::Finish, fndecl, DECL_PT_UID(fndecl) } });
}

static auto early_gimple_passes_start_callback(void *, void *) -> void
{
  trace_pass.push_front({ { PassEventKind::Start, "early_gimple_passes" } });
}

static auto early_gimple_passes_end_callback(void *, void *) -> void
{
  trace_pass.push_front({ { PassEventKind::End, "early_gimple_passes" } });
}

static auto all_ipa_passes_start_callback(void *, void *) -> void
{
  trace_pass.push_front({ { PassEventKind::Start, "all_ipa_passes" } });
}

static auto all_ipa_passes_end_callback(void *, void *) -> void
{
  trace_pass.push_front({ { PassEventKind::End, "all_ipa_passes" } });
}

static auto override_gate_callback(void *, void *) -> void
{
  if (std::strcmp(::current_pass->name, "*time_trace") == 0) {
    auto pass = static_cast<TimeTracePass *>(::current_pass);
    auto uid = ::current_function_decl ? DECL_PT_UID(::current_function_decl) : -1u;
    switch (pass->trace_kind) {
    case TimeTracePassKind::Single:
      trace_pass.push_front({ { PassEventKind::End, pass->trace_name, NULL_TREE, -1u } });
      break;

    case TimeTracePassKind::StartList:
      trace_pass.push_front({ { PassEventKind::Start, pass->trace_name, ::current_function_decl, uid } });
      break;

    case TimeTracePassKind::EndList:
      trace_pass.push_front({ { PassEventKind::End, pass->trace_name, ::current_function_decl, uid } });
      break;
    }
  }
}

static auto pass_execution_callback(void *event_data, void *) -> void
{
  auto pass = static_cast<opt_pass *>(event_data);
  trace_pass.push_front({ { PassEventKind::Start, pass->name, NULL_TREE, -1u } });
}

static auto finish_callback(void *, void *) -> void
{
  auto dump_start = EventClock::now();

  trace_unit.reverse();
  trace_include.reverse();
  trace_parse.reverse();
  trace_pass.reverse();

  auto epoch = std::min({
    trace_unit.empty() ? EventTimePoint::max() : trace_unit.front().timestamp,
    trace_include.empty() ? EventTimePoint::max() : trace_include.front().timestamp,
    trace_parse.empty() ? EventTimePoint::max() : trace_parse.front().timestamp,
    trace_pass.empty() ? EventTimePoint::max() : trace_pass.front().timestamp,
  });

  struct File
  {
    FILE *file;
    ~File() { ::fclose(file); }
  };

  std::string filename;
  filename += aux_base_name;
  filename += ".trace.json";
  File guard { ::fopen(filename.c_str(), "wb") };
  TraceWriter writer { guard.file, epoch, decl_verbosity };
  WriteCallback cb { writer };

  EventTracker<WriteCallback> tracker { cb };
  for (auto &event : trace_unit) {
    tracker.push_event(event);
  }
  for (auto &event : trace_include) {
    tracker.push_event(event);
  }
  for (auto &event : trace_parse) {
    tracker.push_event(event);
  }
  for (auto &event : trace_pass) {
    tracker.push_event(event);
  }
  tracker.finish();

  auto dump_end = EventClock::now();
  writer.write_slice("plugin_dump", dump_start, dump_end);
}

static auto setup_option(plugin_name_args *args) -> bool
{
  decl_verbosity = 1;
  version_check = true;
  for (auto i = 0; i < args->argc; ++i) {
    if (std::strcmp(args->argv[i].key, "verbose-decl") == 0) {
      if (not args->argv[i].value) {
        error("missing argument to %<-fplugin-arg-%s-%s%>", args->base_name, args->argv[i].key);
        return false;
      }

      if (std::strcmp(args->argv[i].value, "0") == 0) {
        decl_verbosity = 0;
      } else if (std::strcmp(args->argv[i].value, "1") == 0) {
        decl_verbosity = 1;
      } else if (std::strcmp(args->argv[i].value, "2") == 0) {
        decl_verbosity = 2;
      } else {
        error("argument of %<-fplugin-arg-%s-%s%> must be 0, 1, or 2", args->base_name, args->argv[i].key);
        return false;
      }
    } else if (std::strcmp(args->argv[i].key, "disable-version-check") == 0) {
      if (args->argv[i].value) {
        error("unexpected argument to %<-fplugin-arg-%s-%s%>", args->base_name, args->argv[i].key);
        return false;
      }

      version_check = false;
    } else {
      error("unrecoginized timetrace plugin option %<-fplugin-arg-%s-%s%>", args->base_name, args->argv[i].key);
      return false;
    }
  }
  return true;
}

static auto collect_passes(const opt_pass *pass, std::vector<const opt_pass *> &passes) -> void
{
  for (; pass; pass = pass->next) {
    passes.push_back(pass);
    collect_passes(pass->sub, passes);
  }
}

static auto setup_time_trace_passes() -> void
{
  std::vector<const opt_pass *> passes;
  for (auto list : ::g->get_passes()->pass_lists) {
    collect_passes(*list, passes);
  }

  for (auto i = passes.cbegin(); i != passes.cend(); ++i) {
    auto probe = [&](const opt_pass *pass) { return pass->type != (*i)->type or std::strcmp(pass->name, (*i)->name); };
    if (std::all_of(passes.cbegin(), i, probe)) {
      auto pass = new TimeTracePass { (*i)->type, TimeTracePassKind::Single, (*i)->name };
      register_pass(pass, PASS_POS_INSERT_AFTER, (*i)->name, 0);
    }
  }

#define DEF_PASS_LIST(LIST) { ::g->get_passes()->LIST, #LIST },
  std::vector<std::pair<const opt_pass *, std::string>> pass_lists { GCC_PASS_LISTS };
#undef DEF_PASS_LIST

  for (auto pass : passes) {
    for (auto name : { "build_ssa_passes", "opt_local_passes" }) {
      if (not std::strcmp(pass->name, name)) {
        std::string list_name;
        list_name += name;
        list_name += "_local";
        pass_lists.emplace_back(pass->sub, std::move(list_name));
      }
    }
  }

  for (const auto &pass_list : pass_lists) {
    auto pass = pass_list.first;
    auto start = new TimeTracePass { pass->type, TimeTracePassKind::StartList, pass_list.second };
    register_pass(start, PASS_POS_INSERT_BEFORE, pass->name, pass->static_pass_number);
    while (pass->next) {
      pass = pass->next;
    }
    auto end = new TimeTracePass { pass->type, TimeTracePassKind::EndList, pass_list.second };
    register_pass(end, PASS_POS_INSERT_AFTER, pass->name, pass->static_pass_number);
  }
}

static auto setup_plugin_callbacks(const char *plugin_name) -> void
{
  register_callback(plugin_name, PLUGIN_FINISH_UNIT, &finish_unit_callback, nullptr);
  register_callback(plugin_name, PLUGIN_START_UNIT, &start_unit_callback, nullptr);
  register_callback(plugin_name, PLUGIN_PRE_GENERICIZE, &pre_genericize_callback, nullptr);
  register_callback(plugin_name, PLUGIN_START_PARSE_FUNCTION, &start_parse_function_callback, nullptr);
  register_callback(plugin_name, PLUGIN_FINISH_PARSE_FUNCTION, &finish_parse_function_callback, nullptr);
  register_callback(plugin_name, PLUGIN_EARLY_GIMPLE_PASSES_START, &early_gimple_passes_start_callback, nullptr);
  register_callback(plugin_name, PLUGIN_EARLY_GIMPLE_PASSES_END, &early_gimple_passes_end_callback, nullptr);
  register_callback(plugin_name, PLUGIN_ALL_IPA_PASSES_START, &all_ipa_passes_start_callback, nullptr);
  register_callback(plugin_name, PLUGIN_ALL_IPA_PASSES_END, &all_ipa_passes_end_callback, nullptr);
  register_callback(plugin_name, PLUGIN_OVERRIDE_GATE, &override_gate_callback, nullptr);
  register_callback(plugin_name, PLUGIN_PASS_EXECUTION, &pass_execution_callback, nullptr);
  register_callback(plugin_name, PLUGIN_FINISH, &finish_callback, nullptr);
}

auto plugin_init(plugin_name_args *args, plugin_gcc_version *version) -> int
{
  if (not setup_option(args)) {
    return 1;
  }

  setup_time_trace_passes();
  setup_plugin_callbacks(args->base_name);

  if (version_check and not plugin_default_version_check(version, &::gcc_version)) {
    error("plugin %qs is built for a different version of GCC", args->base_name);
    return 1;
  }

  return 0;
}
