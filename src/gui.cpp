#include "gui.hpp"

#include "core.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace vmspoof;
namespace fs = std::filesystem;

namespace {

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT WM_APP_RUN = WM_APP + 3;

constexpr int ID_EXE = 1001;
constexpr int ID_EFI = 1002;
constexpr int ID_VMX = 1003;
constexpr int ID_GPU_ID = 1005;
constexpr int ID_AHCI_ID = 1006;

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Pen;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;

const Color cBg(255, 20, 22, 26);
const Color cRail(255, 26, 29, 34);
const Color cCard(255, 31, 35, 41);
const Color cEdit(255, 16, 18, 22);
const Color cBorder(255, 44, 49, 58);
const Color cText(255, 230, 233, 239);
const Color cMuted(255, 145, 155, 172);
const Color cAccent(255, 91, 141, 239);
const Color cAccentHi(255, 116, 160, 247);
const Color cAccentDim(255, 30, 44, 68);
const Color cOk(255, 63, 185, 80);
const Color cErr(255, 248, 81, 73);
const Color cWarn(255, 210, 153, 34);

enum class Action {
    None,
    Primary,
    OnlyExe,
    OnlyEfi,
    Driver,
    Verify,
    Restore,
    ProfileFull,
    ProfileGpu,
    CustomProfile,
    BrowseExe,
    BrowseEfi,
    BrowseVmx,
    ToggleForce,
    ToggleDry,
    ToggleVmx,
    ToggleCustomIds
};

enum class Status { Idle, Running, Ok, Error };
enum class Job { All, Exe, Efi, Driver, Verify, Restore };

struct Hit {
    RECT rect{};
    Action action = Action::None;
};

struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    int dpi = 96;
    HFONT fTitle = nullptr;
    HFONT fSub = nullptr;
    HFONT fBody = nullptr;
    HFONT fSmall = nullptr;
    HFONT fBold = nullptr;
    HFONT fMono = nullptr;
    HBRUSH brEdit = nullptr;
    HBRUSH brBg = nullptr;

    HWND eExe = nullptr;
    HWND eEfi = nullptr;
    HWND eVmx = nullptr;
    HWND eGpuId = nullptr;
    HWND eAhciId = nullptr;
    HWND eLog = nullptr;

    std::wstring exe, efi, vmx;
    std::wstring customProfile;

    int profile = 0; // 0 fullspoof, 1 gpu-only, -1 custom
    bool optForce = false;
    bool optDry = false;
    bool optVmx = false;
    bool optCustomIds = false;

    Status status = Status::Idle;
    std::wstring statusText = L"Listo";
    bool running = false;
    std::thread worker;

    std::vector<Hit> hits;
    Action hot = Action::None;
    Action pressed = Action::None;
    bool trackingLeave = false;

    struct Layout {
        RECT rail{}, exeCard{}, efiCard{}, vmxCard{}, logCard{};
        RECT exeEdit{}, exeBrowse{}, efiEdit{}, efiBrowse{}, vmxEdit{}, vmxBrowse{};
        RECT logEdit{};
        RECT gpuIdEdit{}, ahciIdEdit{};
        int contentX = 0, contentW = 0, optionsY = 0, customY = 0, actionsY = 0;
    } lay;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};

App* g_app = nullptr;
ULONG_PTR g_gdiplus = 0;

void update_custom_enabled(App& a);
bool parse_id(const std::wstring& text, uint16_t& vid, uint16_t& did);

std::wstring from_utf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring trim(std::wstring s) {
    auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    s = s.substr(b, e - b);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n), L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return trim(s);
}

void set_font(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, WPARAM(f), TRUE); }

Color mix(Color a, Color b, double t) {
    auto k = [&](BYTE x, BYTE y) { return BYTE(x + (int(y) - int(x)) * t); };
    return Color(255, k(a.GetR(), b.GetR()), k(a.GetG(), b.GetG()), k(a.GetB(), b.GetB()));
}

void round_rect(Graphics& g, const RECT& r, int radius, Color fill, Color border) {
    const float w = float(r.right - r.left);
    const float h = float(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;
    if (radius <= 1) {
        SolidBrush b(fill);
        g.FillRectangle(&b, float(r.left), float(r.top), w, h);
        if (border.GetA()) {
            Pen p(border, 1.0f);
            g.DrawRectangle(&p, float(r.left) + 0.5f, float(r.top) + 0.5f, w - 1, h - 1);
        }
        return;
    }
    const float d = float(radius) * 2.0f;
    GraphicsPath path;
    path.AddArc(float(r.left), float(r.top), d, d, 180, 90);
    path.AddArc(float(r.right) - d, float(r.top), d, d, 270, 90);
    path.AddArc(float(r.right) - d, float(r.bottom) - d, d, d, 0, 90);
    path.AddArc(float(r.left), float(r.bottom) - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush b(fill);
    g.FillPath(&b, &path);
    if (border.GetA()) {
        Pen p(border, 1.0f);
        g.DrawPath(&p, &path);
    }
}

void circle(Graphics& g, int cx, int cy, int radius, Color fill, Color border) {
    SolidBrush b(fill);
    g.FillEllipse(&b, float(cx - radius), float(cy - radius), float(radius * 2), float(radius * 2));
    if (border.GetA()) {
        Pen p(border, 1.0f);
        g.DrawEllipse(&p, float(cx - radius), float(cy - radius), float(radius * 2), float(radius * 2));
    }
}

void draw_text(HDC dc, const std::wstring& s, RECT r, HFONT font, Color color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, RGB(color.GetR(), color.GetG(), color.GetB()));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, flags);
    SelectObject(dc, old);
}

