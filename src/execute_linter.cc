//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "src/execute_linter.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>

#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "re2/re2.h"
#include "src/check_list.h"
#include "src/config.pb.h"
#include "src/lint_errors.h"
#include "src/linter.h"
#include "src/linter_options.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"

namespace zetasql::linter {

LinterResult ParseNoLintSingleComment(absl::string_view line,
                                      const absl::string_view& sql,
                                      int position, LinterOptions* options) {
  LinterResult result;
  std::map<std::string, ErrorCode> error_map = GetErrorMap();

  const LazyRE2 kRegex = {"\\s*(NOLINT|LINT)\\s*\\(([a-z ,-]*)\\)\\s*(.*)\\s*"};
  std::string type = "";
  std::string check_names = "";
  std::string lint_comment = "";
  bool matched =
      RE2::FullMatch(line, *kRegex, &type, &check_names, &lint_comment);

  // Found enabling or disabling type of comment.
  if (matched) {
    check_names.erase(
        std::remove_if(check_names.begin(), check_names.end(), ::isspace),
        check_names.end());

    std::vector<std::string> names = absl::StrSplit(check_names, ',');
    for (std::string check_name : names) {
      // The name inside of parantheses is stored in 'check_name'
      // If it is not valid add error, otherwise enable/disable position
      if (!error_map.count(check_name)) {
        result.Add(ErrorCode::kNoLint, sql, position,
                   absl::StrCat("Unkown NOLINT error category: ", check_name));
      } else {
        const ErrorCode& code = error_map[check_name];
        if (type == "NOLINT")
          options->Disable(code, position);
        else
          options->Enable(code, position);
      }
    }
  }
  return result;
}

// TODO(orhanuysal): Extract the code so that 'CheckCommentType'
// implementation and this implementation won't have the same parts.
LinterResult ParseNoLintComments(absl::string_view sql,
                                 LinterOptions* options) {
  LinterResult result;
  for (int i = 0; i < static_cast<int>(sql.size()); ++i) {
    if (IgnoreComments(sql, *options, &i, false)) continue;
    if (IgnoreStrings(sql, &i)) continue;

    bool is_line_comment = false;
    if (i > 0 && sql[i - 1] == '-' && sql[i] == '-') is_line_comment = true;
    if (i > 0 && sql[i - 1] == '/' && sql[i] == '/') is_line_comment = true;
    if (sql[i] == '#') is_line_comment = true;

    if (is_line_comment) {
      std::string line = "";
      while (i + 1 < static_cast<int>(sql.size()) &&
             sql[i] != options->LineDelimeter())
        line += sql[++i];
      result.Add(ParseNoLintSingleComment(line, sql, i, options));
      continue;
    }
  }

  return result;
}

LinterOptions GetOptionsFromConfig(Config config, absl::string_view filename) {
  LinterOptions option(filename);
  if (config.has_tab_size()) option.SetTabSize(config.tab_size());

  if (config.has_end_line()) option.SetLineDelimeter(config.end_line()[0]);

  if (config.has_line_limit()) option.SetLineLimit(config.line_limit());

  if (config.has_single_quote()) option.SetSingleQuote(config.single_quote());

  if (config.has_allowed_indent())
    option.SetAllowedIndent(config.allowed_indent()[0]);

  std::map<std::string, ErrorCode> error_map = GetErrorMap();

  for (std::string check_name : config.nolint()) {
    if (error_map.count(check_name)) {
      option.DisactivateCheck(error_map[check_name]);
    }
  }
  return option;
}

LinterResult RunChecks(absl::string_view sql, LinterOptions options) {
  CheckList list = GetAllChecks();
  LinterResult result = ParseNoLintComments(sql, &options);
  result.SetFilename(options.Filename());
  for (auto check : list.GetList()) {
    result.Add(check(sql, options));
  }
  return result;
}

LinterResult RunChecks(absl::string_view sql, Config config,
                       absl::string_view filename) {
  LinterOptions options = GetOptionsFromConfig(config, filename);
  return RunChecks(sql, options);
}

LinterResult RunChecks(absl::string_view sql, absl::string_view filename) {
  LinterOptions options(filename);
  return RunChecks(sql, options);
}

LinterResult RunChecks(absl::string_view sql) {
  LinterOptions options;
  return RunChecks(sql, options);
}

}  // namespace zetasql::linter
