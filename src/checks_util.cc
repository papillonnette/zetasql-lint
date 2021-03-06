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
#include "src/checks_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "src/lint_errors.h"
#include "src/linter_options.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parse_tree_visitor.h"
#include "zetasql/parser/parser.h"
#include "zetasql/public/parse_helpers.h"
#include "zetasql/public/parse_location.h"
#include "zetasql/public/parse_resume_location.h"

namespace zetasql::linter {

absl::string_view GetNodeString(const ASTNode *node,
                                const absl::string_view &sql) {
  const int &start = node->GetParseLocationRange().start().GetByteOffset();
  const int &end = node->GetParseLocationRange().end().GetByteOffset();
  return sql.substr(start, end - start);
}

bool IsUppercase(char c) { return 'A' <= c && c <= 'Z'; }
bool IsLowercase(char c) { return 'a' <= c && c <= 'z'; }

std::string ConvertToUppercase(absl::string_view name) {
  std::string ret = "";
  for (char c : name) {
    if (IsLowercase(c))
      ret += (c - 'z' + 'Z');
    else
      ret += c;
  }
  return ret;
}

bool IsUpperCamelCase(absl::string_view name) {
  if (name.size() > 0 && !IsUppercase(name[0])) return false;
  for (char c : name)
    if (c == '_') return false;
  return true;
}

bool IsLowerCamelCase(absl::string_view name) {
  if (name.size() > 0 && !IsLowercase(name[0])) return false;
  for (char c : name)
    if (c == '_') return false;
  return true;
}

bool IsAllCaps(absl::string_view name) {
  for (char c : name)
    if (IsLowercase(c)) return false;
  return true;
}

bool IsCapsSnakeCase(absl::string_view name) {
  for (char c : name)
    if (IsLowercase(c)) return false;
  return true;
}

bool IsLowerSnakeCase(absl::string_view name) {
  for (char c : name)
    if (IsUppercase(c)) return false;
  return true;
}

bool IgnoreComments(absl::string_view sql, const LinterOptions option,
                    int *position, bool ignore_single_line) {
  int &i = *position;
  // Ignore multiline comments.
  if (i > 0 && sql[i - 1] == '/' && sql[i] == '*') {
    // It will start checking after '/*' and after the iteration
    // finished, the pointer 'i' will be just after '*/' (incrementation
    // from the for statement is included).
    i += 2;
    while (i < static_cast<int>(sql.size()) &&
           !(sql[i - 1] == '*' && sql[i] == '/')) {
      ++i;
    }
    return 1;
  }

  if (ignore_single_line) {
    // Ignore single line comments.
    if (sql[i] == '#' || (i > 0 && ((sql[i] == '-' && sql[i - 1] == '-') ||
                                    (sql[i] == '/' && sql[i - 1] == '/')))) {
      // Ignore the line.
      while (i < static_cast<int>(sql.size()) &&
             sql[i] != option.LineDelimeter()) {
        ++i;
      }
      return 1;
    }
  }
  return 0;
}

bool IgnoreStrings(absl::string_view sql, int *position) {
  int &i = *position;
  if (sql[i] == '\'' || sql[i] == '"') {
    // The sql is inside string. This will ignore sql part until
    // the same type(' or ") of character will occur
    // without \ in the beginning. For example 'a"b' is a valid string.
    for (int j = i + 1; j < static_cast<int>(sql.size()); ++j) {
      if (sql[j - 1] == '\\' && sql[j] == sql[i]) continue;
      if (sql[j] == sql[i] || j + 1 == static_cast<int>(sql.size())) {
        i = j;
        break;
      }
    }
    return 1;
  }
  return 0;
}

std::string GetNextWord(absl::string_view sql, int *position) {
  int &i = *position;
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t')) i++;
  std::string word = "";
  while (i < sql.size() &&
         !(sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == ';' ||
           sql[i] == '(' || sql[i] == ',')) {
    word += sql[i];
    i++;
  }
  return word;
}

LinterResult PrintASTTree(absl::string_view sql) {
  absl::Status return_status;
  std::unique_ptr<ParserOutput> output;

  ParseResumeLocation location = ParseResumeLocation::FromStringView(sql);
  bool is_the_end = false;
  int cnt = 0;
  while (!is_the_end) {
    return_status = ParseNextScriptStatement(&location, ParserOptions(),
                                             &output, &is_the_end);

    std::cout << "Status for sql#" << ++cnt << " : \""
              << "\" = " << return_status.ToString() << std::endl;

    if (return_status.ok()) {
      std::cout << output->statement()->DebugString() << std::endl;
    } else {
      break;
    }
  }
  return LinterResult(return_status);
}

bool OneLineStatement(absl::string_view line) {
  bool first = true;
  bool last = false;
  bool finish = false;
  std::vector<std::string> last_words{"FUNCTION", "EXISTS", "TABLE", "TYPE",
                                      "VIEW",     "=",      "PROTO", "MODULE"};
  for (auto word : absl::StrSplit(line, ' ')) {
    if (word.size() == 0) continue;
    if (finish) return false;
    std::string uppercase_word = ConvertToUppercase(word);
    if (first) {
      if (uppercase_word != "CREATE" && uppercase_word != "IMPORT")
        return false;
      first = false;
      continue;
    }
    if (last) {
      finish = true;
      continue;
    }
    for (auto &last_word : last_words)
      if (uppercase_word == last_word) {
        if (last_word == "=") finish = true;
        last = true;
      }
  }
  return true;
}

bool ConsistentUppercaseLowercase(const absl::string_view &sql,
                                  const ParseLocationRange &range) {
  bool uppercase = false;
  bool lowercase = false;
  for (int i = range.start().GetByteOffset(); i < range.end().GetByteOffset();
       ++i) {
    if ('a' <= sql[i] && sql[i] <= 'z') lowercase = true;
    if ('A' <= sql[i] && sql[i] <= 'Z') uppercase = true;
  }
  // There shouldn't be any case any Keyword
  // contains both uppercase and lowercase characters
  return !(lowercase && uppercase);
}

LinterResult ASTNodeRule::ApplyTo(absl::string_view sql,
                                  const LinterOptions &option) {
  RuleVisitor visitor(rule_, sql, option);

  std::unique_ptr<ParserOutput> output;
  ParseResumeLocation location = ParseResumeLocation::FromStringView(sql);
  absl::Status status;
  LinterResult result;

  bool is_the_end = false;
  while (!is_the_end) {
    status = ParseNextScriptStatement(&location, ParserOptions(), &output,
                                      &is_the_end);
    if (!status.ok()) return LinterResult();

    status = output->statement()->TraverseNonRecursive(&visitor);
    if (!status.ok()) return LinterResult(status);
  }

  return visitor.GetResult();
}

zetasql_base::StatusOr<VisitResult> RuleVisitor::defaultVisit(
    const ASTNode *node) {
  result_.Add(rule_(node, sql_, option_));
  return VisitResult::VisitChildren(node);
}

}  // namespace zetasql::linter