RECT inset(const RECT& r, int x, int y) {
    return {r.left + x, r.top + y, r.right - x, r.bottom - y};
}

void create_fonts(App& a) {
    auto make = [&](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, a.dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                           face);
    };
    if (a.fTitle) DeleteObject(a.fTitle);
    if (a.fSub) DeleteObject(a.fSub);
    if (a.fBody) DeleteObject(a.fBody);
    if (a.fSmall) DeleteObject(a.fSmall);
    if (a.fBold) DeleteObject(a.fBold);
    if (a.fMono) DeleteObject(a.fMono);
    a.fTitle = make(19, FW_SEMIBOLD, L"Segoe UI Semibold");
    a.fSub = make(10, FW_NORMAL, L"Segoe UI");
    a.fBody = make(10, FW_NORMAL, L"Segoe UI");
    a.fSmall = make(9, FW_NORMAL, L"Segoe UI");
    a.fBold = make(10, FW_SEMIBOLD, L"Segoe UI Semibold");
    a.fMono = make(9, FW_NORMAL, L"Consolas");
}

void layout(App& a, int W, int H) {
    auto& L = a.lay;
    const int margin = a.S(24);
    const int railW = a.S(220);
    const int contentX = railW + margin;
    const int contentW = std::max(a.S(300), W - contentX - margin);
    const int cardH = a.S(78);
    const int editH = a.S(30);
    const int labelOff = a.S(10);
    const int editOff = a.S(36);
    const int pad = a.S(14);
    const int browseW = a.S(84);

    L.rail = {0, 0, railW, H};
    L.contentX = contentX;
    L.contentW = contentW;

    L.exeCard = {contentX, a.S(88), contentX + contentW, a.S(88) + cardH};
    L.efiCard = {contentX, a.S(176), contentX + contentW, a.S(176) + cardH};

    const int rowY = a.S(264);
    L.vmxCard = {contentX, rowY, contentX + contentW, rowY + cardH};

    L.optionsY = a.S(352);
    L.customY = a.S(390);
    L.actionsY = a.S(438);
    L.logCard = {contentX, a.S(496), contentX + contentW, H - margin};

    auto place_edit = [&](HWND h, RECT card, RECT& edit, RECT& browse) {
        edit = {card.left + pad, card.top + editOff, card.right - pad - browseW - a.S(8),
                card.top + editOff + editH};
        browse = {card.right - pad - browseW, card.top + editOff, card.right - pad, card.top + editOff + editH};
        if (h) MoveWindow(h, edit.left, edit.top, edit.right - edit.left, edit.bottom - edit.top, TRUE);
    };
    place_edit(a.eExe, L.exeCard, L.exeEdit, L.exeBrowse);
    place_edit(a.eEfi, L.efiCard, L.efiEdit, L.efiBrowse);
    place_edit(a.eVmx, L.vmxCard, L.vmxEdit, L.vmxBrowse);

    L.logEdit = inset(L.logCard, pad, a.S(40));
    L.logEdit.bottom -= a.S(12);
    if (a.eLog)
        MoveWindow(a.eLog, L.logEdit.left, L.logEdit.top, L.logEdit.right - L.logEdit.left,
                   L.logEdit.bottom - L.logEdit.top, TRUE);

    L.gpuIdEdit = {contentX + a.S(224), L.customY, contentX + a.S(344), L.customY + a.S(30)};
    L.ahciIdEdit = {contentX + a.S(400), L.customY, contentX + a.S(520), L.customY + a.S(30)};
    if (a.eGpuId)
        MoveWindow(a.eGpuId, L.gpuIdEdit.left, L.gpuIdEdit.top, L.gpuIdEdit.right - L.gpuIdEdit.left,
                   L.gpuIdEdit.bottom - L.gpuIdEdit.top, TRUE);
    if (a.eAhciId)
        MoveWindow(a.eAhciId, L.ahciIdEdit.left, L.ahciIdEdit.top,
                   L.ahciIdEdit.right - L.ahciIdEdit.left, L.ahciIdEdit.bottom - L.ahciIdEdit.top, TRUE);
}

