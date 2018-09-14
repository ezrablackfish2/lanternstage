#pragma once

#include "lanternstage/diagnostics.h"
#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lanternstage {

using QueryValue = std::variant<std::monostate, std::int64_t, bool, std::string>;
using QueryRow = std::unordered_map<std::string, QueryValue>;

enum class QueryTokenKind : std::uint8_t {
  end,
  identifier,
  integer,
  string_literal,
  true_literal,
  false_literal,
  null_literal,
  left_paren,
  right_paren,
  comma,
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  logical_and,
  logical_or,
  logical_not,
  contains,
  starts_with,
  in,
};

struct QueryToken {
  QueryTokenKind kind{QueryTokenKind::end};
  std::string text;
  std::size_t offset{0};
};

class QueryExpression {
 public:
  struct Node;

  QueryExpression();
  ~QueryExpression();
  QueryExpression(QueryExpression&&) noexcept;
  QueryExpression& operator=(QueryExpression&&) noexcept;
  QueryExpression(const QueryExpression&) = delete;
  QueryExpression& operator=(const QueryExpression&) = delete;

  bool evaluate(const QueryRow& row) const;
  std::string explain() const;
  explicit operator bool() const;

 private:
  explicit QueryExpression(std::unique_ptr<Node> root);
  std::unique_ptr<Node> root_;
  friend Result<QueryExpression> parse_query(std::string_view, DiagnosticSink*);
};

std::vector<QueryToken> tokenize_query(std::string_view text,
                                       DiagnosticSink* diagnostics = nullptr);
Result<QueryExpression> parse_query(std::string_view text,
                                    DiagnosticSink* diagnostics = nullptr);
std::vector<std::size_t> filter_rows(const QueryExpression& expression,
                                     const std::vector<QueryRow>& rows);
int compare_query_values(const QueryValue& left, const QueryValue& right);
std::string query_value_to_string(const QueryValue& value);

}  // namespace lanternstage
