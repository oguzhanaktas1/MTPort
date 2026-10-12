#include "Json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace mtport {

Json& Json::operator[](const std::string& key) {
    if (isNull()) v_ = Object{};
    if (!isObject()) throw std::runtime_error("json: not an object");
    return std::get<Object>(v_)[key];
}
const Json* Json::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    const auto& o = std::get<Object>(v_);
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}
void Json::push(Json v) {
    if (isNull()) v_ = Array{};
    if (!isArray()) throw std::runtime_error("json: not an array");
    std::get<Array>(v_).push_back(std::move(v));
}
std::string Json::asString(const std::string& fb) const {
    return isString() ? std::get<std::string>(v_) : fb;
}
int64_t Json::asInt(int64_t fb) const {
    if (auto* i = std::get_if<int64_t>(&v_)) return *i;
    if (auto* d = std::get_if<double>(&v_)) return static_cast<int64_t>(*d);
    return fb;
}
double Json::asDouble(double fb) const {
    if (auto* d = std::get_if<double>(&v_)) return *d;
    if (auto* i = std::get_if<int64_t>(&v_)) return static_cast<double>(*i);
    return fb;
}
bool Json::asBool(bool fb) const {
    return isBool() ? std::get<bool>(v_) : fb;
}
const Json::Array& Json::items() const {
    static const Array empty;
    return isArray() ? std::get<Array>(v_) : empty;
}
const Json::Object& Json::members() const {
    static const Object empty;
    return isObject() ? std::get<Object>(v_) : empty;
}

namespace {
void escapeTo(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}
}  // namespace

std::string Json::dump() const {
    std::string out;
    struct V {
        std::string& out;
        void operator()(std::monostate) { out += "null"; }
        void operator()(bool b) { out += b ? "true" : "false"; }
        void operator()(int64_t i) { out += std::to_string(i); }
        void operator()(double d) {
            if (!std::isfinite(d)) {
                out += "null";
                return;
            }
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            out += buf;
        }
        void operator()(const std::string& s) { escapeTo(out, s); }
        void operator()(const Array& a) {
            out.push_back('[');
            bool first = true;
            for (const auto& e : a) {
                if (!first) out.push_back(',');
                first = false;
                out += e.dump();
            }
            out.push_back(']');
        }
        void operator()(const Object& o) {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first) out.push_back(',');
                first = false;
                escapeTo(out, k);
                out.push_back(':');
                out += v.dump();
            }
            out.push_back('}');
        }
    };
    std::visit(V{out}, v_);
    return out;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i{0};
    int depth{0};

    [[noreturn]] void fail(const char* why) { throw std::runtime_error(std::string("json: ") + why); }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!eat(c)) fail("unexpected character");
    }

    std::string str() {
        expect('"');
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) fail("bad escape");
            char e = s[i++];
            switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    if (i + 4 > s.size()) fail("bad unicode escape");
                    unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80)
                        out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    fail("bad escape");
            }
        }
        return out;
    }

    Json value() {
        if (++depth > 32) fail("nesting too deep");
        ws();
        if (i >= s.size()) fail("unexpected end");
        Json result;
        const char c = s[i];
        if (c == '{') {
            ++i;
            result = Json::object();
            if (!eat('}')) {
                do {
                    ws();
                    std::string k = str();
                    expect(':');
                    result[k] = value();
                } while (eat(','));
                expect('}');
            }
        } else if (c == '[') {
            ++i;
            result = Json::array();
            if (!eat(']')) {
                do {
                    result.push(value());
                } while (eat(','));
                expect(']');
            }
        } else if (c == '"') {
            result = Json(str());
        } else if (s.compare(i, 4, "true") == 0) {
            i += 4;
            result = Json(true);
        } else if (s.compare(i, 5, "false") == 0) {
            i += 5;
            result = Json(false);
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            const size_t start = i;
            bool isFloat = false;
            if (s[i] == '-') ++i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
                                    s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
                if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isFloat = true;
                ++i;
            }
            if (i == start) fail("unexpected token");
            const std::string num = s.substr(start, i - start);
            if (isFloat)
                result = Json(std::strtod(num.c_str(), nullptr));
            else
                result = Json(static_cast<int64_t>(std::strtoll(num.c_str(), nullptr, 10)));
        }
        --depth;
        return result;
    }
};
}  // namespace

Json Json::parse(const std::string& text, size_t maxBytes) {
    if (text.size() > maxBytes) throw std::runtime_error("json: message too large");
    Parser p{text};
    Json v = p.value();
    p.ws();
    if (p.i != text.size()) throw std::runtime_error("json: trailing data");
    return v;
}

}  // namespace mtport