void add_hit(App& a, RECT r, Action action) { a.hits.push_back({r, action}); }

bool hit_test(const App& a, POINT pt, Action& action) {
    for (auto it = a.hits.rbegin(); it != a.hits.rend(); ++it) {
        if (PtInRect(&it->rect, pt)) {
            action = it->action;
            return true;
        }
    }
    action = Action::None;
    return false;
}

std::wstring open_file(HWND owner, const wchar_t* title, const wchar_t* filter) {
    wchar_t buf[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = DWORD(std::size(buf));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return buf;
}

void append_log(HWND log, const std::wstring& text) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[32];
    swprintf_s(ts, L"[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(ts) + text + L"\r\n";
    int len = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, WPARAM(len), LPARAM(len));
    SendMessageW(log, EM_REPLACESEL, FALSE, LPARAM(line.c_str()));
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

void set_status(App& a, Status s, const std::wstring& text) {
    a.status = s;
    a.statusText = text;
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

void set_running_ui(App& a, bool running) {
    a.running = running;
    EnableWindow(a.eExe, !running);
    EnableWindow(a.eEfi, !running);
    EnableWindow(a.eVmx, !running);
    update_custom_enabled(a);
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

void start_job(App& a, Job job) {
    if (a.running) return;
    a.exe = get_text(a.eExe);
    a.efi = get_text(a.eEfi);
    a.vmx = get_text(a.eVmx);

    if (job != Job::Driver && a.optVmx && a.vmx.empty()) {
        set_status(a, Status::Error, L"Falta el .vmx");
        append_log(a.eLog, L"Error: activaste 'Configurar VM' pero no indicaste el .vmx");
        return;
    }
    if (a.profile < 0 && a.customProfile.empty()) {
        set_status(a, Status::Error, L"Falta el perfil");
        append_log(a.eLog, L"Error: selecciona un perfil");
        return;
    }

    Profile profile;
    try {
        profile = a.profile < 0 ? load_profile(fs::path(a.customProfile))
                                : resolve_profile(a.profile == 0 ? "fullspoof" : "gpu-only");
    } catch (const std::exception& e) {
        set_status(a, Status::Error, L"Perfil inválido");
        append_log(a.eLog, L"Error de perfil: " + from_utf8(e.what()));
        return;
    }

    Options o;
    o.force = a.optForce;
    o.dry_run = a.optDry;
    o.install = job != Job::Verify && job != Job::Restore && job != Job::Driver;
    const fs::path stage = fs::temp_directory_path() /
        ("vmspoof-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    o.report = stage / "vmspoof-report.json";
    if (job == Job::Restore) {
        if (!a.vmx.empty()) o.vmx = fs::path(a.vmx);
    } else if (a.optVmx) {
        o.vmx = fs::path(a.vmx);
    }

    if (a.optCustomIds && job != Job::Restore) {
        uint16_t vid = 0, did = 0;
        if (!parse_id(get_text(a.eGpuId), vid, did)) {
            set_status(a, Status::Error, L"GPU ID inválido");
            append_log(a.eLog, L"Error: GPU VID:DID inválido. Usa hexadecimal, p.ej. 10DE:1001.");
            return;
        }
        o.svga_id = std::make_pair(vid, did);
        if (a.profile != 1) {
            const std::wstring ahci = get_text(a.eAhciId);
            if (!ahci.empty()) {
                if (!parse_id(ahci, vid, did)) {
                    set_status(a, Status::Error, L"AHCI ID inválido");
                    append_log(a.eLog, L"Error: AHCI VID:DID inválido. Usa hexadecimal, p.ej. 8086:A114.");
                    return;
                }
                o.ahci_id = std::make_pair(vid, did);
            }
        }
    }

    std::wstring what;
    switch (job) {
    case Job::All:
        o.in = fs::path(a.exe);
        o.in_efi = fs::path(a.efi);
        o.out_dir = stage;
        what = L"Instalar patch (exe + EFI)";
        break;
    case Job::Exe:
        o.in = fs::path(a.exe.empty() ? default_exe_path().wstring() : a.exe);
        o.out = stage / "vmware-vmx.patched.exe";
        what = L"Instalar vmware-vmx.exe";
        break;
    case Job::Efi:
        o.in = fs::path(a.efi.empty() ? default_efi_path().wstring() : a.efi);
        o.out = stage / "EFI64.patched";
        what = L"Instalar EFI64.ROM";
        break;
    case Job::Driver:
        o.out_dir = default_out_dir() / "driver";
        what = L"Generar driver";
        break;
    case Job::Verify:
        o.in = default_exe_path();
        o.out = default_efi_path();
        what = L"Verificar instalación";
        break;
    case Job::Restore:
        o.in = fs::path(a.exe);
        o.in_efi = fs::path(a.efi);
        what = L"Restaurar originales";
        break;
    }

    append_log(a.eLog, L"=== " + what + L" · perfil " + from_utf8(profile.name) + L" ===");
    if (o.install && !a.optDry)
        append_log(a.eLog, L"AVISO: se reemplazan los archivos de VMware y afecta a todas las VMs.");
    if (a.optDry) append_log(a.eLog, L"Modo simulación: no se escribirá nada.");
    if (job == Job::Restore) append_log(a.eLog, L"Objetivo: " + default_exe_path().wstring());

    set_status(a, Status::Running, L"Ejecutando…");
    set_running_ui(a, true);

    const HWND hwnd = a.hwnd;
    a.worker = std::thread([hwnd, profile, o, job] {
        const Log log = [hwnd](std::string_view s) {
            PostMessageW(hwnd, WM_APP_LOG, 0, LPARAM(new std::wstring(from_utf8(s))));
        };
        Result r;
        try {
            switch (job) {
            case Job::All: r = patch_all(profile, o, log); break;
            case Job::Exe: r = patch_vmx(profile, o, log); break;
            case Job::Efi: r = patch_efi(profile, o, log); break;
            case Job::Driver: r = patch_driver(profile, o, log); break;
            case Job::Verify: r = verify(profile, o, log); break;
            case Job::Restore: r = restore_vmware(profile, o, log); break;
            }
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
        } catch (...) {
            r.ok = false;
            r.error = "error desconocido";
        }
        std::wstring* msg = new std::wstring(
            r.ok ? L"Operación completada." : (L"Error: " + from_utf8(r.error)));
        PostMessageW(hwnd, WM_APP_DONE, r.ok ? 1 : 0, LPARAM(msg));
    });
}

fs::path gui_exe_dir() {
    wchar_t buf[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (n == 0) return fs::current_path();
    return fs::path(std::wstring(buf, n)).parent_path();
}

void configure_defaults(App& a) {
    std::error_code ec;
    const fs::path root = (gui_exe_dir() / "..").lexically_normal();
    const fs::path repo_exe = root / "back up" / "vmware-vmx.exe";
    const fs::path repo_efi = root / "back up" / "EFI64.ROM";
    a.exe = (fs::exists(repo_exe, ec) ? repo_exe : default_exe_path()).wstring();
    a.efi = (fs::exists(repo_efi, ec) ? repo_efi : default_efi_path()).wstring();
    SetWindowTextW(a.eExe, a.exe.c_str());
    SetWindowTextW(a.eEfi, a.efi.c_str());
}

void browse(App& a, Action action) {
    std::wstring value;
    switch (action) {
    case Action::BrowseExe:
        value = open_file(a.hwnd, L"Selecciona vmware-vmx.exe",
                          L"Ejecutables (*.exe)\0*.exe\0Todos (*.*)\0*.*\0\0");
        if (!value.empty()) SetWindowTextW(a.eExe, value.c_str());
        break;
    case Action::BrowseEfi:
        value = open_file(a.hwnd, L"Selecciona EFI64.ROM",
                          L"Firmware (*.ROM;*.rom)\0*.ROM;*.rom\0Todos (*.*)\0*.*\0\0");
        if (!value.empty()) SetWindowTextW(a.eEfi, value.c_str());
        break;
    case Action::BrowseVmx:
        value = open_file(a.hwnd, L"Selecciona la VM (.vmx)",
                          L"VMware (*.vmx)\0*.vmx\0Todos (*.*)\0*.*\0\0");
        if (!value.empty()) {
            SetWindowTextW(a.eVmx, value.c_str());
            a.optVmx = true;
        }
        break;
    default: break;
    }
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

bool parse_id(const std::wstring& text, uint16_t& vid, uint16_t& did) {
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos) return false;
    const std::wstring left = trim(text.substr(0, colon));
    const std::wstring right = trim(text.substr(colon + 1));
    if (left.empty() || right.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(left.c_str(), &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFF) return false;
    const unsigned long d = std::wcstoul(right.c_str(), &end, 16);
    if (!end || *end != L'\0' || d > 0xFFFF) return false;
    vid = uint16_t(v);
    did = uint16_t(d);
    return true;
}

void reset_id_fields(App& a) {
    if (a.profile == 1) {
        SetWindowTextW(a.eGpuId, L"10DE:1001");
        SetWindowTextW(a.eAhciId, L"15AD:07E0");
    } else if (a.profile == 0) {
        SetWindowTextW(a.eGpuId, L"10DE:1001");
        SetWindowTextW(a.eAhciId, L"8086:A114");
    }
}

void update_custom_enabled(App& a) {
    const bool on = a.optCustomIds && !a.running;
    EnableWindow(a.eGpuId, on);
    EnableWindow(a.eAhciId, on && a.profile != 1);
}

void handle_action(App& a, Action action) {
    switch (action) {
    case Action::Primary: start_job(a, Job::All); break;
    case Action::OnlyExe: start_job(a, Job::Exe); break;
    case Action::OnlyEfi: start_job(a, Job::Efi); break;
    case Action::Driver: start_job(a, Job::Driver); break;
    case Action::Verify: start_job(a, Job::Verify); break;
    case Action::Restore: {
        const int r = MessageBoxW(
            a.hwnd,
            L"Restaurar vmware-vmx.exe y EFI64.ROM originales en la instalación de VMware.\n\n"
            L"- Se respalda el estado actual antes de reemplazar.\n"
            L"- Se elimina efi64.filename del .vmx si indicaste una VM.\n"
            L"- Requiere VMware cerrado.\n\n¿Restaurar?",
            L"Restaurar originales", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (r == IDYES) start_job(a, Job::Restore);
        break;
    }
    case Action::ProfileFull:
        a.profile = 0;
        a.customProfile.clear();
        reset_id_fields(a);
        update_custom_enabled(a);
        break;
    case Action::ProfileGpu:
        a.profile = 1;
        a.customProfile.clear();
        reset_id_fields(a);
        update_custom_enabled(a);
        break;
    case Action::CustomProfile: {
        std::wstring p = open_file(a.hwnd, L"Selecciona un perfil JSON",
                                   L"Perfiles (*.json)\0*.json\0Todos (*.*)\0*.*\0\0");
        if (!p.empty()) {
            a.customProfile = p;
            a.profile = -1;
        }
        break;
    }
    case Action::ToggleForce: a.optForce = !a.optForce; break;
    case Action::ToggleDry: a.optDry = !a.optDry; break;
    case Action::ToggleVmx:
        a.optVmx = !a.optVmx;
        if (a.optVmx && get_text(a.eVmx).empty()) browse(a, Action::BrowseVmx);
        break;
    case Action::ToggleCustomIds:
        a.optCustomIds = !a.optCustomIds;
        if (a.optCustomIds && get_text(a.eGpuId).empty()) reset_id_fields(a);
        update_custom_enabled(a);
        break;
    default: break;
    }
    InvalidateRect(a.hwnd, nullptr, FALSE);
}

void render(App& a, HDC dc, const RECT& client) {
    a.hits.clear();
    layout(a, client.right, client.bottom);
    const auto& L = a.lay;

    Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    auto txt = [&](const std::wstring& s, RECT r, HFONT f, Color c, UINT flags) {
        g.Flush();
        draw_text(dc, s, r, f, c, flags);
    };
    auto rr = [&](RECT r, int radius, Color fill, Color border) {
        round_rect(g, r, a.S(radius), fill, border);
    };
    auto hit = [&](RECT r, Action action, bool enabled = true) {
        if (enabled) add_hit(a, r, action);
    };

    const int W = client.right, H = client.bottom;
    SolidBrush bg(cBg);
    g.FillRectangle(&bg, 0, 0, W, H);
    SolidBrush rail(cRail);
    g.FillRectangle(&rail, 0, 0, L.rail.right, H);

    // Logo + títulos del rail
    RECT logo{a.S(20), a.S(20), a.S(64), a.S(64)};
    rr(logo, 12, cAccent, cAccent);
    txt(L"VM", logo, a.fTitle, Color(255, 255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT t1{a.S(76), a.S(22), a.S(210), a.S(50)};
    txt(L"VM Spoof", t1, a.fTitle, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t2{a.S(77), a.S(48), a.S(210), a.S(70)};
    txt(L"VMware Workstation", t2, a.fSmall, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Perfiles
    auto profile_card = [&](int y, const wchar_t* title, const wchar_t* sub, Action action, bool selected) {
        RECT card{a.S(16), a.S(y), a.S(204), a.S(y + 64)};
        rr(card, 10, selected ? cAccentDim : cCard, selected ? cAccent : cBorder);
        const int cy = (card.top + card.bottom) / 2;
        circle(g, a.S(34), cy, a.S(7), selected ? cAccent : cCard, selected ? cAccent : cBorder);
        if (selected) circle(g, a.S(34), cy, a.S(3), Color(255, 255, 255, 255), Color(0, 0, 0, 0));
        RECT rt{a.S(50), a.S(y + 10), card.right - a.S(10), a.S(y + 34)};
        txt(title, rt, a.fBold, selected ? cText : cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rs{a.S(50), a.S(y + 32), card.right - a.S(8), a.S(y + 56)};
        txt(sub, rs, a.fSmall, cMuted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        hit(card, action, !a.running);
    };
    profile_card(112, L"Full spoof", L"GPU, AHCI, USB, audio", Action::ProfileFull,
                 a.profile == 0);
    profile_card(184, L"GPU only", L"Solo SVGA; no toca AHCI", Action::ProfileGpu, a.profile == 1);

    RECT custom{a.S(16), a.S(262), a.S(204), a.S(288)};
    const bool customSel = a.profile < 0;
    rr(custom, 8, customSel ? cAccentDim : Color(0, 0, 0, 0), customSel ? cAccent : Color(0, 0, 0, 0));
    txt(a.customProfile.empty() ? L"Perfil personalizado…" : fs::path(a.customProfile).filename().wstring(),
        inset(custom, a.S(10), 0), a.fSmall, customSel ? cText : cAccent,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    hit(custom, Action::CustomProfile, !a.running);

    RECT footer{a.S(18), H - a.S(38), a.S(204), H - a.S(18)};
    txt(L"v1.0 · C++ nativo", footer, a.fSmall, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Cabecera
    RECT title{L.contentX, a.S(20), L.contentX + L.contentW, a.S(56)};
    txt(L"VMware VM Spoof", title, a.fTitle, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitle{L.contentX, a.S(52), L.contentX + L.contentW, a.S(76)};
    txt(L"Genera un vmware-vmx.exe y un EFI64.ROM coherentes, con backups y verificación.",
        subtitle, a.fSmall, cMuted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Estado
    {
        Color sc = a.status == Status::Ok ? cOk : a.status == Status::Error ? cErr
                 : a.status == Status::Running ? cAccent : cMuted;
        SIZE sz{};
        HGDIOBJ old = SelectObject(dc, a.fSmall);
        GetTextExtentPoint32W(dc, a.statusText.c_str(), int(a.statusText.size()), &sz);
        SelectObject(dc, old);
        const int pillW = sz.cx + a.S(30);
        RECT pill{client.right - a.S(24) - pillW, a.S(28), client.right - a.S(24), a.S(52)};
        rr(pill, 12, Color(38, sc.GetR(), sc.GetG(), sc.GetB()), Color(80, sc.GetR(), sc.GetG(), sc.GetB()));
        txt(a.statusText, pill, a.fSmall, sc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    auto card = [&](RECT r, bool active) { rr(r, 10, cCard, active ? cAccent : cBorder); };
    auto label = [&](RECT cardRc, const std::wstring& s) {
        RECT r{cardRc.left + a.S(14), cardRc.top + a.S(8), cardRc.right - a.S(14), cardRc.top + a.S(32)};
        txt(s, r, a.fSmall, cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    };
    auto browse_btn = [&](RECT r, Action action) {
        rr(r, 7, cCard, cBorder);
        if (a.hot == action) rr(r, 7, mix(cCard, cAccent, 0.18), cAccent);
        txt(L"Examinar…", r, a.fSmall, cText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hit(r, action, !a.running);
    };

    card(L.exeCard, false);
    label(L.exeCard, L"vmware-vmx.exe original");
    browse_btn(L.exeBrowse, Action::BrowseExe);

    card(L.efiCard, false);
    label(L.efiCard, L"EFI64.ROM original");
    browse_btn(L.efiBrowse, Action::BrowseEfi);

    card(L.vmxCard, a.optVmx);
    label(L.vmxCard, L"VM .vmx (opcional)");
    browse_btn(L.vmxBrowse, Action::BrowseVmx);

    // Toggles
    auto toggle = [&](int x, int y, bool on, const std::wstring& text, Action action) {
        RECT track{x, y, x + a.S(34), y + a.S(18)};
        rr(track, 9, on ? cAccent : Color(255, 62, 68, 78), on ? cAccent : cBorder);
        const int knob = a.S(7);
        circle(g, on ? track.right - a.S(9) : track.left + a.S(9), (track.top + track.bottom) / 2, knob,
               Color(255, 245, 247, 250), Color(0, 0, 0, 0));
        SIZE sz{};
        HGDIOBJ old = SelectObject(dc, a.fSmall);
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &sz);
        SelectObject(dc, old);
        const int labelX = track.right + a.S(8);
        RECT labelRc{labelX, y - a.S(3), labelX + sz.cx + a.S(2), y + a.S(21)};
        txt(text, labelRc, a.fSmall, on ? cText : cMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT h{track.left, y - a.S(4), labelX + sz.cx, y + a.S(22)};
        hit(h, action, !a.running);
        return labelX + sz.cx + a.S(22);
    };
    const int oy = L.optionsY + a.S(12);
    int tx = L.contentX;
    tx = toggle(tx, oy, a.optForce, L"Sobrescribir", Action::ToggleForce);
    tx = toggle(tx, oy, a.optDry, L"Dry-run", Action::ToggleDry);
    toggle(tx, oy, a.optVmx, L"Configurar VM", Action::ToggleVmx);

    // IDs personalizados
    const int cy = L.customY + a.S(6);
    toggle(L.contentX, cy, a.optCustomIds, L"IDs personalizados", Action::ToggleCustomIds);
    auto field_box = [&](RECT edit) {
        RECT box = edit;
        InflateRect(&box, a.S(2), a.S(2));
        rr(box, 7, cEdit, a.optCustomIds ? cBorder : mix(cBorder, cBg, 0.4));
    };
    field_box(L.gpuIdEdit);
    field_box(L.ahciIdEdit);
    const Color idColor = a.optCustomIds ? cText : cMuted;
    RECT gpuLabel{L.gpuIdEdit.left - a.S(40), L.gpuIdEdit.top, L.gpuIdEdit.left - a.S(6),
                  L.gpuIdEdit.bottom};
    txt(L"GPU", gpuLabel, a.fSmall, idColor, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT ahciLabel{L.ahciIdEdit.left - a.S(46), L.ahciIdEdit.top, L.ahciIdEdit.left - a.S(6),
                   L.ahciIdEdit.bottom};
    txt(L"AHCI", ahciLabel, a.fSmall,
        a.optCustomIds && a.profile != 1 ? cText : cMuted,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // Botones
    auto button = [&](RECT r, const std::wstring& text, bool primary, bool danger, bool enabled,
                      Action action) {
        Color fill = primary ? cAccent : cCard;
        Color border = primary ? cAccent : (danger ? cErr : cBorder);
        Color textColor = primary ? Color(255, 255, 255, 255) : (danger ? cErr : cText);
        if (!enabled) {
            fill = mix(cCard, cBg, 0.35);
            border = cBorder;
            textColor = cMuted;
        } else if (a.hot == action) {
            fill = primary ? cAccentHi : mix(cCard, danger ? cErr : cAccent, 0.18);
            border = primary ? cAccent : (danger ? cErr : cAccent);
        }
        if (enabled && a.pressed == action) fill = mix(fill, cBg, 0.25);
        rr(r, 8, fill, border);
        txt(text, r, a.fBold, textColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hit(r, action, enabled);
    };

    const int by = L.actionsY;
    const int bh = a.S(40);
    int bx = L.contentX;
    auto advance = [&](int w) { RECT r{bx, by, bx + a.S(w), by + bh}; bx += a.S(w) + a.S(8); return r; };
    const bool idle = !a.running;
    button(advance(152), L"Instalar patch", true, false, idle, Action::Primary);
    button(advance(92), L"Solo .exe", false, false, idle, Action::OnlyExe);
    button(advance(88), L"Solo EFI", false, false, idle, Action::OnlyEfi);
    button(advance(120), L"Generar driver", false, false, idle, Action::Driver);
    button(advance(92), L"Verificar", false, false, idle, Action::Verify);
    button(advance(100), L"Restaurar", false, true, idle, Action::Restore);

    // Log
    card(L.logCard, false);
    RECT logTitle{L.logCard.left + a.S(14), L.logCard.top + a.S(10), L.logCard.right - a.S(14),
                  L.logCard.top + a.S(34)};
    txt(L"Registro", logTitle, a.fBold, cText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void create_controls(App& a) {
    a.brEdit = CreateSolidBrush(RGB(16, 18, 22));
    a.brBg = CreateSolidBrush(RGB(20, 22, 26));

    auto make_edit = [&](int id, bool multiline, HFONT font) {
        DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
        if (multiline) style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL;
        HWND h = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, a.hwnd, HMENU(INT_PTR(id)), a.inst,
                                 nullptr);
        SendMessageW(h, WM_SETFONT, WPARAM(font), TRUE);
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(a.S(8), a.S(8)));
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        return h;
    };
    a.eExe = make_edit(ID_EXE, false, a.fBody);
    a.eEfi = make_edit(ID_EFI, false, a.fBody);
    a.eVmx = make_edit(ID_VMX, false, a.fBody);
    a.eGpuId = make_edit(ID_GPU_ID, false, a.fBody);
    a.eAhciId = make_edit(ID_AHCI_ID, false, a.fBody);
    a.eLog = make_edit(0, true, a.fMono);

    configure_defaults(a);
    reset_id_fields(a);
    update_custom_enabled(a);
    append_log(a.eLog, L"Destino: " + default_exe_path().parent_path().wstring());
    append_log(a.eLog, L"Listo. Selecciona perfil y pulsa Instalar patch.");
}

void apply_dark_frame(App& a) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(a.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DwmSetWindowAttribute(a.hwnd, 19 /* pre-20H1 */, &dark, sizeof(dark));
    COLORREF caption = RGB(20, 22, 26);
    COLORREF text = RGB(230, 233, 239);
    DwmSetWindowAttribute(a.hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(a.hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(a.hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
}

void recreate_fonts(App& a) {
    create_fonts(a);
    set_font(a.eExe, a.fBody);
    set_font(a.eEfi, a.fBody);
    set_font(a.eVmx, a.fBody);
    set_font(a.eGpuId, a.fBody);
    set_font(a.eAhciId, a.fBody);
    set_font(a.eLog, a.fMono);
    SendMessageW(a.eLog, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(a.S(8), a.S(8)));
}

LRESULT CALLBACK EditSubclass(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            PostMessageW(GetParent(h), WM_APP_RUN, 0, 0);
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, LPARAM(-1));
            return 0;
        }
    }
    if (msg == WM_CHAR && w == VK_RETURN) return 0;
    return DefSubclassProc(h, msg, w, l);
}

void handle_done(App& a, bool ok, std::wstring* message) {
    append_log(a.eLog, *message);
    delete message;
    if (a.worker.joinable()) a.worker.join();
    set_running_ui(a, false);
    set_status(a, ok ? Status::Ok : Status::Error, ok ? L"OK" : L"Error");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    App* a = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        a = static_cast<App*>(cs->lpCreateParams);
        a->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
    }
    if (!a) return DefWindowProcW(hwnd, msg, w, l);

    switch (msg) {
    case WM_CREATE:
        a->dpi = GetDpiForWindow(hwnd);
        if (!a->dpi) a->dpi = 96;
        create_fonts(*a);
        create_controls(*a);
        SetWindowSubclass(a->eExe, EditSubclass, 1, 0);
        SetWindowSubclass(a->eEfi, EditSubclass, 1, 0);
        SetWindowSubclass(a->eVmx, EditSubclass, 1, 0);
        apply_dark_frame(*a);
        return 0;

    case WM_SIZE:
        layout(*a, LOWORD(l), HIWORD(l));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = a->S(900);
        mmi->ptMinTrackSize.y = a->S(620);
        return 0;
    }

    case WM_DPICHANGED: {
        a->dpi = HIWORD(w);
        if (!a->dpi) a->dpi = 96;
        recreate_fonts(*a);
        auto* rc = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout(*a, rc->right - rc->left, rc->bottom - rc->top);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            render(*a, mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, RGB(230, 233, 239));
        SetBkColor(dc, RGB(16, 18, 22));
        return LRESULT(a->brEdit);
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Action action = Action::None;
        hit_test(*a, pt, action);
        if (action != a->hot) {
            a->hot = action;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!a->trackingLeave) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            a->trackingLeave = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        a->trackingLeave = false;
        if (a->hot != Action::None) {
            a->hot = Action::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && a->hot != Action::None) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Action action = Action::None;
        if (hit_test(*a, pt, action)) {
            a->pressed = action;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        Action action = Action::None;
        hit_test(*a, pt, action);
        const Action pressed = a->pressed;
        a->pressed = Action::None;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (action != Action::None && action == pressed) {
            if (action == Action::BrowseExe || action == Action::BrowseEfi ||
                action == Action::BrowseVmx) {
                browse(*a, action);
            } else {
                handle_action(*a, action);
            }
        }
        return 0;
    }

    case WM_APP_RUN:
        if (!a->running) handle_action(*a, Action::Primary);
        return 0;

    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(l));
        append_log(a->eLog, *line);
        return 0;
    }

    case WM_APP_DONE:
        handle_done(*a, w != 0, reinterpret_cast<std::wstring*>(l));
        return 0;

    case WM_CLOSE:
        if (a->running) {
            MessageBoxW(hwnd, L"Hay una operación en curso. Espera a que termine.", L"VM Spoof",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (a->worker.joinable()) a->worker.join();
        if (a->fTitle) DeleteObject(a->fTitle);
        if (a->fSub) DeleteObject(a->fSub);
        if (a->fBody) DeleteObject(a->fBody);
        if (a->fSmall) DeleteObject(a->fSmall);
        if (a->fBold) DeleteObject(a->fBold);
        if (a->fMono) DeleteObject(a->fMono);
        if (a->brEdit) DeleteObject(a->brEdit);
        if (a->brBg) DeleteObject(a->brBg);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

} // namespace

int gui_run(HINSTANCE instance) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput gdi{};
    if (Gdiplus::GdiplusStartup(&g_gdiplus, &gdi, nullptr) != Gdiplus::Ok) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"VmSpoofGui";
    if (!RegisterClassExW(&wc)) {
        Gdiplus::GdiplusShutdown(g_gdiplus);
        return 1;
    }

    static App app;
    app.inst = instance;
    g_app = &app;

    const int dpi = GetDpiForSystem();
    RECT wr{0, 0, MulDiv(1040, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(dpi));
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int wx = work.left + ((work.right - work.left) - ww) / 2;
    const int wy = work.top + ((work.bottom - work.top) - wh) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VM Spoof", WS_OVERLAPPEDWINDOW,
                                wx, wy, ww, wh, nullptr, nullptr, instance, &app);
    if (!hwnd) {
        Gdiplus::GdiplusShutdown(g_gdiplus);
        return 1;
    }
    app.hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    apply_dark_frame(app);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_app = nullptr;
    CoUninitialize();
    Gdiplus::GdiplusShutdown(g_gdiplus);
    return int(msg.wParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) { return gui_run(instance); }
