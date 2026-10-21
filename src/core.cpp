#include "core.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <variant>

#include "LzmaLib.h"

namespace vmspoof {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string to_utf8(const fs::path& p) { return to_utf8(p.wstring()); }

std::wstring from_utf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

uint64_t parse_hex_u64(std::string_view text, std::string_view what) {
    std::string s = trim(text);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty()) throw std::runtime_error(std::string(what) + ": valor hex vacío");
    unsigned long long v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc() || ptr != s.data() + s.size())
        throw std::runtime_error(std::string(what) + ": hex inválido '" + std::string(text) + "'");
    return static_cast<uint64_t>(v);
}

std::string hex_u64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

void log_line(const Log& log, const std::string& s) {
    if (log) log(s);
}

std::string read_text_file(const fs::path& path) {
    auto bytes = read_file(path);
    size_t start = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;
    return std::string(bytes.begin() + start, bytes.end());
}

std::string now_stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

fs::path exe_dir() {
    std::wstring buf(1024, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0) return fs::current_path();
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void write_le32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

size_t find_unique(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle,
                   std::string_view what) {
    if (needle.empty()) fail(std::string(what) + ": patrón vacío");
    size_t found = std::string::npos;
    int count = 0;
    auto it = hay.begin();
    while (true) {
        it = std::search(it, hay.end(), needle.begin(), needle.end());
        if (it == hay.end()) break;
        ++count;
        found = size_t(it - hay.begin());
        ++it;
        if (count > 1) break;
    }
    if (count != 1)
        fail(std::string(what) + ": se esperaba 1 coincidencia, encontradas " + std::to_string(count));
    return found;
}

// Localiza un "sub/cmp eax, imm32" por opcode y cola, sin depender del inmediato
// (que es justamente lo que se parchea). Devuelve offset del inmediato y su valor.
std::pair<size_t, uint32_t> find_immediate(const std::vector<uint8_t>& hay,
                                           const std::vector<uint8_t>& prefix,
                                           const std::vector<uint8_t>& tail, std::string_view what) {
    size_t found = std::string::npos;
    uint32_t value = 0;
    int count = 0;
    for (size_t i = 0; i + prefix.size() + 4 + tail.size() <= hay.size(); ++i) {
        if (!std::equal(prefix.begin(), prefix.end(), hay.begin() + std::ptrdiff_t(i))) continue;
        const size_t tail_pos = i + prefix.size() + 4;
        if (!std::equal(tail.begin(), tail.end(), hay.begin() + std::ptrdiff_t(tail_pos))) continue;
        ++count;
        found = i + prefix.size();
        if (count > 1) break;
    }
    if (count != 1)
        fail(std::string(what) + ": se esperaba 1 coincidencia, encontradas " + std::to_string(count));
    value = read_le32(hay.data() + found);
    return {found, value};
}

// ---------------------------------------------------------------------------
// Minimal JSON (objects, arrays, strings, numbers, bool, null)
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Int, Double, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_int() const { return type == Type::Int; }
    bool is_bool() const { return type == Type::Bool; }

    const Json& at(std::string_view key) const {
        if (type != Type::Object) fail("JSON: se esperaba objeto");
        auto it = obj.find(std::string(key));
        if (it == obj.end()) fail("JSON: falta la clave '" + std::string(key) + "'");
        return it->second;
    }
    bool contains(std::string_view key) const {
        return type == Type::Object && obj.find(std::string(key)) != obj.end();
    }
    const Json& at(size_t idx) const {
        if (type != Type::Array || idx >= arr.size()) fail("JSON: índice fuera de rango");
        return arr[idx];
    }
    size_t size() const { return type == Type::Array ? arr.size() : (type == Type::Object ? obj.size() : 0); }
    int64_t as_int() const {
        if (type != Type::Int) fail("JSON: se esperaba entero");
        return i;
    }
    std::string as_string() const {
        if (type != Type::String) fail("JSON: se esperaba string");
        return s;
    }
    bool as_bool() const {
        if (type != Type::Bool) fail("JSON: se esperaba booleano");
        return b;
    }
};

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back(char(cp));
    else if (cp <= 0x7FF) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    Json parse() {
        ws();
        Json v = value();
        ws();
        if (pos_ != s_.size()) err("texto extra al final");
        return v;
    }

private:
    std::string_view s_;
    size_t pos_ = 0;

    [[noreturn]] void err(const std::string& msg) {
        fail("JSON: " + msg + " (offset " + std::to_string(pos_) + ")");
    }
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
            ++pos_;
    }
    char peek() {
        if (pos_ >= s_.size()) err("fin inesperado");
        return s_[pos_];
    }
    char get() {
        char c = peek();
        ++pos_;
        return c;
    }
    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!consume(c)) err(std::string("se esperaba '") + c + "'");
    }

    Json value() {
        switch (peek()) {
        case '{': return object();
        case '[': return array();
        case '"': {
            Json j;
            j.type = Json::Type::String;
            j.s = string();
            return j;
        }
        case 't': literal("true"); { Json j; j.type = Json::Type::Bool; j.b = true; return j; }
        case 'f': literal("false"); { Json j; j.type = Json::Type::Bool; j.b = false; return j; }
        case 'n': literal("null"); return {};
        default: return number();
        }
    }

    void literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) err("literal inválido");
        pos_ += lit.size();
    }

    Json object() {
        Json j;
        j.type = Json::Type::Object;
        expect('{');
        ws();
        if (consume('}')) return j;
        for (;;) {
            ws();
            if (peek() != '"') err("se esperaba clave string");
            std::string key = string();
            ws();
            expect(':');
            ws();
            j.obj.emplace(std::move(key), value());
            ws();
            if (consume('}')) break;
            expect(',');
        }
        return j;
    }

    Json array() {
        Json j;
        j.type = Json::Type::Array;
        expect('[');
        ws();
        if (consume(']')) return j;
        for (;;) {
            ws();
            j.arr.push_back(value());
            ws();
            if (consume(']')) break;
            expect(',');
        }
        return j;
    }

    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) err("string sin cerrar");
            char c = get();
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            char e = get();
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = get();
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
                    else err("\\u inválido");
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                    pos_ += 2;
                    uint32_t lo = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = get();
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= uint32_t(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= uint32_t(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= uint32_t(h - 'A' + 10);
                        else err("\\u inválido");
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: err("escape inválido");
            }
        }
        return out;
    }

    Json number() {
        size_t start = pos_;
        if (consume('-')) {}
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        bool floating = false;
        if (consume('.')) {
            floating = true;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            floating = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ == start) err("número inválido");
        std::string tok(s_.substr(start, pos_ - start));
        Json j;
        if (floating) {
            j.type = Json::Type::Double;
            j.d = std::strtod(tok.c_str(), nullptr);
        } else {
            int64_t v = 0;
            auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v, 10);
            if (ec != std::errc() || ptr != tok.data() + tok.size()) err("entero inválido");
            j.type = Json::Type::Int;
            j.i = v;
        }
        return j;
    }
};

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(char(c));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Profile loading
// ---------------------------------------------------------------------------

EfiDriver parse_driver(const Json& j, std::string_view what) {
    EfiDriver d;
    uint64_t vid = parse_hex_u64(j.at("vid").as_string(), std::string(what) + ".vid");
    uint64_t did = parse_hex_u64(j.at("did").as_string(), std::string(what) + ".did");
    if (vid > 0xFFFF || did > 0xFFFF) fail(std::string(what) + ": vendor/device deben ser 16 bits");
    d.vid = uint16_t(vid);
    d.did = uint16_t(did);
    d.patch = j.at("patch").as_bool();
    return d;
}

