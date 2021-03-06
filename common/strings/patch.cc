// Copyright 2017-2020 The Verible Authors.
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

#include "common/strings/patch.h"

#include <iostream>
#include <iterator>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "common/strings/position.h"
#include "common/strings/split.h"
#include "common/util/algorithm.h"
#include "common/util/container_iterator_range.h"
#include "common/util/iterator_range.h"

namespace verible {

static bool LineMarksOldFile(absl::string_view line) {
  return absl::StartsWith(line, "--- ");
}

static bool IsValidMarkedLine(absl::string_view line) {
  if (line.empty()) return false;
  switch (line.front()) {
    case ' ':
    case '-':
    case '+':
      return true;
    default:
      return false;
  }
}

namespace internal {

static std::vector<LineRange> LineIteratorsToRanges(
    const std::vector<LineIterator>& iters) {
  // TODO(fangism): This pattern appears elsewhere in the codebase, so refactor.
  CHECK_GE(iters.size(), 2);
  std::vector<LineRange> result;
  result.reserve(iters.size());
  auto prev = iters.begin();
  for (auto next = std::next(prev); next != iters.end(); prev = next, ++next) {
    result.emplace_back(*prev, *next);
  }
  return result;
}

absl::Status MarkedLine::Parse(absl::string_view text) {
  // text is already a whole line
  if (!IsValidMarkedLine(text)) {
    return absl::InvalidArgumentError(absl::StrCat(
        "MarkedLine must begin with one of [ -+], but got: \"", text, "\"."));
  }
  line.assign(text.begin(), text.end());  // copy
  return absl::OkStatus();
}

std::ostream& operator<<(std::ostream& stream, const MarkedLine& line) {
  return stream << line.line;
}

absl::Status HunkIndices::Parse(absl::string_view text) {
  // text is expected to look like "int,int"
  StringSpliterator splitter(text);
  const absl::string_view start_text = splitter(',');
  const absl::string_view count_text = splitter(',');
  if (!absl::SimpleAtoi(start_text, &start) ||  //
      !absl::SimpleAtoi(count_text, &count) ||  //
      splitter /* unexpected second ',' */) {
    return absl::InvalidArgumentError(
        absl::StrCat("HunkIndices expects int,int, but got: ", text, "\"."));
  }
  return absl::OkStatus();
}

std::ostream& operator<<(std::ostream& stream, const HunkIndices& indices) {
  return stream << indices.start << ',' << indices.count;
}

absl::Status HunkHeader::Parse(absl::string_view text) {
  constexpr absl::string_view kDelimiter("@@");
  StringSpliterator tokenizer(text);
  {
    absl::string_view first = tokenizer(kDelimiter);
    // first token should be empty
    if (!first.empty() || !tokenizer) {
      return absl::InvalidArgumentError(absl::StrCat(
          "HunkHeader should start with @@, but got: ", text, "\"."));
    }
  }

  // Parse ranges between the "@@"s.
  {
    const absl::string_view ranges =
        absl::StripAsciiWhitespace(tokenizer(kDelimiter));
    if (!tokenizer) {
      return absl::InvalidArgumentError(absl::StrCat(
          "HunkHeader expects ranges in @@...@@, but got: ", text, "\"."));
    }

    auto splitter = MakeStringSpliterator(ranges, ' ');
    absl::string_view old_range_str(splitter());
    if (!absl::ConsumePrefix(&old_range_str, "-")) {
      return absl::InvalidArgumentError(absl::StrCat(
          "old-file range should start with '-', but got: ", old_range_str,
          "\"."));
    }
    absl::string_view new_range_str(splitter());
    if (!absl::ConsumePrefix(&new_range_str, "+")) {
      return absl::InvalidArgumentError(absl::StrCat(
          "new-file range should start with '+', but got: ", new_range_str,
          "\"."));
    }
    {
      const auto status = old_range.Parse(old_range_str);
      if (!status.ok()) return status;
    }
    {
      const auto status = new_range.Parse(new_range_str);
      if (!status.ok()) return status;
    }
  }

  // Text that follows the last "@@" provides context and is optional.
  const absl::string_view trailing_text = tokenizer(kDelimiter);
  context.assign(trailing_text.begin(), trailing_text.end());

  return absl::OkStatus();
}

std::ostream& operator<<(std::ostream& stream, const HunkHeader& header) {
  return stream << "@@ -" << header.old_range << " +" << header.new_range
                << " @@" << header.context;
}

// Type M could be any container or range of MarkedLines.
template <class M>
static void CountMarkedLines(const M& lines, int* before, int* after) {
  *before = 0;
  *after = 0;
  for (const auto& line : lines) {
    switch (line.Marker()) {
      case ' ':  // line is common to both, unchanged
        ++*before;
        ++*after;
        break;
      case '-':
        ++*before;
        break;
      case '+':
        ++*after;
        break;
    }
  }
}

absl::Status Hunk::IsValid() const {
  int original_lines = 0;
  int new_lines = 0;
  CountMarkedLines(lines, &original_lines, &new_lines);
  if (original_lines != header.old_range.count) {
    return absl::InvalidArgumentError(
        absl::StrCat("Hunk is invalid: expected ", header.old_range.count,
                     " lines before, but got ", original_lines, "."));
  }
  if (new_lines != header.new_range.count) {
    return absl::InvalidArgumentError(
        absl::StrCat("Hunk is invalid: expected ", header.new_range.count,
                     " lines after, but got ", new_lines, "."));
  }
  return absl::OkStatus();
}

void Hunk::UpdateHeader() {
  CountMarkedLines(lines, &header.old_range.count, &header.new_range.count);
}

LineNumberSet Hunk::AddedLines() const {
  LineNumberSet line_numbers;
  int line_number = header.new_range.start;
  for (const auto& line : lines) {
    if (line.Marker() == '+') line_numbers.Add(line_number);
    if (line.Marker() != '-') ++line_number;
  }
  return line_numbers;
}

absl::Status Hunk::Parse(const LineRange& hunk_lines) {
  {
    const auto status = header.Parse(hunk_lines.front());
    if (!status.ok()) return status;
  }

  LineRange body(hunk_lines);
  body.pop_front();  // remove the header
  lines.resize(body.size());
  auto line_iter = lines.begin();
  for (const auto& line : body) {
    const auto status = line_iter->Parse(line);
    if (!status.ok()) return status;
    ++line_iter;
  }
  return IsValid();
}

std::ostream& operator<<(std::ostream& stream, const Hunk& hunk) {
  stream << hunk.header << std::endl;
  for (const auto& line : hunk.lines) {
    stream << line << std::endl;
  }
  return stream;
}

absl::Status SourceInfo::Parse(absl::string_view text) {
  StringSpliterator splitter(text);

  absl::string_view token = splitter('\t');
  path.assign(token.begin(), token.end());

  token = splitter('\t');  // time string (optional) is not parsed any further
  timestamp.assign(token.begin(), token.end());

  if (path.empty() || timestamp.empty() ||
      splitter /* unexpected trailing text */) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected \"path timestamp\" (tab-separated), but got: \"",
                     text, "\"."));
  }
  return absl::OkStatus();
}

