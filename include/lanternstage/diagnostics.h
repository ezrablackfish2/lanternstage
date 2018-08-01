#pragma once

#include "lanternstage/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lanternstage {

enum class Severity : std::uint8_t { note = 0, warning = 1, error = 2, fatal = 3 };

struct SourceLocation {
  std::string source;
  std::size_t byte_offset{0};
  std::size_t line{1};
  std::size_t column{1};
};

struct SourceRange {
  SourceLocation begin;
  SourceLocation end;
};

struct DiagnosticNote {
  std::string message;
  std::optional<SourceRange> range;
};

struct Diagnostic {
  Severity severity{Severity::error};
  std::string code;
  std::string message;
  std::optional<SourceRange> range;
  std::vector<DiagnosticNote> notes;
};

class SourceMap {
 public:
  SourceMap(std::string name, std::string text);

  const std::string& name() const;
  const std::string& text() const;
  SourceLocation locate(std::size_t offset) const;
  SourceRange range(std::size_t begin, std::size_t end) const;
  std::string_view line_text(std::size_t line) const;
  std::size_t line_count() const;

 private:
  std::string name_;
  std::string text_;
  std::vector<std::size_t> line_offsets_;
};

class DiagnosticSink {
 public:
  explicit DiagnosticSink(std::size_t maximum = 1000);

  void emit(Diagnostic diagnostic);
  void note(std::string code, std::string message,
            std::optional<SourceRange> range = std::nullopt);
  void warning(std::string code, std::string message,
               std::optional<SourceRange> range = std::nullopt);
  void error(std::string code, std::string message,
             std::optional<SourceRange> range = std::nullopt);
  void fatal(std::string code, std::string message,
             std::optional<SourceRange> range = std::nullopt);

  const std::vector<Diagnostic>& diagnostics() const;
  bool has_errors() const;
  bool truncated() const;
  std::size_t count(Severity severity) const;
  void clear();

 private:
  std::size_t maximum_{0};
  std::vector<Diagnostic> diagnostics_;
  bool truncated_{false};
};

std::string severity_name(Severity severity);
std::string format_diagnostic(const Diagnostic& diagnostic, const SourceMap* source = nullptr,
                              bool color = false);
std::string diagnostics_as_json(const std::vector<Diagnostic>& diagnostics);
Status status_from_diagnostics(const DiagnosticSink& sink);

}  // namespace lanternstage
