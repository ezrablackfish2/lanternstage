#include "lanternstage/query.h"

#include "lanternstage/text.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>

namespace lanternstage {
namespace {

bool is_identifier_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_identifier_continue(char c) {
  return is_identifier_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

QueryTokenKind keyword_kind(std::string_view text) {
  const std::string lower = ascii_lower(text);
  if (lower == "and") return QueryTokenKind::logical_and;
  if (lower == "or") return QueryTokenKind::logical_or;
  if (lower == "not") return QueryTokenKind::logical_not;
  if (lower == "true") return QueryTokenKind::true_literal;
  if (lower == "false") return QueryTokenKind::false_literal;
  if (lower == "null") return QueryTokenKind::null_literal;
  if (lower == "contains") return QueryTokenKind::contains;
  if (lower == "starts_with") return QueryTokenKind::starts_with;
  if (lower == "in") return QueryTokenKind::in;
  return QueryTokenKind::identifier;
}

bool truthy(const QueryValue& value) {
  if (std::holds_alternative<std::monostate>(value)) return false;
  if (const auto* integer = std::get_if<std::int64_t>(&value)) return *integer != 0;
  if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
  if (const auto* text = std::get_if<std::string>(&value)) return !text->empty();
  return false;
}

QueryValue lookup(const QueryRow& row, std::string_view name) {
  auto direct = row.find(std::string(name));
  if (direct != row.end()) return direct->second;
  const std::string lower = ascii_lower(name);
  for (const auto& [key, value] : row) {
    if (ascii_lower(key) == lower) return value;
  }
  return {};
}

std::string node_name(QueryTokenKind kind) {
  switch (kind) {
    case QueryTokenKind::equal: return "=";
    case QueryTokenKind::not_equal: return "!=";
    case QueryTokenKind::less: return "<";
    case QueryTokenKind::less_equal: return "<=";
    case QueryTokenKind::greater: return ">";
    case QueryTokenKind::greater_equal: return ">=";
    case QueryTokenKind::logical_and: return "and";
    case QueryTokenKind::logical_or: return "or";
    case QueryTokenKind::logical_not: return "not";
    case QueryTokenKind::contains: return "contains";
    case QueryTokenKind::starts_with: return "starts_with";
    case QueryTokenKind::in: return "in";
    default: return "?";
  }
}

}  // namespace

struct QueryExpression::Node {
  enum class Kind : std::uint8_t { literal, field, unary, binary, list };
  Kind kind{Kind::literal};
  QueryTokenKind op{QueryTokenKind::end};
  QueryValue literal;
  std::string field;
  std::unique_ptr<Node> left;
  std::unique_ptr<Node> right;
  std::vector<std::unique_ptr<Node>> values;

  QueryValue value(const QueryRow& row) const {
    switch (kind) {
      case Kind::literal:
        return literal;
      case Kind::field:
        return lookup(row, field);
      case Kind::unary:
        if (op == QueryTokenKind::logical_not) return !truthy(left->value(row));
        return {};
      case Kind::binary: {
        const QueryValue lhs = left->value(row);
        if (op == QueryTokenKind::logical_and && !truthy(lhs)) return false;
        if (op == QueryTokenKind::logical_or && truthy(lhs)) return true;
        const QueryValue rhs = right->value(row);
        switch (op) {
          case QueryTokenKind::equal: return compare_query_values(lhs, rhs) == 0;
          case QueryTokenKind::not_equal: return compare_query_values(lhs, rhs) != 0;
          case QueryTokenKind::less: return compare_query_values(lhs, rhs) < 0;
          case QueryTokenKind::less_equal: return compare_query_values(lhs, rhs) <= 0;
          case QueryTokenKind::greater: return compare_query_values(lhs, rhs) > 0;
          case QueryTokenKind::greater_equal: return compare_query_values(lhs, rhs) >= 0;
          case QueryTokenKind::logical_and: return truthy(lhs) && truthy(rhs);
          case QueryTokenKind::logical_or: return truthy(lhs) || truthy(rhs);
          case QueryTokenKind::contains: {
            const std::string haystack = ascii_lower(query_value_to_string(lhs));
            const std::string needle = ascii_lower(query_value_to_string(rhs));
            return haystack.find(needle) != std::string::npos;
          }
          case QueryTokenKind::starts_with: {
            const std::string haystack = ascii_lower(query_value_to_string(lhs));
            const std::string needle = ascii_lower(query_value_to_string(rhs));
            return haystack.rfind(needle, 0) == 0;
          }
          case QueryTokenKind::in:
            if (right->kind == Kind::list) {
              for (const auto& item : right->values) {
                if (compare_query_values(lhs, item->value(row)) == 0) return true;
              }
            }
            return false;
          default:
            return {};
        }
      }
      case Kind::list:
        return static_cast<std::int64_t>(values.size());
    }
    return {};
  }