std::ostream& operator<<(std::ostream& stream, const SourceInfo& info) {
  return stream << info.path << '\t' << info.timestamp;
}

static absl::Status ParseSourceInfoWithMarker(
    SourceInfo* info, absl::string_view line,
    absl::string_view expected_marker) {
  StringSpliterator splitter(line);
  absl::string_view marker = splitter(' ');
  if (marker != expected_marker) {
    return absl::InvalidArgumentError(
        absl::StrCat("Expected old-file marker \"", expected_marker,
                     "\", but got: \"", marker, "\""));
  }
  return info->Parse(splitter.Remainder());
}

bool FilePatch::IsNewFile() const { return old_file.path == "/dev/null"; }

bool FilePatch::IsDeletedFile() const { return new_file.path == "/dev/null"; }

LineNumberSet FilePatch::AddedLines() const {
  LineNumberSet line_numbers;
  for (const auto& hunk : hunks) {
    line_numbers.Union(hunk.AddedLines());
  }
  return line_numbers;
}

absl::Status FilePatch::Parse(const LineRange& lines) {
  LineIterator line_iter(
      std::find_if(lines.begin(), lines.end(), &LineMarksOldFile));
  if (lines.begin() == lines.end() || line_iter == lines.end()) {
    return absl::InvalidArgumentError(
        "Expected a file marker starting with \"---\", but did not find one.");
  }
  // Lines leading up to the old file marker "---" are metadata.
  for (const auto& line : make_range(lines.begin(), line_iter)) {
    metadata.emplace_back(line);
  }

  {
    const auto status = ParseSourceInfoWithMarker(&old_file, *line_iter, "---");
    if (!status.ok()) return status;
  }
  ++line_iter;
  if (line_iter == lines.end()) {
    return absl::InvalidArgumentError(
        "Expected a file marker starting with \"+++\", but did not find one.");
  } else {
    const auto status = ParseSourceInfoWithMarker(&new_file, *line_iter, "+++");
    if (!status.ok()) return status;
  }
  ++line_iter;

  // find hunk starts, and parse ranges of hunk texts
  std::vector<LineIterator> hunk_starts;
  find_all(
      line_iter, lines.end(), std::back_inserter(hunk_starts),
      [](absl::string_view line) { return absl::StartsWith(line, "@@ "); });

  if (hunk_starts.empty()) {
    // Unusual, but degenerate case of no hunks is parseable and valid.
    return absl::OkStatus();
  }

  hunk_starts.push_back(lines.end());  // make it easier to construct ranges
  const std::vector<LineRange> hunk_ranges(LineIteratorsToRanges(hunk_starts));

  hunks.resize(hunk_ranges.size());
  auto hunk_iter = hunks.begin();
  for (const auto& hunk_range : hunk_ranges) {
    const auto status = hunk_iter->Parse(hunk_range);
    if (!status.ok()) return status;
    ++hunk_iter;
  }
  return absl::OkStatus();
}

