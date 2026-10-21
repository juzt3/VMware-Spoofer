#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vmspoof {

namespace fs = std::filesystem;

struct Patch {
    uint64_t off = 0;
    std::vector<uint8_t> orig;
    std::vector<uint8_t> repl;
    std::vector<uint8_t> base; // "new" original del perfil (antes de overrides)
    std::string cat;
};

struct EfiDriver {
    uint16_t vid = 0;
    uint16_t did = 0;
    bool patch = false;
    uint32_t value() const { return uint32_t(vid) | (uint32_t(did) << 16); }
};

struct Profile {
    std::string name;
    fs::path source;

    uint64_t exe_size = 0;
    std::string exe_sha256;
    std::string exe_expected_sha256;
    std::vector<Patch> patches;

    uint64_t efi_size = 0;
    std::string efi_sha256;
    std::string efi_build;
    EfiDriver svga;
    EfiDriver ahci;
};

struct Options {
    fs::path in;        // exe for patch-vmx / verify, ROM for patch-efi
    fs::path out;       // exe or ROM output, ROM for verify
    fs::path in_efi;    // patch-all
    fs::path out_dir;   // patch-all
    fs::path vmx;
    fs::path report;
    std::optional<std::pair<uint16_t, uint16_t>> svga_id; // vid, did
    std::optional<std::pair<uint16_t, uint16_t>> ahci_id;
    bool force = false;
    bool dry_run = false;
    bool install = false;
};

struct Range {
    uint64_t off = 0;
    uint64_t len = 0;
    bool operator==(const Range&) const = default;
};

struct AppliedPatch {
    uint64_t off = 0;
    std::string orig;
    std::string repl;
    std::string cat;
    bool ok = false;
    bool already = false;
};

struct Report {
    std::string command;
    std::string profile;
    std::string in_path;
    std::string out_path;
    uint64_t in_size = 0;
    uint64_t out_size = 0;
    std::string in_sha256;
    std::string out_sha256;
    std::string expected_sha256;
    std::vector<AppliedPatch> patches;
    bool efi = false;
    uint64_t section_off = 0;
    uint64_t data_off = 0;
    uint64_t capacity = 0;
    uint64_t compressed = 0;
    std::vector<Range> efi_diffs;
    std::vector<std::string> warnings;
    bool ok = false;
    std::string error;

    std::string to_json() const;
};

struct Result {
    bool ok = false;
    std::string error;
    Report report;
};

using Log = std::function<void(std::string_view)>;

struct LzmaSection {
    uint64_t section_off = 0;
    uint64_t section_size = 0;
    uint64_t data_off = 0;
    uint64_t capacity = 0;
};

constexpr uint64_t kDxeSize = 0x48E010;

Profile load_profile(const fs::path& path);
Profile resolve_profile(const std::string& name_or_path);

std::vector<uint8_t> read_file(const fs::path& path);
std::string sha256_hex(const std::vector<uint8_t>& data);
std::string hex(const uint8_t* data, size_t len);
std::string hex(const std::vector<uint8_t>& data);
std::vector<uint8_t> unhex(std::string_view text, std::string_view what);
void write_file_atomic(const fs::path& out, const std::vector<uint8_t>& data, bool force);

LzmaSection find_lzma_section(const std::vector<uint8_t>& rom);
std::vector<uint8_t> lzma_compress_alone(const std::vector<uint8_t>& input);
std::vector<uint8_t> lzma_decompress_alone(const uint8_t* src, size_t src_len, size_t out_size);
std::vector<Range> diff_ranges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

void apply_vmx_patches(const Profile& profile, std::vector<uint8_t>& data, Report& report);

Result patch_vmx(const Profile& profile, const Options& options, const Log& log);
Result patch_efi(const Profile& profile, const Options& options, const Log& log);
Result patch_all(const Profile& profile, const Options& options, const Log& log);
Result verify(const Profile& profile, const Options& options, const Log& log);
Result restore_vmware(const Profile& profile, const Options& options, const Log& log);
Result manifest_diff(const fs::path& original, const fs::path& patched, const fs::path& out,
                     bool force, const Log& log);

fs::path default_vmware_dir();
fs::path default_exe_path();
fs::path default_efi_path();
fs::path default_out_dir();

} // namespace vmspoof