  std::string explain() const {
    switch (kind) {
      case Kind::literal:
        return query_value_to_string(literal);
      case Kind::field:
        return field;
      case Kind::unary:
        return '(' + node_name(op) + ' ' + left->explain() + ')';
      case Kind::binary:
        return '(' + left->explain() + ' ' + node_name(op) + ' ' + right->explain() + ')';
      case Kind::list: {
        std::vector<std::string> parts;
        for (const auto& item : values) parts.push_back(item->explain());
        return '(' + join_strings(parts, ", ") + ')';
      }
    }
    return {};
  }
};

namespace {

class Parser {
 public:
  Parser(std::vector<QueryToken> tokens, DiagnosticSink& diagnostics)
      : tokens_(std::move(tokens)), diagnostics_(diagnostics) {}

  std::unique_ptr<QueryExpression::Node> parse() {
    auto expression = parse_or();
    if (peek().kind != QueryTokenKind::end) {
      diagnostics_.error("Q100", "unexpected token '" + peek().text + "' after expression");
    }
    return expression;
  }

 private:
  const QueryToken& peek(std::size_t distance = 0) const {
    const std::size_t index = std::min(position_ + distance, tokens_.size() - 1);
    return tokens_[index];
  }

  QueryToken consume() { return tokens_[position_++]; }

  bool match(QueryTokenKind kind) {
    if (peek().kind != kind) return false;
    ++position_;
    return true;
  }

  std::unique_ptr<QueryExpression::Node> parse_or() {
    auto left = parse_and();
    while (match(QueryTokenKind::logical_or)) {
      auto right = parse_and();
      left = binary(QueryTokenKind::logical_or, std::move(left), std::move(right));
    }
    return left;
  }

  std::unique_ptr<QueryExpression::Node> parse_and() {
    auto left = parse_not();
    while (match(QueryTokenKind::logical_and)) {
      auto right = parse_not();
      left = binary(QueryTokenKind::logical_and, std::move(left), std::move(right));
    }
    return left;
  }

  std::unique_ptr<QueryExpression::Node> parse_not() {
    if (match(QueryTokenKind::logical_not)) {
      auto node = std::make_unique<QueryExpression::Node>();
      node->kind = QueryExpression::Node::Kind::unary;
      node->op = QueryTokenKind::logical_not;
      node->left = parse_not();
      return node;
    }
    return parse_comparison();
  }

  std::unique_ptr<QueryExpression::Node> parse_comparison() {
    auto left = parse_primary();
    const QueryTokenKind kind = peek().kind;
    if (kind == QueryTokenKind::equal || kind == QueryTokenKind::not_equal ||
        kind == QueryTokenKind::less || kind == QueryTokenKind::less_equal ||
        kind == QueryTokenKind::greater || kind == QueryTokenKind::greater_equal ||
        kind == QueryTokenKind::contains || kind == QueryTokenKind::starts_with) {
      consume();
      return binary(kind, std::move(left), parse_primary());
    }
    if (match(QueryTokenKind::in)) {
      if (!match(QueryTokenKind::left_paren)) {
        diagnostics_.error("Q101", "'in' requires a parenthesized value list");
        return left;
      }
      auto list = std::make_unique<QueryExpression::Node>();
      list->kind = QueryExpression::Node::Kind::list;
      if (!match(QueryTokenKind::right_paren)) {
        do {
          list->values.push_back(parse_primary());
        } while (match(QueryTokenKind::comma));
        if (!match(QueryTokenKind::right_paren)) diagnostics_.error("Q102", "unterminated value list");
      }
      return binary(QueryTokenKind::in, std::move(left), std::move(list));
    }
    return left;
  }

