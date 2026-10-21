#include "core.hpp"

#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vmspoof;

namespace {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void usage() {
    std::cout <<
        "vmspoof - VMware VM Spoof Patcher (C++)\n\n"
        "Uso:\n"
        "  vmspoof patch-vmx --profile <nombre|ruta> --in <exe> --out <exe> [opciones]\n"
        "  vmspoof patch-efi --profile <nombre|ruta> --in <rom> --out <rom> [opciones]\n"
        "  vmspoof patch-all --profile <nombre|ruta> [--in-exe <exe>] [--in-efi <rom>]\n"
        "                    [--out-dir <dir>] [--vmx <ruta.vmx>] [opciones]\n"
        "  vmspoof verify    --profile <nombre|ruta> --vmx-exe <exe> --efi <rom> [--json-report <json>]\n"
        "  vmspoof restore   --profile <nombre|ruta> [--in-exe <exe>] [--in-efi <rom>]\n"
        "                    [--vmx <ruta.vmx>] [--dry-run] [--json-report <json>]\n"
        "  vmspoof manifest-diff --original <exe> --patched <exe> --out <json> [--force]\n\n"
        "Opciones:\n"
        "  --force              permite un build desconocido y sobreescribe salidas\n"
        "  --install            instala en la carpeta de VMware (backup + reemplazo atómico)\n"
        "  --vmx <ruta>         copia el ROM a la VM y configura efi64.filename\n"
        "  --svga-id VID:DID    IDs hexadecimales personalizados para la GPU (ej. 1002:1B80)\n"
        "  --ahci-id VID:DID    IDs hexadecimales personalizados para AHCI (ej. 1B4B:9215)\n"
        "  --json-report <ruta> escribe un reporte JSON con offsets y hashes\n"
        "  --dry-run            valida y calcula, pero no escribe nada\n"
        "  --help               muestra esta ayuda\n";
}

struct Args {
    std::wstring command;
    std::map<std::wstring, std::wstring> values;
    std::set<std::wstring> flags;
};

bool is_value_option(const std::wstring& name) {
    static const std::set<std::wstring> names = {
        L"--profile", L"--in", L"--out", L"--vmx", L"--in-exe", L"--in-efi",
        L"--out-dir", L"--json-report", L"--original", L"--patched", L"--vmx-exe", L"--efi",
        L"--svga-id", L"--ahci-id"};
    return names.count(name) != 0;
}

Args parse_args(int argc, wchar_t** argv) {
    Args a;
    if (argc < 2) return a;
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg.rfind(L"--", 0) != 0) throw std::runtime_error("argumento inesperado: " + to_utf8(arg));
        if (is_value_option(arg)) {
            if (i + 1 >= argc) throw std::runtime_error("falta el valor de " + to_utf8(arg));
            a.values[arg] = argv[++i];
        } else {
            a.flags.insert(arg);
        }
    }
    return a;
}

std::wstring require(const Args& a, const wchar_t* key) {
    auto it = a.values.find(key);
    if (it == a.values.end()) throw std::runtime_error("falta " + to_utf8(key));
    return it->second;
}

std::wstring optional(const Args& a, const wchar_t* key) {
    auto it = a.values.find(key);
    return it == a.values.end() ? std::wstring() : it->second;
}

std::pair<uint16_t, uint16_t> parse_id(const std::wstring& text) {
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos)
        throw std::runtime_error("ID inválido (usa VID:DID, p.ej. 10DE:1001)");
    const std::wstring left = text.substr(0, colon);
    const std::wstring right = text.substr(colon + 1);
    wchar_t* end = nullptr;
    const unsigned long vid = std::wcstoul(left.c_str(), &end, 16);
    if (!end || *end || vid > 0xFFFF) throw std::runtime_error("VID inválido");
    const unsigned long did = std::wcstoul(right.c_str(), &end, 16);
    if (!end || *end || did > 0xFFFF) throw std::runtime_error("DID inválido");
    return {uint16_t(vid), uint16_t(did)};
}

Options make_options(const Args& a) {
    Options o;
    o.force = a.flags.count(L"--force") != 0;
    o.dry_run = a.flags.count(L"--dry-run") != 0;
    o.install = a.flags.count(L"--install") != 0;
    if (a.values.count(L"--vmx")) o.vmx = fs::path(optional(a, L"--vmx"));
    if (a.values.count(L"--json-report")) o.report = fs::path(optional(a, L"--json-report"));
    if (a.values.count(L"--svga-id")) o.svga_id = parse_id(optional(a, L"--svga-id"));
    if (a.values.count(L"--ahci-id")) o.ahci_id = parse_id(optional(a, L"--ahci-id"));
    return o;
}