bool has_cat(const Profile& p, std::string_view cat) {
    for (const auto& patch : p.patches)
        if (iequals(patch.cat, cat)) return true;
    return false;
}

void validate_profile(const Profile& p) {
    if (p.name.empty()) fail("perfil: 'name' vacío");
    if (p.exe_size == 0) fail("perfil: vmware_vmx.size inválido");
    if (p.patches.empty()) fail("perfil: no hay patches de vmware-vmx");
    if (p.exe_sha256.size() != 64) fail("perfil: original_sha256 inválido");
    if (!p.exe_expected_sha256.empty() && p.exe_expected_sha256.size() != 64)
        fail("perfil: expected_sha256 inválido");
    if (p.efi_size == 0) fail("perfil: efi64.size inválido");
    if (p.efi_sha256.size() != 64) fail("perfil: efi64.original_sha256 inválido");

    for (const auto& patch : p.patches) {
        if (patch.cat.empty()) fail("perfil: patch sin 'cat' en " + hex_u64(patch.off));
        if (patch.orig.empty()) fail("perfil: patch sin 'orig' en " + hex_u64(patch.off));
        if (patch.orig.size() != patch.repl.size())
            fail("perfil: orig/new de distinto tamaño en " + hex_u64(patch.off));
        if (patch.off > p.exe_size || patch.orig.size() > p.exe_size - patch.off)
            fail("perfil: patch fuera del ejecutable en " + hex_u64(patch.off));
    }

    const bool svga_patches = has_cat(p, "svga");
    const bool ahci_patches = has_cat(p, "ahci");
    if (p.svga.patch != svga_patches)
        fail("perfil incoherente: efi64.svga.patch=" + std::string(p.svga.patch ? "true" : "false") +
             " pero patches svga=" + std::string(svga_patches ? "sí" : "no"));
    if (p.ahci.patch != ahci_patches)
        fail("perfil incoherente: efi64.ahci.patch=" + std::string(p.ahci.patch ? "true" : "false") +
             " pero patches ahci=" + std::string(ahci_patches ? "sí" : "no"));
}

Profile profile_from_json(const Json& root, const fs::path& source) {
    Profile p;
    p.source = source;
    p.name = root.at("name").as_string();

    const Json& vmx = root.at("vmware_vmx");
    if (!vmx.contains("size") || !vmx.contains("original_sha256") || !vmx.contains("patches"))
        fail("perfil: faltan claves en vmware_vmx");
    p.exe_size = uint64_t(vmx.at("size").as_int());
    p.exe_sha256 = lower_ascii(vmx.at("original_sha256").as_string());
    if (vmx.contains("expected_sha256"))
        p.exe_expected_sha256 = lower_ascii(vmx.at("expected_sha256").as_string());

    for (const Json& pj : vmx.at("patches").arr) {
        Patch patch;
        patch.off = parse_hex_u64(pj.at("off").as_string(), "patch.off");
        patch.orig = unhex(pj.at("orig").as_string(), "patch.orig");
        patch.repl = unhex(pj.at("new").as_string(), "patch.new");
        patch.base = patch.repl;
        patch.cat = pj.at("cat").as_string();
        p.patches.push_back(std::move(patch));
    }

    const Json& efi = root.at("efi64");
    p.efi_size = uint64_t(efi.at("size").as_int());
    p.efi_sha256 = lower_ascii(efi.at("original_sha256").as_string());
    p.efi_build = efi.at("build").as_string();
    p.svga = parse_driver(efi.at("svga"), "efi64.svga");
    p.ahci = parse_driver(efi.at("ahci"), "efi64.ahci");

    validate_profile(p);
    return p;
}

// ---------------------------------------------------------------------------
// VMX/EFI implementation helpers
// ---------------------------------------------------------------------------

const std::array<uint8_t, 16> kLzmaGuid = {0x98, 0x58, 0x4E, 0xEE, 0x14, 0x39, 0x59, 0x42,
                                            0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF};
const std::array<uint8_t, 10> kSvgaPattern = {0x2D, 0xAD, 0x15, 0x05, 0x04,
                                              0xA9, 0xFF, 0xFF, 0xFE, 0xFF};
const std::array<uint8_t, 7> kAhciPattern = {0x81, 0x7D, 0xD4, 0xAD, 0x15, 0xE0, 0x07};

std::vector<uint8_t> as_vec(const std::array<uint8_t, 16>& a) { return {a.begin(), a.end()}; }

void gate_input(const std::vector<uint8_t>& data, uint64_t expected_size,
                const std::string& expected_sha, bool force, std::string_view what, const Log& log) {
    if (data.size() != expected_size)
        fail(std::string(what) + ": tamaño inesperado " + std::to_string(data.size()) +
             " (esperado " + std::to_string(expected_size) + ")");
    std::string sha = sha256_hex(data);
    if (!force && sha != expected_sha) {
        fail(std::string(what) + ": SHA-256 no coincide\n  esperado: " + expected_sha +
             "\n  obtenido: " + sha + "\nUsa --force solo si sabes que el build es compatible.");
    }
    if (force && sha != expected_sha)
        log_line(log, "AVISO: hash del original no coincide; --force activo");
    log_line(log, std::string(what) + ": " + std::to_string(data.size()) + " bytes, SHA-256 " + sha);
}

void configure_vmx(const fs::path& vmx_path, const fs::path& rom_path, bool dry_run, const Log& log) {
    if (rom_path.empty()) fail("VMX: falta el ROM de salida");
    std::error_code ec;
    if (!fs::exists(vmx_path, ec)) fail("VMX: no existe " + to_utf8(vmx_path));

    std::string text = read_text_file(vmx_path);    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";

    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();

    const std::string value = to_utf8(rom_path.filename());
    const std::string new_line = "efi64.filename = \"" + value + "\"";
    bool found = false;
    for (auto& line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (iequals(trim(std::string_view(line).substr(0, eq)), "efi64.filename")) {
            line = new_line;
            found = true;
        }
    }
    if (!found) lines.push_back(new_line);

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || crlf) out += eol;
    }
    if (!out.empty() && out.back() != '\n') out += eol;

    if (dry_run) {
        log_line(log, "VMX (dry-run): " + to_utf8(vmx_path) + " -> " + new_line +
                          "; se copiaría " + to_utf8(rom_path.filename()));
        return;
    }

    const fs::path rom_dst = vmx_path.parent_path() / rom_path.filename();
    if (!fs::equivalent(rom_path, rom_dst, ec)) {
        std::error_code ec2;
        if (fs::exists(rom_dst, ec2)) {
            const fs::path old_backup = rom_dst.string() + ".bak-" + now_stamp();
            if (!CopyFileW(rom_dst.c_str(), old_backup.c_str(), FALSE))
                fail("VMX: no se pudo respaldar el ROM anterior " + to_utf8(rom_dst));
            log_line(log, "Backup ROM anterior: " + to_utf8(old_backup));
        }
        fs::copy_file(rom_path, rom_dst, fs::copy_options::overwrite_existing, ec2);
        if (ec2) fail("VMX: no se pudo copiar el ROM a " + to_utf8(rom_dst) + ": " + ec2.message());
    }
    const fs::path backup = vmx_path.string() + ".bak-" + now_stamp();
    if (!CopyFileW(vmx_path.c_str(), backup.c_str(), FALSE))
        fail("VMX: no se pudo crear el backup " + to_utf8(backup));
    std::vector<uint8_t> bytes(out.begin(), out.end());
    write_file_atomic(vmx_path, bytes, true);
    log_line(log, "VMX actualizado: " + to_utf8(vmx_path) + " (" + new_line + ")");
    log_line(log, "ROM copiado a " + to_utf8(rom_dst));
}

