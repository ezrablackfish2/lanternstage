#include "lanternstage/diagnostics.h"

#include "lanternstage/text.h"

#include <algorithm>
#include <sstream>

namespace lanternstage {
namespace {

std::string color_prefix(Severity severity) {
  switch (severity) {
    case Severity::note: return "\033[36m";
    case Severity::warning: return "\033[33m";
    case Severity::error: return "\033[31m";
    case Severity::fatal: return "\033[1;31m";
  }
  return {};
}

std::string format_location(const SourceLocation& location) {
  std::ostringstream out;
  if (!location.source.empty()) out << location.source;
  else out << "<input>";
  out << ':' << location.line << ':' << location.column;
  return out.str();
}

std::string underline(std::string_view line, std::size_t start_column,
                      std::size_t end_column) {
  std::string out;
  const std::size_t maximum = std::max<std::size_t>(1, display_width(line));
  start_column = std::max<std::size_t>(1, std::min(start_column, maximum));
  end_column = std::max(start_column + 1, std::min(end_column, maximum + 1));
  out.append(start_column - 1, ' ');
  out.push_back('^');
  if (end_column > start_column + 1) out.append(end_column - start_column - 1, '~');
  return out;
}

}  // namespace

SourceMap::SourceMap(std::string name, std::string text)
    : name_(std::move(name)), text_(std::move(text)) {
  line_offsets_.push_back(0);
  for (std::size_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\n') line_offsets_.push_back(i + 1);
  }
}

const std::string& SourceMap::name() const { return name_; }
const std::string& SourceMap::text() const { return text_; }

SourceLocation SourceMap::locate(std::size_t offset) const {
  offset = std::min(offset, text_.size());
  auto upper = std::upper_bound(line_offsets_.begin(), line_offsets_.end(), offset);
  const std::size_t line_index = upper == line_offsets_.begin() ? 0 :
                                 static_cast<std::size_t>(upper - line_offsets_.begin() - 1);
  const std::size_t line_start = line_offsets_[line_index];
  const std::string_view prefix(text_.data() + line_start, offset - line_start);
  return {name_, offset, line_index + 1, display_width(prefix) + 1};
}

SourceRange SourceMap::range(std::size_t begin, std::size_t end) const {
  if (end < begin) std::swap(begin, end);
  return {locate(begin), locate(end)};
}

std::string_view SourceMap::line_text(std::size_t line) const {
  if (line == 0 || line > line_offsets_.size()) return {};
  const std::size_t begin = line_offsets_[line - 1];
  std::size_t end = line < line_offsets_.size() ? line_offsets_[line] : text_.size();
  while (end > begin && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) --end;
  return std::string_view(text_).substr(begin, end - begin);
}

std::size_t SourceMap::line_count() const { return line_offsets_.size(); }

DiagnosticSink::DiagnosticSink(std::size_t maximum) : maximum_(maximum) {}

void DiagnosticSink::emit(Diagnostic diagnostic) {
  if (diagnostics_.size() >= maximum_) {
    truncated_ = true;
    return;
  }
  diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::note(std::string code, std::string message,
                          std::optional<SourceRange> range) {
  emit({Severity::note, std::move(code), std::move(message), std::move(range), {}});
}

void DiagnosticSink::warning(std::string code, std::string message,
                             std::optional<SourceRange> range) {
  emit({Severity::warning, std::move(code), std::move(message), std::move(range), {}});
}

void DiagnosticSink::error(std::string code, std::string message,
                           std::optional<SourceRange> range) {
  emit({Severity::error, std::move(code), std::move(message), std::move(range), {}});
}

void DiagnosticSink::fatal(std::string code, std::string message,
                           std::optional<SourceRange> range) {
  emit({Severity::fatal, std::move(code), std::move(message), std::move(range), {}});
}

const std::vector<Diagnostic>& DiagnosticSink::diagnostics() const { return diagnostics_; }

bool DiagnosticSink::has_errors() const {
  return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic& value) {
    return value.severity == Severity::error || value.severity == Severity::fatal;
  });
}

bool DiagnosticSink::truncated() const { return truncated_; }

std::size_t DiagnosticSink::count(Severity severity) const {
  return static_cast<std::size_t>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
                                                [severity](const Diagnostic& value) {
                                                  return value.severity == severity;
                                                }));
}

void DiagnosticSink::clear() {
  diagnostics_.clear();
  truncated_ = false;
}

std::string severity_name(Severity severity) {
  switch (severity) {
    case Severity::note: return "note";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    case Severity::fatal: return "fatal";
  }
  return "unknown";
}

std::string format_diagnostic(const Diagnostic& diagnostic, const SourceMap* source, bool color) {
  std::ostringstream out;
  const std::string prefix = color ? color_prefix(diagnostic.severity) : std::string();
  const std::string reset = color ? "\033[0m" : std::string();
  if (diagnostic.range) out << format_location(diagnostic.range->begin) << ": ";
  out << prefix << severity_name(diagnostic.severity) << reset;
  if (!diagnostic.code.empty()) out << '[' << diagnostic.code << ']';
  out << ": " << diagnostic.message << '\n';
  if (source != nullptr && diagnostic.range &&
      diagnostic.range->begin.source == source->name()) {
    const std::size_t line_number = diagnostic.range->begin.line;
    const std::string_view line = source->line_text(line_number);
    out << "  " << line_number << " | " << line << '\n';
    const std::size_t end_column = diagnostic.range->end.line == line_number
                                       ? diagnostic.range->end.column
                                       : display_width(line) + 1;
    out << "    | " << prefix
        << underline(line, diagnostic.range->begin.column, end_column) << reset << '\n';
  }
  for (const DiagnosticNote& note : diagnostic.notes) {
    if (note.range) out << format_location(note.range->begin) << ": ";
    out << "note: " << note.message << '\n';
  }
  return out.str();
}

std::string diagnostics_as_json(const std::vector<Diagnostic>& diagnostics) {
  std::ostringstream out;
  out << '[';
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0) out << ',';
    const Diagnostic& diagnostic = diagnostics[i];
    out << "{\"severity\":\"" << severity_name(diagnostic.severity)
        << "\",\"code\":\"" << json_escape(diagnostic.code)
        << "\",\"message\":\"" << json_escape(diagnostic.message) << '"';
    if (diagnostic.range) {
      out << ",\"source\":\"" << json_escape(diagnostic.range->begin.source)
          << "\",\"line\":" << diagnostic.range->begin.line
          << ",\"column\":" << diagnostic.range->begin.column
          << ",\"end_line\":" << diagnostic.range->end.line
          << ",\"end_column\":" << diagnostic.range->end.column;
    }
    out << ",\"notes\":[";
    for (std::size_t n = 0; n < diagnostic.notes.size(); ++n) {
      if (n != 0) out << ',';
      out << "{\"message\":\"" << json_escape(diagnostic.notes[n].message) << "\"}";
    }
    out << "]}";
  }
  out << ']';
  return out.str();
}

Status status_from_diagnostics(const DiagnosticSink& sink) {
  if (sink.count(Severity::fatal) != 0)
    return Status::failure(Status::Code::internal_error, "fatal diagnostics were emitted");
  if (sink.count(Severity::error) != 0)
    return Status::failure(Status::Code::rejected, "validation diagnostics contain errors");
  return Status::success();
}

}  // namespace lanternstage