  std::unique_ptr<QueryExpression::Node> parse_primary() {
    const QueryToken token = consume();
    auto node = std::make_unique<QueryExpression::Node>();
    switch (token.kind) {
      case QueryTokenKind::identifier:
        node->kind = QueryExpression::Node::Kind::field;
        node->field = token.text;
        return node;
      case QueryTokenKind::integer: {
        std::int64_t value = 0;
        auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
        if (parsed.ec != std::errc{}) diagnostics_.error("Q103", "integer literal is outside range");
        node->kind = QueryExpression::Node::Kind::literal;
        node->literal = value;
        return node;
      }
      case QueryTokenKind::string_literal:
        node->kind = QueryExpression::Node::Kind::literal;
        node->literal = token.text;
        return node;
      case QueryTokenKind::true_literal:
      case QueryTokenKind::false_literal:
        node->kind = QueryExpression::Node::Kind::literal;
        node->literal = token.kind == QueryTokenKind::true_literal;
        return node;
      case QueryTokenKind::null_literal:
        node->kind = QueryExpression::Node::Kind::literal;
        node->literal = std::monostate{};
        return node;
      case QueryTokenKind::left_paren: {
        auto nested = parse_or();
        if (!match(QueryTokenKind::right_paren)) diagnostics_.error("Q104", "missing closing parenthesis");
        return nested;
      }
      default:
        diagnostics_.error("Q105", "expected a field, literal, or parenthesized expression");
        node->kind = QueryExpression::Node::Kind::literal;
        node->literal = false;
        return node;
    }
  }

  std::unique_ptr<QueryExpression::Node> binary(
      QueryTokenKind op, std::unique_ptr<QueryExpression::Node> left,
      std::unique_ptr<QueryExpression::Node> right) {
    auto node = std::make_unique<QueryExpression::Node>();
    node->kind = QueryExpression::Node::Kind::binary;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
  }

  std::vector<QueryToken> tokens_;
  DiagnosticSink& diagnostics_;
  std::size_t position_{0};
};

}  // namespace

QueryExpression::QueryExpression() = default;
QueryExpression::~QueryExpression() = default;
QueryExpression::QueryExpression(QueryExpression&&) noexcept = default;
QueryExpression& QueryExpression::operator=(QueryExpression&&) noexcept = default;
QueryExpression::QueryExpression(std::unique_ptr<Node> root) : root_(std::move(root)) {}

bool QueryExpression::evaluate(const QueryRow& row) const {
  return root_ != nullptr && truthy(root_->value(row));
}

std::string QueryExpression::explain() const { return root_ ? root_->explain() : "<empty>"; }
QueryExpression::operator bool() const { return root_ != nullptr; }

std::vector<QueryToken> tokenize_query(std::string_view text, DiagnosticSink* diagnostics) {
  DiagnosticSink local;
  DiagnosticSink& sink = diagnostics != nullptr ? *diagnostics : local;
  std::vector<QueryToken> tokens;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const char c = text[offset];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++offset;
      continue;
    }
    if (is_identifier_start(c)) {
      const std::size_t start = offset++;
      while (offset < text.size() && is_identifier_continue(text[offset])) ++offset;
      std::string value(text.substr(start, offset - start));
      tokens.push_back({keyword_kind(value), std::move(value), start});
      continue;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const std::size_t start = offset++;
      if (c == '-' && (offset >= text.size() || text[offset] < '0' || text[offset] > '9')) {
        sink.error("Q001", "'-' must begin an integer literal");
        continue;
      }
      while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') ++offset;
      tokens.push_back({QueryTokenKind::integer, std::string(text.substr(start, offset - start)), start});
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      const std::size_t start = offset++;
      std::string value;
      bool closed = false;
      while (offset < text.size()) {
        char current = text[offset++];
        if (current == quote) {
          closed = true;
          break;
        }
        if (current == '\\' && offset < text.size()) {
          const char escaped = text[offset++];
          switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(escaped); break;
          }
        } else {
          value.push_back(current);
        }
      }
      if (!closed) sink.error("Q002", "unterminated string literal");
      tokens.push_back({QueryTokenKind::string_literal, std::move(value), start});
      continue;
    }
    const std::size_t start = offset;
    auto push = [&](QueryTokenKind kind, std::size_t length) {
      tokens.push_back({kind, std::string(text.substr(offset, length)), offset});
      offset += length;
    };
    if (c == '(') push(QueryTokenKind::left_paren, 1);
    else if (c == ')') push(QueryTokenKind::right_paren, 1);
    else if (c == ',') push(QueryTokenKind::comma, 1);
    else if (c == '=' && offset + 1 < text.size() && text[offset + 1] == '=') push(QueryTokenKind::equal, 2);
    else if (c == '=') push(QueryTokenKind::equal, 1);
    else if (c == '!' && offset + 1 < text.size() && text[offset + 1] == '=') push(QueryTokenKind::not_equal, 2);
    else if (c == '<' && offset + 1 < text.size() && text[offset + 1] == '=') push(QueryTokenKind::less_equal, 2);
    else if (c == '>' && offset + 1 < text.size() && text[offset + 1] == '=') push(QueryTokenKind::greater_equal, 2);
    else if (c == '<') push(QueryTokenKind::less, 1);
    else if (c == '>') push(QueryTokenKind::greater, 1);
    else if (c == '&' && offset + 1 < text.size() && text[offset + 1] == '&') push(QueryTokenKind::logical_and, 2);
    else if (c == '|' && offset + 1 < text.size() && text[offset + 1] == '|') push(QueryTokenKind::logical_or, 2);
    else if (c == '!') push(QueryTokenKind::logical_not, 1);
    else {
      sink.error("Q003", "unexpected character in query at offset " + std::to_string(start));
      ++offset;
    }
  }
  tokens.push_back({QueryTokenKind::end, {}, text.size()});
  return tokens;
}