void save_original_stash(const fs::path& source, const fs::path& target,
                         const std::string& expected_sha, const Log& log) {
    if (expected_sha.empty()) return;
    try {
        auto data = read_file(source);
        if (sha256_hex(data) != expected_sha) return;
        const fs::path stash_dir = target.parent_path() / "vmspoof-original";
        const fs::path stash = stash_dir / target.filename();
        std::error_code ec;
        if (fs::exists(stash, ec) && sha256_hex(read_file(stash)) == expected_sha) return;
        write_file_atomic(stash, data, true);
        log_line(log, "Original guardado para restaurar: " + to_utf8(stash));
    } catch (const std::exception& e) {
        log_line(log, "AVISO: no se pudo guardar el original para restaurar: " + std::string(e.what()));
    }
}

void install_one(const fs::path& source, const fs::path& target, const Log& log) {
    if (source.empty() || target.empty()) fail("install: rutas vacías");
    if (!fs::exists(source)) fail("install: no existe " + to_utf8(source));
    if (!fs::exists(target))
        fail("install: no existe " + to_utf8(target) + " (¿VMware Workstation está instalado?)");
    std::error_code equiv_ec;
    if (fs::equivalent(source, target, equiv_ec)) {
        log_line(log, "install: el archivo ya está instalado (" + to_utf8(target) + ")");
        return;
    }

    if (fs::exists(target)) {
        HANDLE h = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_SHARING_VIOLATION)
                fail("install: " + to_utf8(target) + " está en uso (¿hay una VM corriendo?)");
            if (e == ERROR_ACCESS_DENIED)
                fail("install: acceso denegado a " + to_utf8(target));
            if (e != ERROR_FILE_NOT_FOUND)
                fail("install: no se pudo abrir " + to_utf8(target) + " (error " + std::to_string(e) + ")");
        } else {
            CloseHandle(h);
        }
    }

    DWORD attrs = GetFileAttributesW(target.c_str());
    const bool readonly = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY);
    const fs::path bdir = [&] {
        const fs::path base = target.parent_path() / ("backup-" + now_stamp());
        std::error_code extra_ec;
        if (!fs::exists(base, extra_ec)) return base;
        for (int i = 1; i < 1000; ++i) {
            const fs::path candidate = base.string() + "-" + std::to_string(i);
            if (!fs::exists(candidate, extra_ec)) return candidate;
        }
        fail("install: no se pudo generar un nombre de backup único");
    }();
    std::error_code ec;
    fs::create_directories(bdir, ec);
    if (ec) fail("install: no se pudo crear backup en " + to_utf8(bdir) + ": " + ec.message());
    const fs::path bfile = bdir / target.filename();
    if (!CopyFileW(target.c_str(), bfile.c_str(), FALSE))
        fail("install: no se pudo respaldar " + to_utf8(target));
    log_line(log, "Backup: " + to_utf8(bfile));

    if (readonly) SetFileAttributesW(target.c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_READONLY));

    try {
        write_file_atomic(target, read_file(source), true);
        if (readonly) SetFileAttributesW(target.c_str(), attrs);
    } catch (...) {
        if (readonly) SetFileAttributesW(target.c_str(), attrs);
        throw;
    }
    log_line(log, "Instalado: " + to_utf8(target) + " (afecta a todas las VMs)");
}

std::string file_sha(const fs::path& path) {
    try {
        if (!fs::exists(path)) return {};
        return sha256_hex(read_file(path));
    } catch (...) {
        return {};
    }
}

fs::path find_original_file(const fs::path& explicit_path, const fs::path& target,
                            const std::string& expected_sha, const wchar_t* env_name,
                            const Log& log) {
    std::vector<fs::path> candidates;
    if (!explicit_path.empty()) candidates.push_back(explicit_path);
    candidates.push_back(target.parent_path() / "vmspoof-original" / target.filename());

    std::vector<std::pair<fs::file_time_type, fs::path>> backups;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(target.parent_path(), ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("backup-", 0) != 0) continue;
        const fs::path f = entry.path() / target.filename();
        if (!fs::exists(f, ec)) continue;
        std::error_code tec;
        backups.push_back({fs::last_write_time(f, tec), f});
    }
    std::sort(backups.begin(), backups.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& b : backups) candidates.push_back(b.second);

    if (const wchar_t* env = _wgetenv(env_name)) {
        if (*env) candidates.push_back(fs::path(env));
    }
    const std::vector<fs::path> roots = {
        (exe_dir() / "..").lexically_normal(),
        fs::current_path(),
        (fs::current_path() / "..").lexically_normal()};
    for (const auto& root : roots) candidates.push_back(root / "back up" / target.filename());

    for (const auto& candidate : candidates) {
        if (file_sha(candidate) == expected_sha) {
            log_line(log, "Origen original: " + to_utf8(candidate));
            return candidate;
        }
    }
    fail("restore: no se encontró un " + target.filename().string() +
         " original válido. Guarda una copia original, crea un backup, o indícalo con --in-exe/--in-efi.");
}

void unconfigure_vmx(const fs::path& vmx_path, const Log& log) {
    std::error_code ec;
    if (!fs::exists(vmx_path, ec)) fail("VMX: no existe " + to_utf8(vmx_path));
    std::string text = read_text_file(vmx_path);
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();

    bool changed = false;
    std::vector<std::string> kept;
    for (auto& line : lines) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos &&
            iequals(trim(std::string_view(line).substr(0, eq)), "efi64.filename")) {
            changed = true;
            continue;
        }
        kept.push_back(line);
    }
    if (!changed) {
        log_line(log, "VMX: no tenía efi64.filename; no se modificó.");
        return;
    }
    std::string out;
    for (size_t i = 0; i < kept.size(); ++i) {
        out += kept[i];
        if (i + 1 < kept.size() || crlf) out += eol;
    }
    if (!out.empty() && out.back() != '\n') out += eol;
    const fs::path backup = vmx_path.string() + ".bak-" + now_stamp();
    if (!CopyFileW(vmx_path.c_str(), backup.c_str(), FALSE))
        fail("VMX: no se pudo crear el backup " + to_utf8(backup));
    std::vector<uint8_t> bytes(out.begin(), out.end());
    write_file_atomic(vmx_path, bytes, true);
    log_line(log, "VMX restaurado: " + to_utf8(vmx_path) + " (efi64.filename eliminado)");
}

void restore_file(const fs::path& source, const fs::path& target, const Log& log) {
    if (fs::exists(target)) {
        install_one(source, target, log);
    } else {
        write_file_atomic(target, read_file(source), true);
        log_line(log, "Restaurado: " + to_utf8(target));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public utilities
// ---------------------------------------------------------------------------

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("no se pudo abrir " + to_utf8(path));
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) fail("no se pudo medir " + to_utf8(path));
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(data.data()), n);
    if (!f) fail("error leyendo " + to_utf8(path));
    return data;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        fail("BCrypt: no se pudo abrir SHA-256");
    DWORD obj_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb, 0);
    std::vector<uint8_t> obj(obj_len);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        fail("BCrypt: no se pudo crear el hash");
    }
    if (!data.empty()) BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0);
    std::array<uint8_t, 32> digest{};
    NTSTATUS st = BCryptFinishHash(hash, digest.data(), DWORD(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) fail("BCrypt: no se pudo finalizar el hash");
    return hex(digest.data(), digest.size());
}

std::string hex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    return out;
}

std::string hex(const std::vector<uint8_t>& data) { return hex(data.data(), data.size()); }