void check_options(const Args& a, const std::set<std::wstring>& values,
                   const std::set<std::wstring>& flags) {
    for (const auto& [key, value] : a.values) {
        (void)value;
        if (!values.count(key)) throw std::runtime_error("opción no válida: " + to_utf8(key));
    }
    for (const auto& flag : a.flags) {
        if (!flags.count(flag)) throw std::runtime_error("opción no válida: " + to_utf8(flag));
    }
}

void print_summary(const Result& r) {
    if (!r.ok) {
        std::cerr << "error: " << r.error << "\n";
        return;
    }
    std::cout << "OK";
    if (!r.report.out_sha256.empty()) std::cout << "  SHA-256: " << r.report.out_sha256;
    std::cout << "\n";
}

int run(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Args a = parse_args(argc, argv);
    if (a.command.empty() || a.command == L"--help" || a.command == L"help" ||
        a.flags.count(L"--help")) {
        usage();
        return a.command.empty() ? 2 : 0;
    }
    if (a.command == L"--version") {
        std::cout << "vmspoof 1.0\n";
        return 0;
    }

    const Log log = [](std::string_view line) {
        std::cout << line << "\n";
        std::cout.flush();
    };

    if (a.command == L"patch-vmx" || a.command == L"patch-efi") {
        check_options(a, {L"--profile", L"--in", L"--out", L"--json-report", L"--svga-id",
                          L"--ahci-id"},
                      {L"--force", L"--dry-run", L"--install"});
        Profile profile = resolve_profile(to_utf8(require(a, L"--profile")));
        Options o = make_options(a);
        o.in = fs::path(require(a, L"--in"));
        o.out = fs::path(require(a, L"--out"));
        Result r = (a.command == L"patch-vmx") ? patch_vmx(profile, o, log) : patch_efi(profile, o, log);
        print_summary(r);
        return r.ok ? 0 : 1;
    }
    if (a.command == L"patch-all") {
        check_options(a, {L"--profile", L"--in-exe", L"--in-efi", L"--out-dir", L"--vmx",
                          L"--json-report", L"--svga-id", L"--ahci-id"},
                      {L"--force", L"--dry-run", L"--install"});
        Profile profile = resolve_profile(to_utf8(require(a, L"--profile")));
        Options o = make_options(a);
        if (a.values.count(L"--in-exe")) o.in = fs::path(optional(a, L"--in-exe"));
        if (a.values.count(L"--in-efi")) o.in_efi = fs::path(optional(a, L"--in-efi"));
        if (a.values.count(L"--out-dir")) o.out_dir = fs::path(optional(a, L"--out-dir"));
        Result r = patch_all(profile, o, log);
        print_summary(r);
        return r.ok ? 0 : 1;
    }
    if (a.command == L"verify") {
        check_options(a, {L"--profile", L"--vmx-exe", L"--efi", L"--json-report", L"--svga-id",
                          L"--ahci-id"},
                      {});
        Profile profile = resolve_profile(to_utf8(require(a, L"--profile")));
        Options o = make_options(a);
        o.in = fs::path(require(a, L"--vmx-exe"));
        o.out = fs::path(require(a, L"--efi"));
        Result r = verify(profile, o, log);
        print_summary(r);
        return r.ok ? 0 : 1;
    }
    if (a.command == L"restore") {
        check_options(a, {L"--profile", L"--in-exe", L"--in-efi", L"--vmx", L"--json-report"},
                      {L"--dry-run"});
        Profile profile = resolve_profile(to_utf8(require(a, L"--profile")));
        Options o = make_options(a);
        if (a.values.count(L"--in-exe")) o.in = fs::path(optional(a, L"--in-exe"));
        if (a.values.count(L"--in-efi")) o.in_efi = fs::path(optional(a, L"--in-efi"));
        Result r = restore_vmware(profile, o, log);
        print_summary(r);
        return r.ok ? 0 : 1;
    }
    if (a.command == L"manifest-diff") {
        check_options(a, {L"--original", L"--patched", L"--out"}, {L"--force"});
        Result r = manifest_diff(fs::path(require(a, L"--original")), fs::path(require(a, L"--patched")),
                                 fs::path(require(a, L"--out")), a.flags.count(L"--force") != 0, log);
        print_summary(r);
        return r.ok ? 0 : 1;
    }

    throw std::runtime_error("comando desconocido: " + to_utf8(a.command));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n\n";
        usage();
        return 2;
    }
}