Result<QueryExpression> parse_query(std::string_view text, DiagnosticSink* diagnostics) {
  DiagnosticSink local;
  DiagnosticSink& sink = diagnostics != nullptr ? *diagnostics : local;
  Result<QueryExpression> result;
  std::vector<QueryToken> tokens = tokenize_query(text, &sink);
  Parser parser(std::move(tokens), sink);
  std::unique_ptr<QueryExpression::Node> root = parser.parse();
  if (sink.has_errors()) {
    result.status = status_from_diagnostics(sink);
    return result;
  }
  result.value = QueryExpression(std::move(root));
  result.status = Status::success();
  return result;
}

std::vector<std::size_t> filter_rows(const QueryExpression& expression,
                                     const std::vector<QueryRow>& rows) {
  std::vector<std::size_t> result;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (expression.evaluate(rows[i])) result.push_back(i);
  }
  return result;
}

int compare_query_values(const QueryValue& left, const QueryValue& right) {
  if (left.index() == right.index()) {
    if (std::holds_alternative<std::monostate>(left)) return 0;
    if (const auto* a = std::get_if<std::int64_t>(&left)) {
      const auto b = std::get<std::int64_t>(right);
      return *a < b ? -1 : (*a > b ? 1 : 0);
    }
    if (const auto* a = std::get_if<bool>(&left)) {
      const auto b = std::get<bool>(right);
      return *a == b ? 0 : (*a ? 1 : -1);
    }
    const std::string a = ascii_lower(std::get<std::string>(left));
    const std::string b = ascii_lower(std::get<std::string>(right));
    return a < b ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<std::monostate>(left)) return -1;
  if (std::holds_alternative<std::monostate>(right)) return 1;
  if (const auto* integer = std::get_if<std::int64_t>(&left)) {
    if (const auto* boolean = std::get_if<bool>(&right)) {
      const std::int64_t converted = *boolean ? 1 : 0;
      return *integer < converted ? -1 : (*integer > converted ? 1 : 0);
    }
  }
  if (const auto* boolean = std::get_if<bool>(&left)) {
    if (const auto* integer = std::get_if<std::int64_t>(&right)) {
      const std::int64_t converted = *boolean ? 1 : 0;
      return converted < *integer ? -1 : (converted > *integer ? 1 : 0);
    }
  }
  const std::string a = ascii_lower(query_value_to_string(left));
  const std::string b = ascii_lower(query_value_to_string(right));
  return a < b ? -1 : (a > b ? 1 : 0);
}

std::string query_value_to_string(const QueryValue& value) {
  if (std::holds_alternative<std::monostate>(value)) return "null";
  if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
  if (const auto* boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
  return std::get<std::string>(value);
}

}  // namespace lanternstage