std::vector<uint8_t> unhex(std::string_view text, std::string_view what) {
    if (text.size() % 2 != 0) fail(std::string(what) + ": hex de longitud impar");
    auto val = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        fail(std::string(what) + ": carácter hex inválido");
    };
    std::vector<uint8_t> out(text.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = uint8_t((val(text[i * 2]) << 4) | val(text[i * 2 + 1]));
    return out;
}

void write_file_atomic(const fs::path& out, const std::vector<uint8_t>& data, bool force) {
    std::error_code ec;
    if (!force && fs::exists(out, ec)) fail("la salida ya existe: " + to_utf8(out) + " (usa --force)");
    if (!out.parent_path().empty()) {
        fs::create_directories(out.parent_path(), ec);
        if (ec) fail("no se pudo crear la carpeta " + to_utf8(out.parent_path()) + ": " + ec.message());
    }
    const fs::path tmp = out.string() + ".tmp" + std::to_string(GetCurrentProcessId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) fail("no se pudo crear " + to_utf8(tmp));
        if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code rm;
            fs::remove(tmp, rm);
            fail("error escribiendo " + to_utf8(tmp));
        }
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | (force ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(tmp.c_str(), out.c_str(), flags)) {
        DWORD e = GetLastError();
        std::error_code rm;
        fs::remove(tmp, rm);
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
            fail("la salida ya existe: " + to_utf8(out) + " (usa --force)");
        if (e == ERROR_ACCESS_DENIED)
            fail("permiso denegado al escribir " + to_utf8(out) + " (ejecuta como administrador)");
        fail("no se pudo mover " + to_utf8(tmp) + " a " + to_utf8(out) + " (error " + std::to_string(e) + ")");
    }
}

LzmaSection find_lzma_section(const std::vector<uint8_t>& rom) {
    const std::vector<uint8_t> guid = as_vec(kLzmaGuid);
    auto it = rom.begin();
    while (true) {
        it = std::search(it, rom.end(), guid.begin(), guid.end());
        if (it == rom.end()) break;
        const size_t guid_pos = size_t(it - rom.begin());
        if (guid_pos >= 4) {
            const size_t sec = guid_pos - 4;
            const uint64_t size = uint64_t(rom[sec]) | (uint64_t(rom[sec + 1]) << 8) | (uint64_t(rom[sec + 2]) << 16);
            if (sec + 20 + 2 <= rom.size() && rom[sec + 3] == 0x02 && size >= 0x18 &&
                sec + size <= rom.size() && sec + size >= sec) {
                const uint16_t data_off = uint16_t(rom[sec + 20]) | (uint16_t(rom[sec + 21]) << 8);
                if (data_off == 0x18 && sec + data_off < rom.size() && rom[sec + data_off] == 0x5D) {
                    LzmaSection s;
                    s.section_off = sec;
                    s.section_size = size;
                    s.data_off = data_off;
                    s.capacity = size - data_off;
                    return s;
                }
            }
        }
        it += 1;
    }
    fail("EFI: no se encontró una sección LZMA válida (¿EFI32/ROM no soportada?)");
}

std::vector<uint8_t> lzma_decompress_alone(const uint8_t* src, size_t src_len, size_t out_size) {
    if (src_len < 13) fail("LZMA: stream demasiado corto");
    std::vector<uint8_t> out(out_size);
    size_t dest_len = out_size;
    size_t in_len = src_len - 13;
    int res = LzmaUncompress(out.data(), &dest_len, src + 13, &in_len, src, 5);
    if (res != SZ_OK)
        fail("LZMA: error de descompresión (" + std::to_string(res) + ")");
    if (dest_len != out_size)
        fail("LZMA: tamaño descomprimido " + std::to_string(dest_len) + " (esperado " +
             std::to_string(out_size) + ")");
    return out;
}

std::vector<uint8_t> lzma_compress_alone(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> payload(input.size() + input.size() / 3 + 65536);
    size_t payload_len = payload.size();
    std::array<unsigned char, LZMA_PROPS_SIZE> props{};
    size_t props_len = props.size();
    int res = LzmaCompress(payload.data(), &payload_len, input.data(), input.size(), props.data(), &props_len,
                           9, 1u << 24, 3, 0, 2, 64, 1);
    if (res != SZ_OK)
        fail("LZMA: error de compresión (" + std::to_string(res) + ")");
    if (props_len != LZMA_PROPS_SIZE || props[0] != 0x5D)
        fail("LZMA: propiedades inesperadas");
    std::vector<uint8_t> out;
    out.reserve(13 + payload_len);
    out.insert(out.end(), props.begin(), props.end());
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t((uint64_t(input.size()) >> (i * 8)) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.begin() + std::ptrdiff_t(payload_len));
    return out;
}

std::vector<Range> diff_ranges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) fail("diff: tamaños distintos");
    std::vector<Range> out;
    size_t i = 0;
    while (i < a.size()) {
        if (a[i] == b[i]) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < a.size() && a[i] != b[i]) ++i;
        out.push_back({start, i - start});
    }
    return out;
}

bool diffs_within(const std::vector<Range>& diffs, const std::vector<Range>& expected) {
    for (const auto& d : diffs) {
        bool inside = false;
        for (const auto& e : expected) {
            if (d.off >= e.off && d.off + d.len <= e.off + e.len) {
                inside = true;
                break;
            }
        }
        if (!inside) return false;
    }
    return true;
}

Profile effective_profile(const Profile& base, const Options& options, const Log& log) {
    Profile p = base;
    if (!options.svga_id && !options.ahci_id) return p;

    if (options.svga_id) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < p.patches.size(); ++i)
            if (iequals(p.patches[i].cat, "svga") && p.patches[i].repl.size() == 2) idx.push_back(i);
        if (idx.size() != 2)
            fail("IDs personalizados: el perfil no tiene exactamente 2 patches svga de 2 bytes");
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b) { return p.patches[a].off < p.patches[b].off; });
        const uint16_t vid = options.svga_id->first;
        const uint16_t did = options.svga_id->second;
        p.patches[idx[0]].repl = {uint8_t(vid), uint8_t(vid >> 8)};
        p.patches[idx[1]].repl = {uint8_t(did), uint8_t(did >> 8)};
        p.svga = {vid, did, true};
        log_line(log, "IDs SVGA personalizados: " + hex_u64(vid | (uint32_t(did) << 16)));
    }
    if (options.ahci_id) {
        const uint16_t vid = options.ahci_id->first;
        const uint16_t did = options.ahci_id->second;
        const uint32_t value = uint32_t(vid) | (uint32_t(did) << 16);
        bool any = false;
        for (auto& patch : p.patches) {
            if (!iequals(patch.cat, "ahci")) continue;
            if (patch.repl.size() != 4)
                fail("IDs personalizados: patch ahci de tamaño inesperado en " + hex_u64(patch.off));
            patch.repl = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
            any = true;
        }
        if (!any) fail("IDs personalizados: este perfil no parchea AHCI (no hay entradas cat=ahci)");
        p.ahci = {vid, did, true};
        log_line(log, "IDs AHCI personalizados: " + hex_u64(value));
    }
    p.exe_expected_sha256.clear();
    return p;
}

