#include "mq/calc.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/float_chars.h"

namespace hailstone::mq {

namespace {

// MQ's eCalcOp. Number and the parentheses take no part in precedence.
enum class Op : std::uint8_t {
    Number, Open, Close, Add, Subtract, Multiply, Divide, IntDivide, LogicalAnd, And, LogicalOr, Or, Xor, Equal,
    NotEqual, Greater, NotGreater, Less, NotLess, Modulus, Power, LogicalNot, Not, ShiftLeft, ShiftRight, Negate,
};

int precedence(Op op) {
    switch (op) {
    case Op::Number: case Op::Open: case Op::Close: return 0;
    case Op::LogicalOr: return 1;
    case Op::LogicalAnd: return 2;
    case Op::Or: return 3;
    case Op::Xor: return 4;
    case Op::And: return 5;
    case Op::Equal: case Op::NotEqual: return 6;
    case Op::Greater: case Op::NotGreater: case Op::Less: case Op::NotLess: return 7;
    case Op::ShiftLeft: case Op::ShiftRight: return 8;
    case Op::Add: case Op::Subtract: return 9;
    case Op::Multiply: case Op::Divide: case Op::IntDivide: case Op::Modulus: return 10;
    case Op::Power: return 11;
    case Op::LogicalNot: case Op::Not: case Op::Negate: return 12;
    }
    return 0;
}

struct Item {
    Op op = Op::Number;
    double value = 0;
};

// A double as a 32-bit int the way x86's truncating conversion gives it: NaN and values outside int's range are
// 0x80000000 (MQ's (int) casts compile to it; in C++ they are undefined).
std::int32_t to_int(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0)) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(d);
}

// Both stacks keep slot 0 as MQ's do: never pushed to, read as the top of an empty stack (0, or Number for the
// operator stack), and written by an operator that finds only one operand.
bool evaluate(const std::vector<Item>& list, double& result) {
    if (list.empty()) return false;
    std::vector<double> st(1, 0.0);
    std::size_t n = 0;
    auto top = [&]() -> double& { return st[n]; };
    auto push = [&](double v) {
        ++n;
        if (st.size() <= n) st.push_back(v);
        else st[n] = v;
    };
    auto pop = [&]() {
        if (n == 0) return false; // MQ: "Illegal arithmetic in calculation"
        --n;
        return true;
    };
    for (const Item& it : list) {
        if (it.op == Op::Number) {
            push(it.value);
            continue;
        }
        if (it.op == Op::Open || it.op == Op::Close) continue; // an unclosed '(' left on the stack
        if (it.op == Op::Negate) { top() = -top(); continue; }
        if (it.op == Op::LogicalNot) { top() = to_int(top()) == 0 ? 1.0 : 0.0; continue; }
        if (it.op == Op::Not) { top() = static_cast<double>(~to_int(top())); continue; }
        if (it.op == Op::Divide && top() == 0.0) return false; // MQ checks before taking the operand
        if (it.op == Op::IntDivide || it.op == Op::Modulus) {
            const std::int32_t right = to_int(top());
            if (right == 0) return false;
            if (!pop()) return false;
            const std::int32_t left = to_int(top());
            if (left == std::numeric_limits<std::int32_t>::min() && right == -1) // a divide fault in MQ
                top() = it.op == Op::IntDivide ? static_cast<double>(left) : 0.0;
            else
                top() = static_cast<double>(it.op == Op::IntDivide ? left / right : left % right);
            continue;
        }
        const double right = top();
        if (!pop()) return false;
        double& left = top();
        const std::int32_t li = to_int(left), ri = to_int(right);
        const auto u = [](std::int32_t v) { return static_cast<std::uint32_t>(v); };
        switch (it.op) {
        case Op::Add: left += right; break;
        case Op::Subtract: left -= right; break;
        case Op::Multiply: left *= right; break;
        case Op::Divide: left /= right; break;
        case Op::LogicalAnd: left = (left != 0.0 && right != 0.0) ? 1.0 : 0.0; break;
        case Op::LogicalOr: left = (left != 0.0 || right != 0.0) ? 1.0 : 0.0; break;
        case Op::Equal: left = left == right ? 1.0 : 0.0; break;
        case Op::NotEqual: left = left != right ? 1.0 : 0.0; break;
        case Op::Greater: left = left > right ? 1.0 : 0.0; break;
        case Op::NotGreater: left = left <= right ? 1.0 : 0.0; break;
        case Op::Less: left = left < right ? 1.0 : 0.0; break;
        case Op::NotLess: left = left >= right ? 1.0 : 0.0; break;
        // x86 takes a shift count modulo 32; a left shift wraps.
        case Op::ShiftLeft: left = static_cast<double>(static_cast<std::int32_t>(u(li) << (u(ri) & 31u))); break;
        case Op::ShiftRight: left = static_cast<double>(li >> (u(ri) & 31u)); break;
        case Op::And: left = static_cast<double>(li & ri); break;
        case Op::Or: left = static_cast<double>(li | ri); break;
        case Op::Xor: left = static_cast<double>(li ^ ri); break;
        case Op::Power: left = std::pow(left, right); break;
        default: break;
        }
    }
    result = top();
    return true;
}

} // namespace