std::ostream& operator<<(std::ostream& stream, const FilePatch& patch) {
  for (const auto& line : patch.metadata) {
    stream << line << std::endl;
  }
  stream << "--- " << patch.old_file << '\n'  //
         << "+++ " << patch.new_file << std::endl;
  for (const auto& hunk : patch.hunks) {
    stream << hunk;
  }
  return stream;
}

}  // namespace internal

static bool LineBelongsToPreviousSection(absl::string_view line) {
  if (line.empty()) return true;
  return IsValidMarkedLine(line);
}

absl::Status PatchSet::Parse(absl::string_view patch_contents) {
  // Split lines.  The resulting lines will not include the \n delimiters.
  std::vector<absl::string_view> lines(
      absl::StrSplit(patch_contents, absl::ByChar('\n')));

  // Consider an empty patch file valid.
  if (lines.empty()) return absl::OkStatus();

  // Well-formed files end with a newline [POSIX], so delete the last partition.
  internal::LineRange lines_range(lines.cbegin(), std::prev(lines.cend()));

  // Split set of lines into ranges that correspond to individual files.
  // Strategy: find all old-file lines that start with "--- ", and then
  // search backwards to find the last line that starts with one of [ +-].
  std::vector<internal::LineIterator> file_patch_begins;
  {
    find_all(lines_range.begin(), lines_range.end(),
             std::back_inserter(file_patch_begins), &LineMarksOldFile);
    if (file_patch_begins.empty()) return absl::OkStatus();

    // Move line iterators back to correct starting points.
    for (auto& iter : file_patch_begins) {
      while (iter != lines_range.begin()) {
        const auto prev = std::prev(iter);
        const absl::string_view& peek(*prev);
        if (LineBelongsToPreviousSection(peek)) break;
        iter = prev;
      }
    }

    // For easier construction of ranges, append an end() iterator.
    file_patch_begins.push_back(lines_range.end());
  }

  // Record metadata lines, if there are any.
  for (const auto& line :
       make_range(lines_range.begin(), file_patch_begins.front())) {
    metadata_.emplace_back(line);
  }

  // Parse individual file patches.
  const std::vector<internal::LineRange> file_patch_ranges(
      internal::LineIteratorsToRanges(file_patch_begins));
  file_patches_.resize(file_patch_ranges.size());
  auto iter = file_patches_.begin();
  for (const auto& range : file_patch_ranges) {
    const auto status = iter->Parse(range);
    if (!status.ok()) return status;
    ++iter;
  }

  // TODO(fangism): pass around line numbers to include in diagnostics

  return absl::OkStatus();
}

std::ostream& PatchSet::Render(std::ostream& stream) const {
  for (const auto& line : metadata_) {
    stream << line << std::endl;
  }
  for (const auto& file_patch : file_patches_) {
    stream << file_patch;
  }
  return stream;
}

FileLineNumbersMap PatchSet::AddedLinesMap(bool new_file_ranges) const {
  FileLineNumbersMap result;
  for (const auto& file_patch : file_patches_) {
    if (file_patch.IsDeletedFile()) continue;
    LineNumberSet& entry = result[file_patch.new_file.path];
    if (file_patch.IsNewFile() && !new_file_ranges) {
      entry.clear();
    } else {
      entry = file_patch.AddedLines();
    }
  }
  return result;
}

std::ostream& operator<<(std::ostream& stream, const PatchSet& patch) {
  return patch.Render(stream);
}

}  // namespace verible