void apply_vmx_patches(const Profile& profile, std::vector<uint8_t>& data, Report& report) {
    for (const auto& patch : profile.patches) {
        AppliedPatch ap;
        ap.off = patch.off;
        ap.orig = hex(patch.orig);
        ap.repl = hex(patch.repl);
        ap.cat = patch.cat;
        if (patch.off > data.size() || patch.orig.size() > data.size() - patch.off)
            fail("patch " + hex_u64(patch.off) + " (" + patch.cat + "): fuera del archivo");
        const uint8_t* cur = data.data() + patch.off;
        const std::vector<uint8_t>& base = patch.base.empty() ? patch.repl : patch.base;
        const bool is_target = std::memcmp(cur, patch.repl.data(), patch.repl.size()) == 0;
        const bool is_base = std::memcmp(cur, base.data(), base.size()) == 0;
        const bool is_orig = std::memcmp(cur, patch.orig.data(), patch.orig.size()) == 0;
        if (!is_target && !is_base && !is_orig)
            fail("patch " + hex_u64(patch.off) + " (" + patch.cat + "): bytes inesperados\n" +
                 "  original: " + hex(patch.orig) + "\n  perfil:   " + hex(base) +
                 "\n  obtenido: " + hex(cur, patch.orig.size()));
        if (!is_target) std::memcpy(data.data() + patch.off, patch.repl.data(), patch.repl.size());
        ap.already = is_target;
        ap.ok = true;
        report.patches.push_back(std::move(ap));
    }
    if (!report.patches.empty() &&
        std::all_of(report.patches.begin(), report.patches.end(),
                    [](const AppliedPatch& p) { return p.already; })) {
        report.warnings.push_back("El .exe ya tenía todos los patches aplicados; no se modificó.");
    }
}

std::pair<size_t, uint32_t> find_ahci_immediate(const std::vector<uint8_t>& dxe,
                                                const std::vector<uint32_t>& known,
                                                std::string_view what) {
    std::vector<std::pair<size_t, uint32_t>> matches;
    std::vector<uint32_t> all;
    for (size_t i = 0; i + 7 <= dxe.size(); ++i) {
        if (dxe[i] != 0x81 || dxe[i + 1] != 0x7D || dxe[i + 2] != 0xD4) continue;
        const uint32_t value = read_le32(dxe.data() + i + 3);
        all.push_back(value);
        if (std::find(known.begin(), known.end(), value) != known.end()) matches.push_back({i + 3, value});
    }
    if (matches.size() != 1) {
        std::string values;
        for (uint32_t v : all) values += " " + hex_u64(v);
        fail(std::string(what) + ": no se pudo identificar el inmediato AHCI (candidatos:" + values + ")");
    }
    return matches[0];
}

// ---------------------------------------------------------------------------
// Profile resolution
// ---------------------------------------------------------------------------

Profile load_profile(const fs::path& path) {
    Json root = JsonParser(read_text_file(path)).parse();
    if (!root.is_object()) fail("perfil: raíz JSON inválida");
    return profile_from_json(root, path);
}