std::optional<double> calculate(std::string_view formula) {
    if (const auto nul = formula.find('\0'); nul != std::string_view::npos) formula = formula.substr(0, nul);
    if (formula.size() > 2047) return std::nullopt;
    // NULL, TRUE and FALSE become numbers of the same length ("0.00", "1.00", "0.000"), as MQ rewrites them in
    // place; the rest is upper case for the search.
    std::string text(formula);
    for (char& c : text)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const auto replace_all = [&](std::string_view word, std::string_view with) {
        for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at))
            text.replace(at, word.size(), with);
    };
    replace_all("NULL", "0.00");
    replace_all("TRUE", "1.00");
    replace_all("FALSE", "0.000");

    std::vector<Item> out;
    std::vector<Op> st(1, Op::Number); // slot 0: the top of an empty stack (MQ's zeroed array)
    std::size_t n = 0;
    std::string token;
    const auto finish = [&]() {
        if (token.empty()) return;
        double v = 0;
        float_from_chars(token.data(), token.data() + token.size(), v); // a failed read leaves 0, as MQ's does
        out.push_back({Op::Number, v});
        token.clear();
    };
    const auto push = [&](Op op) {
        ++n;
        if (st.size() <= n) st.push_back(op);
        else st[n] = op;
    };
    const auto new_op = [&](Op op) {
        finish();
        while (n > 0 && st[n] != Op::Open && precedence(st[n]) >= precedence(op)) out.push_back({st[n--], 0});
        push(op);
    };
    const auto at = [&](std::size_t i) { return i < text.size() ? text[i] : '\0'; };

    bool was_paren = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = at(i + 1);
        switch (c) {
        case ' ': continue; // (keeps was_paren)
        case '(':
            finish();
            push(Op::Open);
            break;
        case ')':
            finish();
            while (st[n] != Op::Open) {
                if (n == 0) return std::nullopt; // no '(' to close
                out.push_back({st[n--], 0});
            }
            --n;
            was_paren = true;
            continue;
        case '+':
            if (next != '+') new_op(Op::Add);
            break;
        case '-':
            if (next == '-') {
                ++i;
                new_op(Op::Add);
            } else {
                new_op(!token.empty() || was_paren ? Op::Subtract : Op::Negate);
            }
            break;
        case '*': new_op(Op::Multiply); break;
        case '\\': new_op(Op::IntDivide); break;
        case '/': new_op(Op::Divide); break;
        case '%': new_op(Op::Modulus); break;
        case '~': new_op(Op::Not); break;
        case '|':
            if (next == '|') ++i;
            new_op(next == '|' ? Op::LogicalOr : Op::Or);
            break;
        case '&':
            if (next == '&') ++i;
            new_op(next == '&' ? Op::LogicalAnd : Op::And);
            break;
        case '^':
            if (next == '^') ++i;
            new_op(next == '^' ? Op::Xor : Op::Power);
            break;
        case '!':
            if (next == '=') ++i;
            new_op(next == '=' ? Op::NotEqual : Op::LogicalNot);
            break;
        case '=':
            if (next != '=') return std::nullopt;
            ++i;
            new_op(Op::Equal);
            break;
        case '<':
            if (next == '=' || next == '<') ++i;
            new_op(next == '=' ? Op::NotGreater : next == '<' ? Op::ShiftLeft : Op::Less);
            break;
        case '>':
            if (next == '=' || next == '>') ++i;
            new_op(next == '=' ? Op::NotLess : next == '>' ? Op::ShiftRight : Op::Greater);
            break;
        default:
            if ((c >= '0' && c <= '9') || c == '.') {
                token += c;
                break;
            }
            return std::nullopt; // MQ: "Unparsable in Calculation"
        }
        was_paren = false;
    }
    finish();
    while (n > 0) out.push_back({st[n--], 0});

    double result = 0;
    if (!evaluate(out, result)) return std::nullopt;
    return result;
}

namespace {

std::string_view trim_ws(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

} // namespace

int int_from_text(std::string_view text, int fail) {
    text = trim_ws(text);
    std::from_chars(text.data(), text.data() + text.size(), fail); // a failure leaves `fail`
    return fail;
}

std::int64_t int64_from_text(std::string_view text, std::int64_t fail) {
    text = trim_ws(text);
    std::from_chars(text.data(), text.data() + text.size(), fail);
    return fail;
}

double double_from_text(std::string_view text, double fail) {
    text = trim_ws(text);
    float_from_chars(text.data(), text.data() + text.size(), fail);
    return fail;
}

std::string get_arg(std::string_view text, int n, bool csv, char separator) {
    if (const auto nul = text.find('\0'); nul != std::string_view::npos) text = text.substr(0, nul);
    const bool custom = separator != 0;
    const auto is_sep = [&](char c) {
        return custom ? c == separator : (c == ' ' || c == '\t' || (csv && c == ','));
    };
    std::size_t i = 0;
    const auto skip_seps = [&] {
        while (i < text.size() && is_sep(text[i])) ++i;
    };
    // MQ's GetNextArg: past n - 1 arguments.
    skip_seps();
    bool quoted = false;
    for (int k = n - 1; k > 0; --k) {
        if (i >= text.size()) return {}; // used up: every later argument is empty (n can be near 2^31)
        while (i < text.size() && (quoted || !is_sep(text[i]))) {
            if (text[i] == '"') quoted = !quoted;
            ++i;
        }
        if (quoted) return {}; // no closing quote: an empty argument
        skip_seps();
    }
    std::string out;
    quoted = false;
    for (; i < text.size() && (quoted || !is_sep(text[i])); ++i) {
        if (text[i] == '"') quoted = !quoted;
        else out += text[i];
    }
    return out;
}

} // namespace hailstone::mq
