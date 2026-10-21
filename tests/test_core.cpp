#include "../src/core.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace vmspoof;

namespace {

int g_fail = 0;
int g_pass = 0;
int g_skip = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            ++g_pass;                                                                \
        } else {                                                                     \
            ++g_fail;                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n";   \
        }                                                                            \
    } while (0)

template <class F>
bool throws(F f) {
    try {
        f();
    } catch (...) {
        return true;
    }
    return false;
}

std::wstring env_w(const wchar_t* name, const wchar_t* fallback) {
    const wchar_t* v = _wgetenv(name);
    return v && *v ? std::wstring(v) : std::wstring(fallback);
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

uint32_t read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t read16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

fs::path temp_dir() {
    fs::path p = fs::current_path() / "build" / "testtmp";
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

const Log quiet = [](std::string_view) {};

void test_profiles(const fs::path&) {
    Profile full = resolve_profile("fullspoof");
    CHECK(full.name == "fullspoof");
    CHECK(full.patches.size() == 46);
    CHECK(full.svga.patch);
    CHECK(full.ahci.patch);
    CHECK(full.svga.value() == 0x100110DEu);
    CHECK(full.ahci.value() == 0xA1148086u);
    CHECK(full.exe_expected_sha256 ==
          "be9ad4329edb9442adc49eabd7bcbae55a193e9fb837f08ad1670973b56de5f3");

    Profile gpu = resolve_profile("gpu-only");
    CHECK(gpu.name == "gpu-only");
    CHECK(gpu.patches.size() == 2);
    CHECK(gpu.svga.patch);
    CHECK(!gpu.ahci.patch);
    for (const auto& p : gpu.patches) CHECK(p.cat == "svga");
}

void test_driver_patch(const fs::path& dir) {
    const fs::path src = dir / "driver-src";
    const fs::path out = dir / "driver-out";
    std::error_code ec;
    fs::create_directories(src, ec);

    const std::string inf =
        "[Version]\r\nCatalogFile = vm3d.cat\r\n\r\n[Manufacturer]\r\n"
        "%SVGA% = VM3D_AMD64, PCI\\VEN_15AD&DEV_0405&SUBSYS_040515AD&REV_00\r\n"
        "%SVGA% = VM3D_AMD64, PCI\\VEN_15AD&DEV_0405&SUBSYS_00000000&REV_00\r\n";
    write_file_atomic(src / "oem5.inf", std::vector<uint8_t>(inf.begin(), inf.end()), true);
    write_file_atomic(src / "vm3dmp.sys", {0x4D, 0x5A}, true);
    write_file_atomic(src / "oem5.PNF", {0x50, 0x4E, 0x46}, true);
    const std::vector<uint8_t> gl = {0xB8, 0xAD, 0x15, 0x00, 0x00, 0xB8, 0x05, 0x04, 0x00, 0x00};
    write_file_atomic(src / "vm3dgl.dll", gl, true);
    write_file_atomic(src / "vm3dgl64.dll", gl, true);

    Profile p;
    p.name = "driver-test";
    p.svga = {0x1002, 0x1B80, true};
    Options o;
    o.in = src;
    o.out_dir = out;
    Result r = patch_driver(p, o, quiet);
    CHECK(r.ok);

    auto patched_inf = read_file(out / "oem5.inf");
    const std::string text(patched_inf.begin(), patched_inf.end());
    CHECK(text.find("VEN_1002&DEV_1B80") != std::string::npos);
    CHECK(text.find("VEN_15AD&DEV_0405") == std::string::npos);
    CHECK(!file_exists(out / "oem5.PNF"));
    CHECK(file_exists(out / "LEEME.txt"));

    auto patched_gl = read_file(out / "vm3dgl.dll");
    CHECK(patched_gl.size() == gl.size());
    CHECK(patched_gl[1] == 0x02 && patched_gl[2] == 0x10);
    CHECK(patched_gl[6] == 0x80 && patched_gl[7] == 0x1B);

    auto original_inf = read_file(src / "oem5.inf");
    CHECK(std::string(original_inf.begin(), original_inf.end()).find("VEN_15AD&DEV_0405") !=
          std::string::npos);
}

void test_coherence_negative(const fs::path& dir) {
    const fs::path bad = dir / "bad-profile.json";
    std::string json = R"({
      "name": "bad",
      "vmware_vmx": {
        "original_sha256": "0355b0346e68eebce2d1a3595e18c335f04ac1d42f0dfe7e87c7daf9449f670b",
        "size": 18835712,
        "patches": [ { "off": "0x221590", "orig": "ad15", "new": "de10", "cat": "svga" } ]
      },
      "efi64": {
        "size": 2097152,
        "original_sha256": "dc05979c8d641f701901a0396f7cabca71e302ed8f6339590f92b9c6d1e1e4f8",
        "build": "test",
        "svga": { "vid": "10DE", "did": "1001", "patch": true },
        "ahci": { "vid": "8086", "did": "A114", "patch": true }
      }
    })";
    std::vector<uint8_t> bytes(json.begin(), json.end());
    write_file_atomic(bad, bytes, true);
    CHECK(throws([&] { load_profile(bad); }));
}

void test_orig_validation() {
    Profile p;
    p.name = "unit";
    p.exe_size = 8;
    p.exe_sha256 = std::string(64, '0');
    p.efi_size = 1;
    p.efi_sha256 = std::string(64, '0');
    Patch patch;
    patch.off = 2;
    patch.orig = {0xAA, 0xBB};
    patch.repl = {0x11, 0x22};
    patch.cat = "svga";
    p.patches.push_back(patch);
    p.svga = {0x10DE, 0x1001, true};
    p.ahci = {0x8086, 0xA114, false};

    std::vector<uint8_t> good = {0, 0, 0xAA, 0xBB, 0, 0, 0, 0};
    Report report;
    apply_vmx_patches(p, good, report);
    CHECK(good[2] == 0x11 && good[3] == 0x22);
    CHECK(report.patches.size() == 1 && report.patches[0].ok);

    std::vector<uint8_t> bad = {0, 0, 0xAA, 0xBC, 0, 0, 0, 0};
    CHECK(throws([&] {
        Report r;
        apply_vmx_patches(p, bad, r);
    }));
    CHECK(bad[2] == 0xAA && bad[3] == 0xBC);
}

void test_lzma_roundtrip() {
    std::vector<uint8_t> input(300000);
    uint32_t x = 0x12345678;
    for (size_t i = 0; i < input.size(); ++i) {
        x = x * 1664525u + 1013904223u;
        input[i] = uint8_t((i % 97 == 0) ? (x >> 24) : (i & 0xFF));
    }
    auto compressed = lzma_compress_alone(input);
    CHECK(compressed.size() > 13);
    CHECK(compressed[0] == 0x5D);
    CHECK(compressed[1] == 0x00 && compressed[2] == 0x00 && compressed[3] == 0x00);
    CHECK(compressed[4] == 0x01);
    uint64_t size = 0;
    for (int i = 0; i < 8; ++i) size |= uint64_t(compressed[5 + i]) << (i * 8);
    CHECK(size == input.size());
    auto output = lzma_decompress_alone(compressed.data(), compressed.size(), input.size());
    CHECK(output == input);
}

void test_section(const fs::path& rom_path) {
    if (!file_exists(rom_path)) {
        ++g_skip;
        std::cout << "SKIP seccion LZMA: falta " << rom_path.string() << "\n";
        return;
    }
    auto rom = read_file(rom_path);
    LzmaSection s = find_lzma_section(rom);
    CHECK(s.section_off == 0x6E040);
    CHECK(s.data_off == 0x18);
    CHECK(s.capacity == 0xE37C8);
    auto dxe = lzma_decompress_alone(rom.data() + s.section_off + s.data_off, size_t(s.capacity),
                                     size_t(kDxeSize));
    CHECK(dxe.size() == kDxeSize);
}

void test_golden_exe(const fs::path& exe_path, const fs::path& dir) {
    if (!file_exists(exe_path)) {
        ++g_skip;
        std::cout << "SKIP golden exe: falta " << exe_path.string() << "\n";
        return;
    }
    Profile full = resolve_profile("fullspoof");
    Options o;
    o.in = exe_path;
    o.out = dir / "vmware-vmx.patched.exe";
    Result r = patch_vmx(full, o, quiet);
    CHECK(r.ok);
    if (!r.ok) std::cerr << r.error << "\n";
    CHECK(r.report.out_sha256 ==
          "be9ad4329edb9442adc49eabd7bcbae55a193e9fb837f08ad1670973b56de5f3");

    Result again = patch_vmx(full, o, quiet);
    CHECK(!again.ok);  // idempotencia: la salida ya existe

    Options second;
    second.in = o.out;  // entrada ya parcheada
    second.out = dir / "vmware-vmx.again.exe";
    second.force = true;
    Result forced = patch_vmx(full, second, quiet);
    CHECK(forced.ok);  // idempotente: no cambia nada y permite reinstalar
    CHECK(forced.report.out_sha256 ==
          "be9ad4329edb9442adc49eabd7bcbae55a193e9fb837f08ad1670973b56de5f3");

    Options reinstall;
    reinstall.in = o.out;
    reinstall.out = dir / "vmware-vmx.reinstall.exe";
    Result ri = patch_vmx(full, reinstall, quiet); // sin --force: hash esperado
    CHECK(ri.ok);
    CHECK(ri.report.out_sha256 ==
          "be9ad4329edb9442adc49eabd7bcbae55a193e9fb837f08ad1670973b56de5f3");

    Options custom;
    custom.in = exe_path;
    custom.out = dir / "vmware-vmx.custom.exe";
    custom.svga_id = std::make_pair<uint16_t, uint16_t>(0x1002, 0x1B80);
    custom.ahci_id = std::make_pair<uint16_t, uint16_t>(0x1B4B, 0x9215);
    Result rc = patch_vmx(full, custom, quiet);
    CHECK(rc.ok);
    if (rc.ok) {
        auto bytes = read_file(custom.out);
        CHECK(read16(bytes.data() + 0x221590) == 0x1002);
        CHECK(read16(bytes.data() + 0x2215DB) == 0x1B80);
        CHECK(read32(bytes.data() + 0x204B40) == 0x92151B4B);
    }
}

void test_gpu_only(const fs::path& exe_path, const fs::path& efi_path, const fs::path& dir) {
    if (!file_exists(exe_path) || !file_exists(efi_path)) {
        ++g_skip;
        std::cout << "SKIP gpu-only: faltan binarios\n";
        return;
    }
    Profile gpu = resolve_profile("gpu-only");
    Options oe;
    oe.in = exe_path;
    oe.out = dir / "gpu.exe";
    Result re = patch_vmx(gpu, oe, quiet);
    CHECK(re.ok);
    if (re.ok) {
        auto orig = read_file(exe_path);
        auto patched = read_file(oe.out);
        auto diffs = diff_ranges(orig, patched);
        CHECK(diffs.size() == 2);
        CHECK(diffs[0].off == 0x221590 && diffs[1].off == 0x2215DB);
    }

    Options of;
    of.in = efi_path;
    of.out = dir / "gpu.rom";
    Result rf = patch_efi(gpu, of, quiet);
    CHECK(rf.ok);
    if (!rf.ok) std::cerr << rf.error << "\n";
    if (rf.ok) {
        CHECK(rf.report.efi_diffs.size() == 1);
        CHECK(rf.report.efi_diffs[0].off == 0x314355);
        auto rom = read_file(of.out);
        LzmaSection s = find_lzma_section(rom);
        auto dxe = lzma_decompress_alone(rom.data() + s.section_off + s.data_off, size_t(s.capacity),
                                         size_t(kDxeSize));
        size_t ahci = 0;
        int ahci_count = 0;
        int ahci_original_count = 0;
        for (size_t i = 0; i + 7 <= dxe.size(); ++i) {
            if (dxe[i] == 0x81 && dxe[i + 1] == 0x7D && dxe[i + 2] == 0xD4) {
                ++ahci_count;
                if (read32(dxe.data() + i + 3) == 0x07E015ADu) {
                    ahci = i + 3;
                    ++ahci_original_count;
                }
            }
        }
        CHECK(ahci_count >= 2);
        CHECK(ahci_original_count == 1);
        CHECK(ahci != 0);
        Options vopts;
        vopts.in = oe.out;
        vopts.out = of.out;
        Result vf = verify(gpu, vopts, quiet);
        CHECK(vf.ok);
    }
}

void test_golden_efi(const fs::path& efi_path, const fs::path& dir) {    if (!file_exists(efi_path)) {
        ++g_skip;
        std::cout << "SKIP golden EFI: falta " << efi_path.string() << "\n";
        return;
    }
    Profile full = resolve_profile("fullspoof");
    Options o;
    o.in = efi_path;
    o.out = dir / "EFI64.patched";
    Result r = patch_efi(full, o, quiet);
    CHECK(r.ok);
    if (!r.ok) {
        std::cerr << r.error << "\n";
        return;
    }
    CHECK(r.report.out_size == 2097152);
    CHECK(r.report.compressed > 13);
    CHECK(r.report.compressed <= r.report.capacity);
    CHECK(r.report.efi_diffs.size() == 2);
    CHECK(r.report.efi_diffs[0].off == 0x1CE759 && r.report.efi_diffs[0].len == 4);
    CHECK(r.report.efi_diffs[1].off == 0x314355 && r.report.efi_diffs[1].len == 4);

    auto rom = read_file(o.out);
    auto orig_rom = read_file(efi_path);
    CHECK(rom.size() == orig_rom.size());
    size_t first = 0, last = rom.size();
    while (first < rom.size() && rom[first] == orig_rom[first]) ++first;
    while (last > first && rom[last - 1] == orig_rom[last - 1]) --last;
    CHECK(first >= 0x6E040 && last <= 0x6E040 + 0x0E37E0);

    LzmaSection s = find_lzma_section(rom);
    auto dxe = lzma_decompress_alone(rom.data() + s.section_off + s.data_off, size_t(s.capacity),
                                     size_t(kDxeSize));
    auto orig_dxe = lzma_decompress_alone(orig_rom.data() + s.section_off + s.data_off,
                                          size_t(s.capacity), size_t(kDxeSize));
    auto diffs = diff_ranges(orig_dxe, dxe);
    CHECK(diffs.size() == 2);
    CHECK(diffs[0].off == 0x1CE759 && diffs[0].len == 4);
    CHECK(diffs[1].off == 0x314355 && diffs[1].len == 4);

    Options vopts;
    vopts.in = dir / "vmware-vmx.patched.exe";
    vopts.out = o.out;
    if (file_exists(vopts.in)) {
        Result v = verify(full, vopts, quiet);
        CHECK(v.ok);
        if (!v.ok) std::cerr << v.error << "\n";
    } else {
        ++g_skip;
    }

    // Reinstalar un EFI ya parcheado: se reutiliza sin recomprimir.
    Options reinstall;
    reinstall.in = o.out;
    reinstall.out = dir / "EFI64.reinstall";
    Result ri = patch_efi(full, reinstall, quiet);
    CHECK(ri.ok);
    if (ri.ok) CHECK(ri.report.in_sha256 == ri.report.out_sha256);

    // IDs personalizados coherentes en exe + EFI.
    Options custom;
    custom.in = efi_path;
    custom.out = dir / "EFI64.custom";
    custom.svga_id = std::make_pair<uint16_t, uint16_t>(0x1002, 0x1B80);
    custom.ahci_id = std::make_pair<uint16_t, uint16_t>(0x1B4B, 0x9215);
    Result cr = patch_efi(full, custom, quiet);
    CHECK(cr.ok);
    if (cr.ok) {
        auto rom2 = read_file(custom.out);
        LzmaSection s2 = find_lzma_section(rom2);
        auto d2 = lzma_decompress_alone(rom2.data() + s2.section_off + s2.data_off,
                                        size_t(s2.capacity), size_t(kDxeSize));
        CHECK(read32(d2.data() + 0x314355) == 0x1B801002);
        CHECK(read32(d2.data() + 0x1CE759) == 0x92151B4B);
        CHECK(!cr.report.efi_diffs.empty());

        Options cv;
        cv.in = dir / "vmware-vmx.custom.exe";
        cv.out = custom.out;
        cv.svga_id = custom.svga_id;
        cv.ahci_id = custom.ahci_id;
        if (file_exists(cv.in)) {
            Result v = verify(full, cv, quiet);
            CHECK(v.ok);
        }
    }

    // gpu-only no puede personalizar AHCI.
    Profile gpu = resolve_profile("gpu-only");
    Options g;
    g.in = efi_path;
    g.out = dir / "gpu-bad.rom";
    g.ahci_id = std::make_pair<uint16_t, uint16_t>(0x1B4B, 0x9215);
    Result gr = patch_efi(gpu, g, quiet);
    CHECK(!gr.ok);
}

void test_errors(const fs::path& exe_path, const fs::path& dir) {
    if (!file_exists(exe_path)) {
        ++g_skip;
        return;
    }
    Profile full = resolve_profile("fullspoof");
    Options o;
    o.in = exe_path;
    o.out = dir / "exists.exe";
    write_file_atomic(o.out, {1, 2, 3}, true);
    Result r = patch_vmx(full, o, quiet);
    CHECK(!r.ok);  // salida existente sin --force

    Options bad = o;
    bad.out = dir / "bad.exe";
    bad.force = false;
    auto corrupt = read_file(exe_path);
    corrupt[0x1e14c9] = 0x00; // rompe un patch conocido
    const fs::path corrupt_path = dir / "corrupt.exe";
    write_file_atomic(corrupt_path, corrupt, true);
    bad.in = corrupt_path;
    Result r2 = patch_vmx(full, bad, quiet);
    CHECK(!r2.ok);
    Result missing = patch_vmx(full, [&] {
        Options x = bad;
        x.in = dir / "no-existe.exe";
        return x;
    }(), quiet);
    CHECK(!missing.ok);
}

void test_efi_pattern_missing(const fs::path& efi_path, const fs::path& dir) {
    if (!file_exists(efi_path)) {
        ++g_skip;
        return;
    }
    Profile full = resolve_profile("fullspoof");
    auto rom = read_file(efi_path);
    const LzmaSection sec = find_lzma_section(rom);
    auto dxe = lzma_decompress_alone(rom.data() + sec.section_off + sec.data_off,
                                     size_t(sec.capacity), size_t(kDxeSize));
    const std::vector<uint8_t> pattern = {0x2D, 0xAD, 0x15, 0x05, 0x04,
                                          0xA9, 0xFF, 0xFF, 0xFE, 0xFF};
    auto it = std::search(dxe.begin(), dxe.end(), pattern.begin(), pattern.end());
    CHECK(it != dxe.end());
    if (it == dxe.end()) return;
    *it = 0x2E; // rompe el patrón SVGA
    auto comp = lzma_compress_alone(dxe);
    CHECK(comp.size() <= sec.capacity);
    auto crafted = rom;
    const size_t start = size_t(sec.section_off + sec.data_off);
    std::fill(crafted.begin() + std::ptrdiff_t(start),
              crafted.begin() + std::ptrdiff_t(sec.section_off + sec.section_size), 0xFF);
    std::copy(comp.begin(), comp.end(), crafted.begin() + std::ptrdiff_t(start));
    const fs::path file = dir / "no-pattern.rom";
    write_file_atomic(file, crafted, true);

    Options o;
    o.in = file;
    o.out = dir / "no-pattern.out";
    o.force = true; // evita el gate de hash; debe fallar en el patrón
    Result r = patch_efi(full, o, quiet);
    CHECK(!r.ok);
}

void test_efi_not_lzma(const fs::path& dir) {
    Profile full = resolve_profile("fullspoof");
    std::vector<uint8_t> zeros(size_t(full.efi_size), 0);
    const fs::path file = dir / "not-lzma.rom";
    write_file_atomic(file, zeros, true);
    Options o;
    o.in = file;
    o.out = dir / "not-lzma.out";
    o.force = true;
    Result r = patch_efi(full, o, quiet);
    CHECK(!r.ok);
}

void test_install_restore(const fs::path& exe_path, const fs::path& efi_path, const fs::path& dir) {
    if (!file_exists(exe_path) || !file_exists(efi_path)) {
        ++g_skip;
        return;
    }
    const fs::path fake = dir / "fakevmware";
    const fs::path bin = fake / "x64";
    std::error_code ec;
    fs::remove_all(fake, ec);
    fs::create_directories(bin, ec);

    // Instalación falsa con los binarios originales.
    fs::copy_file(exe_path, bin / "vmware-vmx.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(efi_path, bin / "EFI64.ROM", fs::copy_options::overwrite_existing);
    const fs::path vmx = fake / "Test.vmx";
    write_file_atomic(vmx, std::vector<uint8_t>{'e','f','i','6','4','.','f','i','l','e','n','a','m','e',' ','=',' ',
                                                '"','x','.','R','O','M','"','\n'}, true);
    _wputenv_s(L"VMSPOOF_VMWARE_DIR", fake.wstring().c_str());

    Profile full = resolve_profile("fullspoof");
    Options o;
    o.in = exe_path;
    o.in_efi = efi_path;
    o.out_dir = dir / "fakeout";
    o.install = true;
    Result r = patch_all(full, o, quiet);
    CHECK(r.ok);
    CHECK(sha256_hex(read_file(bin / "vmware-vmx.exe")) ==
          "be9ad4329edb9442adc49eabd7bcbae55a193e9fb837f08ad1670973b56de5f3");
    CHECK(file_exists(bin / "vmspoof-original" / "vmware-vmx.exe"));
    CHECK(file_exists(bin / "vmspoof-original" / "EFI64.ROM"));

    Options ro;
    ro.in = exe_path;
    ro.in_efi = efi_path;
    ro.vmx = vmx;
    Result rr = restore_vmware(full, ro, quiet);
    CHECK(rr.ok);
    CHECK(sha256_hex(read_file(bin / "vmware-vmx.exe")) == full.exe_sha256);
    CHECK(sha256_hex(read_file(bin / "EFI64.ROM")) == full.efi_sha256);
    CHECK(read_file(vmx).size() < 40); // efi64.filename eliminado

    Result again = restore_vmware(full, ro, quiet);
    CHECK(again.ok); // ya son originales

    _wputenv_s(L"VMSPOOF_VMWARE_DIR", L"");
}

void test_manifest(const fs::path& exe_path, const fs::path& dir) {
    fs::path patched = dir / "vmware-vmx.patched.exe";
    if (!file_exists(exe_path) || !file_exists(patched)) {
        ++g_skip;
        return;
    }
    Result r = manifest_diff(exe_path, patched, dir / "manifest.json", true, quiet);
    CHECK(r.ok);
    CHECK(file_exists(dir / "manifest.json"));
    CHECK(fs::file_size(dir / "manifest.json") > 100);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const fs::path dir = temp_dir();
    const fs::path exe = env_w(L"VMSPOOF_VMX_ORIG", L"back up\\vmware-vmx.exe");
    const fs::path efi = env_w(L"VMSPOOF_EFI_ORIG", L"back up\\EFI64.ROM");

    test_profiles(dir);
    test_driver_patch(dir);
    test_coherence_negative(dir);
    test_orig_validation();
    test_lzma_roundtrip();
    test_section(efi);
    test_golden_exe(exe, dir);
    test_gpu_only(exe, efi, dir);
    test_golden_efi(efi, dir);
    test_efi_pattern_missing(efi, dir);
    test_efi_not_lzma(dir);
    test_errors(exe, dir);
    test_install_restore(exe, efi, dir);
    test_manifest(exe, dir);

    std::cout << "\n" << g_pass << " checks OK, " << g_fail << " fallos, " << g_skip << " skips\n";
    return g_fail == 0 ? 0 : 1;
}