Profile resolve_profile(const std::string& name_or_path) {
    std::error_code ec;
    std::u8string u8name(reinterpret_cast<const char8_t*>(name_or_path.data()), name_or_path.size());
    fs::path direct(u8name);
    if (fs::exists(direct, ec) && fs::is_regular_file(direct, ec)) return load_profile(direct);

    std::vector<fs::path> dirs;
    if (const wchar_t* env = _wgetenv(L"VMSPOOF_PROFILES")) {
        std::wstring all(env);
        size_t start = 0;
        while (start <= all.size()) {
            size_t end = all.find(L';', start);
            std::wstring piece = all.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!piece.empty()) dirs.emplace_back(piece);
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    // repo de desarrollo: build\vmspoof.exe + ..\profiles (tiene prioridad sobre la copia)
    dirs.push_back((exe_dir() / ".." / "profiles").lexically_normal());
    dirs.push_back(exe_dir() / "profiles");
    dirs.push_back(fs::current_path() / "profiles");
    dirs.push_back((fs::current_path() / ".." / "profiles").lexically_normal());

    std::vector<fs::path> unique_dirs;
    for (const auto& dir : dirs) {
        const fs::path norm = dir.lexically_normal();
        bool seen = false;
        for (const auto& d : unique_dirs)
            if (d == norm) { seen = true; break; }
        if (!seen) unique_dirs.push_back(norm);
    }

    std::string searched;
    std::string first_error;
    for (const auto& dir : unique_dirs) {
        searched += " " + to_utf8(dir);
        if (!fs::exists(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            try {
                Profile p = load_profile(entry.path());
                if (iequals(p.name, name_or_path) || iequals(entry.path().stem().string(), name_or_path))
                    return p;
            } catch (const std::exception& e) {
                if (first_error.empty()) first_error = e.what();
            }
        }
    }
    fail("perfil no encontrado: '" + name_or_path + "' (buscado en:" + searched +
         (first_error.empty() ? ")" : "; error en un JSON: " + first_error + ")"));
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

fs::path default_vmware_dir() {
    if (const wchar_t* env = _wgetenv(L"VMSPOOF_VMWARE_DIR")) {
        if (*env) return fs::path(env);
    }
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\VMware, Inc.\\VMware Workstation",
                     L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        if (buf[0]) return fs::path(buf);
    }
    return fs::path(L"C:\\Program Files (x86)\\VMware\\VMware Workstation");
}

fs::path vmware_bin_dir() {
    const fs::path dir = default_vmware_dir();
    std::error_code ec;
    const fs::path x64 = dir / "x64";
    if (fs::exists(x64, ec)) return x64;
    return dir;
}

fs::path default_exe_path() { return vmware_bin_dir() / "vmware-vmx.exe"; }

fs::path default_efi_path() { return vmware_bin_dir() / "EFI64.ROM"; }

fs::path default_out_dir() {
    std::error_code ec;
    const fs::path exe = exe_dir();
    // repo de desarrollo: build\vmspoof.exe + ..\profiles -> dist en la raíz
    if (fs::exists((exe / ".." / "profiles").lexically_normal(), ec))
        return (exe / ".." / "dist").lexically_normal();
    if (fs::exists(exe / "profiles", ec)) return exe / "dist";
    return fs::current_path() / "dist";
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

std::string Report::to_json() const {
    std::ostringstream o;
    o << "{";
    o << "\"command\":\"" << json_escape(command) << "\",";
    o << "\"profile\":\"" << json_escape(profile) << "\",";
    o << "\"ok\":" << (ok ? "true" : "false") << ",";
    o << "\"error\":\"" << json_escape(error) << "\",";
    o << "\"in_path\":\"" << json_escape(in_path) << "\",";
    o << "\"out_path\":\"" << json_escape(out_path) << "\",";
    o << "\"in_size\":" << in_size << ",";
    o << "\"out_size\":" << out_size << ",";
    o << "\"in_sha256\":\"" << json_escape(in_sha256) << "\",";
    o << "\"out_sha256\":\"" << json_escape(out_sha256) << "\",";
    o << "\"expected_sha256\":\"" << json_escape(expected_sha256) << "\",";
    o << "\"patches\":[";
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& p = patches[i];
        if (i) o << ",";
        o << "{\"off\":\"" << hex_u64(p.off) << "\",\"orig\":\"" << json_escape(p.orig)
          << "\",\"new\":\"" << json_escape(p.repl)           << "\",\"cat\":\"" << json_escape(p.cat)
          << "\",\"ok\":" << (p.ok ? "true" : "false")
          << ",\"already\":" << (p.already ? "true" : "false") << "}";
    }
    o << "],";
    o << "\"efi\":" << (efi ? "true" : "false") << ",";
    o << "\"lzma\":{\"section\":" << section_off << ",\"data\":" << data_off
      << ",\"capacity\":" << capacity << ",\"compressed\":" << compressed << "},";
    o << "\"efi_diffs\":[";
    for (size_t i = 0; i < efi_diffs.size(); ++i) {
        if (i) o << ",";
        o << "{\"off\":" << efi_diffs[i].off << ",\"len\":" << efi_diffs[i].len << "}";
    }
    o << "],";
    o << "\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i) o << ",";
        o << "\"" << json_escape(warnings[i]) << "\"";
    }
    o << "]}";
    return o.str();
}

namespace {

void write_report_if_requested(const fs::path& path, const std::string& json) {
    if (path.empty()) return;
    std::vector<uint8_t> bytes(json.begin(), json.end());
    write_file_atomic(path, bytes, true);
}

Result run_guarded(const std::string& command, const Profile& profile, const Log& log,
                   const std::function<void(Report&)>& body) {
    Result result;
    result.report.command = command;
    result.report.profile = profile.name;
    try {
        body(result.report);
        result.report.ok = true;
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
        result.report.ok = false;
        result.report.error = result.error;
        log_line(log, "ERROR: " + result.error);
    }
    return result;
}

std::vector<uint8_t> pattern_vec(const auto& arr) { return {arr.begin(), arr.end()}; }

} // namespace

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Result patch_vmx(const Profile& profile_in, const Options& options, const Log& log) {
    return run_guarded("patch-vmx", profile_in, log, [&](Report& r) {
        const Profile profile = effective_profile(profile_in, options, log);
        r.in_path = to_utf8(options.in);
        r.out_path = to_utf8(options.out);
        auto data = read_file(options.in);
        r.in_size = data.size();
        r.in_sha256 = sha256_hex(data);
        if (data.size() != profile.exe_size)
            fail("vmware-vmx.exe: tamaño inesperado " + std::to_string(data.size()) + " (esperado " +
                 std::to_string(profile.exe_size) + ")");

        const bool matches_orig = r.in_sha256 == profile.exe_sha256;
        const bool matches_expected =
            !profile.exe_expected_sha256.empty() && r.in_sha256 == profile.exe_expected_sha256;
        bool all_target = true;
        bool all_known = true;
        for (const auto& patch : profile.patches) {
            if (patch.off > data.size() || patch.orig.size() > data.size() - patch.off) {
                all_target = all_known = false;
                break;
            }
            const uint8_t* cur = data.data() + patch.off;
            const auto& base = patch.base.empty() ? patch.repl : patch.base;
            all_target = all_target &&
                         std::memcmp(cur, patch.repl.data(), patch.repl.size()) == 0;
            const bool known = std::memcmp(cur, patch.orig.data(), patch.orig.size()) == 0 ||
                               std::memcmp(cur, base.data(), base.size()) == 0 ||
                               std::memcmp(cur, patch.repl.data(), patch.repl.size()) == 0;
            all_known = all_known && known;
        }

        const bool has_overrides = options.svga_id.has_value() || options.ahci_id.has_value();
        const bool allow_patched = matches_expected ||
                                   (profile.exe_expected_sha256.empty() && all_target) ||
                                   (has_overrides && all_known);
        if (!matches_orig && !allow_patched && !options.force)
            fail("vmware-vmx.exe: SHA-256 no coincide\n  esperado: " + profile.exe_sha256 +
                 "\n  obtenido: " + r.in_sha256 +
                 "\nParece un build distinto o un archivo ya parcheado con otro perfil."
                 "\nUsa el original de 'back up' o --force si el build es compatible.");
        if (!matches_orig && !matches_expected) {
            r.warnings.push_back("La entrada no es el original; se reutiliza como base ya parcheada.");
            log_line(log, "AVISO: entrada no original; se valida contra los valores conocidos del perfil.");
        } else if (matches_expected) {
            r.warnings.push_back("El .exe ya estaba parcheado con este perfil; se reutiliza.");
            log_line(log, "AVISO: el .exe ya está parcheado; se reutiliza sin modificar.");
        }

        apply_vmx_patches(profile, data, r);
        r.out_size = data.size();
        r.out_sha256 = sha256_hex(data);
        r.expected_sha256 = profile.exe_expected_sha256;

        if (!profile.exe_expected_sha256.empty() && r.out_sha256 != profile.exe_expected_sha256)
            fail("hash final inesperado\n  esperado: " + profile.exe_expected_sha256 +
                 "\n  obtenido: " + r.out_sha256);

        if (!options.dry_run) {
            write_file_atomic(options.out, data, options.force);
            log_line(log, "Escrito: " + to_utf8(options.out));
            if (options.install) {
                save_original_stash(options.in, default_exe_path(), profile.exe_sha256, log);
                install_one(options.out, default_exe_path(), log);
            }
            r.ok = true;
            write_report_if_requested(options.report, r.to_json());
        } else {
            log_line(log, "dry-run: no se escribió " + to_utf8(options.out));
        }
        log_line(log, "SHA-256 resultado: " + r.out_sha256);
    });
}

Result patch_efi(const Profile& profile_in, const Options& options, const Log& log) {
    return run_guarded("patch-efi", profile_in, log, [&](Report& r) {
        const Profile profile = effective_profile(profile_in, options, log);
        r.in_path = to_utf8(options.in);
        r.out_path = to_utf8(options.out);
        r.efi = true;
        auto rom = read_file(options.in);
        r.in_size = rom.size();
        r.in_sha256 = sha256_hex(rom);
        if (rom.size() != profile.efi_size)
            fail("EFI no soportada: " + std::to_string(rom.size()) + " bytes; se esperaba " +
                 std::to_string(profile.efi_size) + " (EFI64 de este build; EFI32 no está implementado)");

        const LzmaSection sec = find_lzma_section(rom);
        r.section_off = sec.section_off;
        r.data_off = sec.data_off;
        r.capacity = sec.capacity;
        log_line(log, "Sección LZMA: offset " + hex_u64(sec.section_off) + ", datos " +
                          hex_u64(sec.section_off + sec.data_off) + ", capacidad " + hex_u64(sec.capacity));

        const size_t start = size_t(sec.section_off + sec.data_off);
        auto dxe = lzma_decompress_alone(rom.data() + start, size_t(sec.capacity), size_t(kDxeSize));
        const auto current_dxe = dxe;
        log_line(log, "DXE descomprimido: " + std::to_string(dxe.size()) + " bytes");

        const auto [svga_off, svga_current] = find_immediate(
            dxe, {0x2D}, {0xA9, 0xFF, 0xFF, 0xFE, 0xFF}, "VmwSvgaDxe Supported()");
        std::vector<uint32_t> known_ahci = {0x07E015ADu};
        if (profile_in.ahci.patch) known_ahci.push_back(profile_in.ahci.value());
        if (profile.ahci.patch) known_ahci.push_back(profile.ahci.value());
        const auto [ahci_off, ahci_current] =
            find_ahci_immediate(dxe, known_ahci, "VmwAhciDxe Supported()");

        if (!profile.svga.patch && svga_current != 0x040515ADu)
            fail("EFI: VmwSvgaDxe ya está parcheado pero el perfil no habilita SVGA");
        if (!profile.ahci.patch && ahci_current != 0x07E015ADu)
            fail("EFI: VmwAhciDxe ya está parcheado pero el perfil no habilita AHCI "
                 "(incoherencia con el .exe)");

        const uint32_t target_svga = profile.svga.patch ? profile.svga.value() : svga_current;
        const uint32_t target_ahci = profile.ahci.patch ? profile.ahci.value() : ahci_current;
        log_line(log, "SVGA actual: " + hex_u64(svga_current) + " -> " + hex_u64(target_svga));
        log_line(log, "AHCI actual: " + hex_u64(ahci_current) + " -> " + hex_u64(target_ahci));

        const bool already = svga_current == target_svga && ahci_current == target_ahci;
        const bool matches_orig = r.in_sha256 == profile.efi_sha256;
        const bool has_overrides = options.svga_id.has_value() || options.ahci_id.has_value();
        const bool known_state =
            (svga_current == 0x040515ADu || svga_current == profile_in.svga.value()) &&
            (ahci_current == 0x07E015ADu || ahci_current == profile_in.ahci.value());

        if (!matches_orig && !already && !options.force && !(has_overrides && known_state))
            fail("EFI64.ROM: SHA-256 no coincide y el DXE no está en un estado conocido\n"
                 "  esperado: " + profile.efi_sha256 + "\n  obtenido: " + r.in_sha256 +
                 "\nUsa el original de 'back up' o --force si el build es compatible.");

        if (already) {
            log_line(log, "AVISO: el EFI ya tiene los IDs del perfil; se reutiliza sin recomprimir.");
            r.warnings.push_back("El EFI ya tenía los IDs del perfil; no se recomprimió.");
            r.compressed = 0;
            r.out_size = rom.size();
            r.out_sha256 = r.in_sha256;
            if (!options.dry_run) {
                write_file_atomic(options.out, rom, options.force);
                log_line(log, "Escrito: " + to_utf8(options.out));
                if (options.install) {
                    save_original_stash(options.in, default_efi_path(), profile.efi_sha256, log);
                    install_one(options.out, default_efi_path(), log);
                }
                r.ok = true;
                write_report_if_requested(options.report, r.to_json());
            } else {
                log_line(log, "dry-run: no se escribió " + to_utf8(options.out));
            }
            return;
        }

        std::vector<Range> expected;
        if (profile.svga.patch && svga_current != target_svga) {
            write_le32(dxe.data() + svga_off, target_svga);
            expected.push_back({svga_off, 4});
            log_line(log, "SVGA parcheado: " + hex_u64(target_svga) + " en DXE " + hex_u64(svga_off));
        }
        if (profile.ahci.patch && ahci_current != target_ahci) {
            write_le32(dxe.data() + ahci_off, target_ahci);
            expected.push_back({ahci_off, 4});
            log_line(log, "AHCI parcheado: " + hex_u64(target_ahci) + " en DXE " + hex_u64(ahci_off));
        }
        std::sort(expected.begin(), expected.end(),
                  [](const Range& a, const Range& b) { return a.off < b.off; });

        auto compressed = lzma_compress_alone(dxe);
        r.compressed = compressed.size();
        log_line(log, "LZMA recomprimido: " + std::to_string(compressed.size()) + " / " +
                          std::to_string(sec.capacity) + " bytes");
        if (compressed.size() > sec.capacity)
            fail("LZMA comprimido (" + std::to_string(compressed.size()) + ") excede la capacidad (" +
                 std::to_string(sec.capacity) + ")");

        auto out = rom;
        std::fill(out.begin() + std::ptrdiff_t(start),
                  out.begin() + std::ptrdiff_t(sec.section_off + sec.section_size), 0xFF);
        std::copy(compressed.begin(), compressed.end(), out.begin() + std::ptrdiff_t(start));

        auto check = lzma_decompress_alone(out.data() + start, size_t(sec.capacity), size_t(kDxeSize));
        if (check != dxe) fail("verificación: el DXE recomprimido no coincide");
        auto diffs = diff_ranges(current_dxe, check);
        r.efi_diffs = diffs;
        if (!diffs_within(diffs, expected)) {
            std::string got;
            for (const auto& d : diffs) got += " " + hex_u64(d.off) + "+" + std::to_string(d.len);
            fail("verificación: diffs EFI fuera de los offsets esperados:" + got);
        }
        r.out_size = out.size();
        r.out_sha256 = sha256_hex(out);

        if (!options.dry_run) {
            write_file_atomic(options.out, out, options.force);
            log_line(log, "Escrito: " + to_utf8(options.out));
            if (options.install) {
                save_original_stash(options.in, default_efi_path(), profile.efi_sha256, log);
                install_one(options.out, default_efi_path(), log);
            }
            r.ok = true;
            write_report_if_requested(options.report, r.to_json());
        } else {
            log_line(log, "dry-run: no se escribió " + to_utf8(options.out));
        }
        log_line(log, "SHA-256 resultado: " + r.out_sha256);
    });
}

Result patch_all(const Profile& profile, const Options& options, const Log& log) {
    return run_guarded("patch-all", profile, log, [&](Report& r) {
        const fs::path in_exe = options.in.empty() ? default_exe_path() : options.in;
        const fs::path in_efi = options.in_efi.empty() ? default_efi_path() : options.in_efi;
        const fs::path out_dir = options.out_dir.empty() ? default_out_dir() : options.out_dir;
        const fs::path out_exe = out_dir / "vmware-vmx.patched.exe";
        const fs::path out_efi = out_dir / "EFI64.patched";

        Options exe_opts = options;
        exe_opts.in = in_exe;
        exe_opts.out = out_exe;
        exe_opts.report.clear();
        Options efi_opts = options;
        efi_opts.in = in_efi;
        efi_opts.out = out_efi;
        efi_opts.report.clear();

        log_line(log, "== vmware-vmx.exe ==");
        Result r1 = patch_vmx(profile, exe_opts, log);
        if (!r1.ok) fail("vmware-vmx.exe: " + r1.error);
        log_line(log, "== EFI64.ROM ==");
        Result r2 = patch_efi(profile, efi_opts, log);
        if (!r2.ok) fail("EFI64.ROM: " + r2.error);

        if (!options.vmx.empty()) {
            log_line(log, "== VMX ==");
            configure_vmx(options.vmx, out_efi, options.dry_run, log);
        }

        r = r2.report;
        r.command = "patch-all";
        r.in_path = to_utf8(in_exe);
        r.out_path = to_utf8(out_dir);
        r.warnings = r1.report.warnings;
        r.warnings.insert(r.warnings.end(), r2.report.warnings.begin(), r2.report.warnings.end());

        if (!options.report.empty() && !options.dry_run) {
            std::string json = "{\"command\":\"patch-all\",\"profile\":\"" + json_escape(profile.name) +
                               "\",\"ok\":" + (r1.ok && r2.ok ? "true" : "false") + ",\"vmx\":" +
                               r1.report.to_json() + ",\"efi\":" + r2.report.to_json() + "}";
            write_report_if_requested(options.report, json);
        }
        const std::string prefix = options.dry_run ? "dry-run, se habría generado: " : "Listo. ";
        log_line(log, prefix + ".exe: " + to_utf8(out_exe));
        log_line(log, prefix + "EFI: " + to_utf8(out_efi));
    });
}

Result verify(const Profile& profile_in, const Options& options, const Log& log) {
    return run_guarded("verify", profile_in, log, [&](Report& r) {
        const Profile profile = effective_profile(profile_in, options, log);
        r.in_path = to_utf8(options.in);
        r.out_path = to_utf8(options.out);
        auto exe = read_file(options.in);
        r.in_size = exe.size();
        r.in_sha256 = sha256_hex(exe);
        log_line(log, "vmware-vmx.exe: " + std::to_string(exe.size()) + " bytes, SHA-256 " + r.in_sha256);

        if (exe.size() != profile.exe_size)
            fail("verify: tamaño de .exe inesperado");
        if (!profile.exe_expected_sha256.empty()) {
            r.expected_sha256 = profile.exe_expected_sha256;
            if (r.in_sha256 != profile.exe_expected_sha256)
                fail("verify: SHA-256 no coincide con el esperado del perfil");
        }
        for (const auto& patch : profile.patches) {
            if (patch.off > exe.size() || patch.repl.size() > exe.size() - patch.off)
                fail("verify: patch fuera del archivo");
            if (std::memcmp(exe.data() + patch.off, patch.repl.data(), patch.repl.size()) != 0)
                fail("verify: patch no aplicado en " + hex_u64(patch.off) + " (" + patch.cat + ")");
            AppliedPatch ap;
            ap.off = patch.off;
            ap.orig = hex(patch.repl);
            ap.repl = hex(patch.repl);
            ap.cat = patch.cat;
            ap.ok = true;
            r.patches.push_back(ap);
        }
        log_line(log, "vmware-vmx.exe: OK (" + std::to_string(profile.patches.size()) + " patches)");

        auto rom = read_file(options.out);
        r.out_size = rom.size();
        r.out_sha256 = sha256_hex(rom);
        r.efi = true;
        if (rom.size() != profile.efi_size)
            fail("verify: EFI no soportada (" + std::to_string(rom.size()) + " bytes; esperado " +
                 std::to_string(profile.efi_size) + ")");
        const LzmaSection sec = find_lzma_section(rom);
        r.section_off = sec.section_off;
        r.data_off = sec.data_off;
        r.capacity = sec.capacity;
        auto dxe = lzma_decompress_alone(rom.data() + sec.section_off + sec.data_off,
                                         size_t(sec.capacity), size_t(kDxeSize));

        const auto [svga_imm, svga_value] = find_immediate(
            dxe, {0x2D}, {0xA9, 0xFF, 0xFF, 0xFE, 0xFF}, "VmwSvgaDxe Supported()");
        (void)svga_imm;
        const uint32_t svga_expected = profile.svga.patch ? profile.svga.value() : 0x040515ADu;
        if (svga_value != svga_expected)
            fail("verify: SVGA " + hex_u64(svga_value) + " (esperado " + hex_u64(svga_expected) + ")");
        log_line(log, "SVGA: " + hex_u64(svga_value) + (profile.svga.patch ? " (parcheado)" : " (original)"));

        const uint32_t ahci_expected = profile.ahci.patch ? profile.ahci.value() : 0x07E015ADu;
        std::vector<uint32_t> ahci_values;
        for (size_t i = 0; i + 7 <= dxe.size(); ++i) {
            if (dxe[i] == 0x81 && dxe[i + 1] == 0x7D && dxe[i + 2] == 0xD4)
                ahci_values.push_back(read_le32(dxe.data() + i + 3));
        }
        const size_t ahci_matches =
            size_t(std::count(ahci_values.begin(), ahci_values.end(), ahci_expected));
        if (ahci_matches != 1) {
            std::string values;
            for (uint32_t v : ahci_values) values += " " + hex_u64(v);
            fail("verify: AHCI esperado " + hex_u64(ahci_expected) + ", candidatos:" + values);
        }
        const uint32_t ahci_value = ahci_expected;
        log_line(log, "AHCI: " + hex_u64(ahci_value) + (profile.ahci.patch ? " (parcheado)" : " (original)"));
        log_line(log, "EFI64.ROM: OK");
        r.ok = true;
        write_report_if_requested(options.report, r.to_json());
    });
}

Result restore_vmware(const Profile& profile, const Options& options, const Log& log) {
    return run_guarded("restore", profile, log, [&](Report& r) {
        const fs::path target_exe = default_exe_path();
        const fs::path target_efi = default_efi_path();
        r.in_path = to_utf8(target_exe.parent_path());
        r.out_path = r.in_path;

        const bool exe_ok = file_sha(target_exe) == profile.exe_sha256;
        const bool efi_ok = file_sha(target_efi) == profile.efi_sha256;
        fs::path src_exe;
        fs::path src_efi;
        if (!exe_ok)
            src_exe = find_original_file(options.in, target_exe, profile.exe_sha256,
                                         L"VMSPOOF_VMX_ORIG", log);
        if (!efi_ok)
            src_efi = find_original_file(options.in_efi, target_efi, profile.efi_sha256,
                                         L"VMSPOOF_EFI_ORIG", log);

        if (options.dry_run) {
            if (!exe_ok) log_line(log, "dry-run: restauraría " + to_utf8(target_exe) + " desde " + to_utf8(src_exe));
            if (!efi_ok) log_line(log, "dry-run: restauraría " + to_utf8(target_efi) + " desde " + to_utf8(src_efi));
            if (!options.vmx.empty()) log_line(log, "dry-run: quitaría efi64.filename de " + to_utf8(options.vmx));
            return;
        }

        if (exe_ok) log_line(log, "vmware-vmx.exe ya es el original; no se toca.");
        else restore_file(src_exe, target_exe, log);
        if (efi_ok) log_line(log, "EFI64.ROM ya es el original; no se toca.");
        else restore_file(src_efi, target_efi, log);

        if (!options.vmx.empty()) unconfigure_vmx(options.vmx, log);

        if (file_sha(target_exe) != profile.exe_sha256)
            fail("restore: la verificación de vmware-vmx.exe falló");
        if (file_sha(target_efi) != profile.efi_sha256)
            fail("restore: la verificación de EFI64.ROM falló");

        r.out_sha256 = profile.exe_sha256;
        r.out_size = fs::exists(target_exe) ? fs::file_size(target_exe) : 0;
        r.warnings.push_back("VMware restaurado a los binarios originales.");
        r.ok = true;
        write_report_if_requested(options.report, r.to_json());
        log_line(log, "Restauración completa. Reinicia VMware.");
    });
}

Result manifest_diff(const fs::path& original, const fs::path& patched, const fs::path& out,
                     bool force, const Log& log) {
    Result result;
    result.report.command = "manifest-diff";
    try {
        auto a = read_file(original);
        auto b = read_file(patched);
        if (a.size() != b.size()) fail("manifest-diff: los archivos tienen distinto tamaño");
        auto ranges = diff_ranges(a, b);
        result.report.in_path = to_utf8(original);
        result.report.out_path = to_utf8(patched);
        result.report.in_size = a.size();
        result.report.out_size = b.size();
        result.report.in_sha256 = sha256_hex(a);
        result.report.out_sha256 = sha256_hex(b);

        std::ostringstream o;
        o << "{\n  \"original\": \"" << json_escape(to_utf8(original)) << "\",\n";
        o << "  \"patched\": \"" << json_escape(to_utf8(patched)) << "\",\n";
        o << "  \"original_sha256\": \"" << result.report.in_sha256 << "\",\n";
        o << "  \"patched_sha256\": \"" << result.report.out_sha256 << "\",\n";
        o << "  \"size\": " << a.size() << ",\n  \"patches\": [\n";
        size_t total = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            const auto& range = ranges[i];
            total += range.len;
            o << "    {\"off\":\"" << hex_u64(range.off) << "\",\"orig\":\""
              << hex(a.data() + range.off, size_t(range.len)) << "\",\"new\":\""
              << hex(b.data() + range.off, size_t(range.len)) << "\",\"cat\":\"unknown\"}";
            o << (i + 1 < ranges.size() ? ",\n" : "\n");
        }
        o << "  ],\n  \"total_bytes\": " << total << "\n}\n";
        const std::string json = o.str();
        write_file_atomic(out, std::vector<uint8_t>(json.begin(), json.end()), force);
        result.ok = true;
        result.report.ok = true;
        log_line(log, "Manifiesto: " + to_utf8(out) + " (" + std::to_string(ranges.size()) +
                          " grupos, " + std::to_string(total) + " bytes)");
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
        result.report.error = result.error;
        log_line(log, "ERROR: " + result.error);
    }
    return result;
}

} // namespace vmspoof
