#include "gpu_driver.hpp"

#include "dusk/archive.hpp"
#include "dusk/config.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/scope_guard.hpp"
#include "dusk/settings.h"
#include "dusk/utilities.hpp"

#include <borealis/io.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(DUSK_HAS_GPU_DRIVER)
#include <SDL3/SDL_system.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#endif

namespace dusk::gpu_driver {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kMetaFileName = "meta.json";
constexpr std::string_view kDriversDirName = "gpu_drivers";
constexpr size_t kMaxPackageSize = 64 * 1024 * 1024;

std::string s_activeId;

fs::path drivers_dir() {
    return CachePath / kDriversDirName;
}

std::optional<DriverInfo> read_meta(const std::vector<uint8_t>& bytes, std::string& error) {
    nlohmann::json meta = nlohmann::json::parse(bytes, nullptr, false);
    if (!meta.is_object()) {
        error = "meta.json is not valid JSON.";
        return std::nullopt;
    }
    if (meta.value("schemaVersion", 0) != 1) {
        error = "Unsupported driver package schema version.";
        return std::nullopt;
    }
    DriverInfo info;
    try {
        info.name = meta.value("name", "");
        info.description = meta.value("description", "");
        info.author = meta.value("author", "");
        info.vendor = meta.value("vendor", "");
        info.driverVersion = meta.value("driverVersion", "");
        info.libraryName = meta.value("libraryName", "");
        info.minApi = meta.value("minApi", 0);
    } catch (const nlohmann::json::exception& e) {
        error = fmt::format("meta.json has an invalid field: {}", e.what());
        return std::nullopt;
    }
    if (info.name.empty() || info.libraryName.empty()) {
        error = "meta.json is missing the driver name or library name.";
        return std::nullopt;
    }
    if (!utils::is_safe_path_component(info.libraryName)) {
        error = "meta.json contains an invalid library name.";
        return std::nullopt;
    }
    return info;
}

std::optional<DriverInfo> read_installed(const fs::path& dir) {
    std::ifstream in(dir / kMetaFileName, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    std::string error;
    auto info = read_meta(bytes, error);
    if (!info) {
        DuskLog.warn("Ignoring GPU driver in {}: {}", dir.filename().string(), error);
        return std::nullopt;
    }
    info->id = dir.filename().string();
    std::error_code ec;
    if (!fs::is_regular_file(dir / info->libraryName, ec)) {
        DuskLog.warn("Ignoring GPU driver {}: {} is missing", info->id, info->libraryName);
        return std::nullopt;
    }
    return info;
}

bool read_location(std::string_view location, std::vector<uint8_t>& bytes, std::string& error) {
    auto opened = borealis::io::open(location);
    if (opened.status != borealis::io::Status::Ok || !opened.file) {
        error = opened.message.empty() ? "The selected file could not be opened." : opened.message;
        return false;
    }
    const uint64_t size = opened.file.size();
    if (size > kMaxPackageSize) {
        error = "The selected file is too large to be a driver package.";
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    if (!bytes.empty() && opened.file.read(bytes.data(), bytes.size()) != bytes.size()) {
        error = opened.file.error().empty() ? "The selected file could not be read." :
                                              opened.file.error();
        return false;
    }
    return true;
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

std::vector<DriverInfo> installed() {
    std::vector<DriverInfo> drivers;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(drivers_dir(), ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        if (auto info = read_installed(entry.path())) {
            drivers.push_back(std::move(*info));
        }
    }
    std::ranges::sort(drivers, {}, &DriverInfo::name);
    return drivers;
}

InstallResult install(std::string_view location) {
    InstallResult result;
    std::vector<uint8_t> bytes;
    if (!read_location(location, bytes, result.message)) {
        return result;
    }

    std::error_code ec;
    const fs::path root = drivers_dir();
    fs::create_directories(root, ec);
    const fs::path tempZip = root / ".install.zip";
    if (!write_file(tempZip, bytes)) {
        result.message = "The driver package could not be copied to internal storage.";
        return result;
    }
    fs::path staging;
    const SimpleScopeGuard cleanup{[&] {
        std::error_code ignored;
        fs::remove(tempZip, ignored);
        if (!staging.empty()) {
            fs::remove_all(staging, ignored);
        }
    }};

    try {
        archive::ZipArchive zip{tempZip};
        const auto names = zip.file_names();

        // Locate the metadata and retain its directory prefix.
        std::string prefix;
        bool found = false;
        for (const auto& name : names) {
            if (name == kMetaFileName) {
                prefix.clear();
                found = true;
                break;
            }
            if (name.ends_with(kMetaFileName) && name.size() > kMetaFileName.size() + 1) {
                const auto candidate = name.substr(0, name.size() - kMetaFileName.size());
                if (!found || candidate.size() < prefix.size()) {
                    prefix = candidate;
                    found = true;
                }
            }
        }
        if (!found) {
            result.message = "The selected file is not a driver package (no meta.json).";
            return result;
        }

        auto info = read_meta(zip.read_file(prefix + std::string{kMetaFileName}), result.message);
        if (!info) {
            return result;
        }
        const std::string library = prefix + info->libraryName;
        if (std::ranges::find(names, library) == names.end()) {
            result.message = fmt::format("The driver package is missing {}.", info->libraryName);
            return result;
        }

        info->id = utils::safe_filename(info->name);
        if (info->id.empty() || !utils::is_safe_path_component(info->id)) {
            result.message = "The driver name cannot be used as a folder name.";
            return result;
        }

        const fs::path target = root / info->id;
        staging = root / (".staging_" + info->id);
        fs::remove_all(staging, ec);
        fs::create_directories(staging, ec);
        if (ec) {
            result.message = fmt::format("Could not create {}: {}", staging.string(), ec.message());
            return result;
        }
        for (const auto& name : names) {
            if (!name.starts_with(prefix)) {
                continue;
            }
            const std::string relative = name.substr(prefix.size());
            if (relative.empty() || !utils::is_safe_path_component(relative)) {
                continue;
            }
            if (!write_file(staging / relative, zip.read_file(name))) {
                result.message = fmt::format("Could not write {}.", relative);
                return result;
            }
        }
        fs::remove_all(target, ec);
        fs::rename(staging, target, ec);
        if (ec) {
            result.message = fmt::format("Could not install the driver: {}", ec.message());
            return result;
        }
        DuskLog.info("Installed GPU driver '{}' ({})", info->name, info->libraryName);
        result.ok = true;
        result.driver = std::move(*info);
        return result;
    } catch (const std::exception& e) {
        result.message = fmt::format("The driver package could not be read: {}", e.what());
        return result;
    }
}

bool remove(std::string_view id) {
    if (!utils::is_safe_path_component(id)) {
        return false;
    }
    std::error_code ec;
    const auto removed = fs::remove_all(drivers_dir() / id, ec);
    if (ec) {
        DuskLog.warn("Failed to remove GPU driver {}: {}", id, ec.message());
        return false;
    }
    if (getSettings().video.gpuDriver.getValue() == id) {
        getSettings().video.gpuDriver.setValue("");
        config::save();
    }
    return removed > 0;
}

const std::string& active_id() {
    return s_activeId;
}

#if defined(DUSK_HAS_GPU_DRIVER)

namespace {

constexpr std::string_view kShimDirName = "vulkan_shim";
constexpr std::string_view kShimLibraryName = "libdusk_vulkan_shim.so";
constexpr std::string_view kAdrenotoolsTmpDirName = "adrenotools_tmp";

std::optional<fs::path> native_library_dir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&active_id), &info) == 0 || info.dli_fname == nullptr) {
        return std::nullopt;
    }
    const std::string_view path{info.dli_fname};
    // Hooks must be extracted; loading them directly from the APK is unsupported.
    if (path.find("!/") != std::string_view::npos) {
        DuskLog.error("Native libraries are not extracted ({}); custom GPU drivers need "
                      "useLegacyPackaging", path);
        return std::nullopt;
    }
    return fs::path{path}.parent_path();
}

std::string dir_string(const fs::path& path) {
    std::string result = path.string();
    if (!result.ends_with('/')) {
        result += '/';
    }
    return result;
}

}  // namespace

std::optional<std::string> activate(std::string_view id) {
    s_activeId.clear();
    if (id.empty()) {
        return std::nullopt;
    }
    if (!utils::is_safe_path_component(id)) {
        DuskLog.warn("Ignoring invalid GPU driver id '{}'", id);
        return std::nullopt;
    }
    const fs::path driverDir = drivers_dir() / id;
    const auto info = read_installed(driverDir);
    if (!info) {
        DuskLog.warn("Configured GPU driver '{}' is not installed; using the system driver", id);
        return std::nullopt;
    }
    const int sdkVersion = SDL_GetAndroidSDKVersion();
    if (info->minApi > sdkVersion) {
        DuskLog.warn("GPU driver '{}' requires Android API {} (device has {}); using the system driver",
            info->name, info->minApi, sdkVersion);
        return std::nullopt;
    }
    const auto nativeLibDir = native_library_dir();
    if (!nativeLibDir) {
        DuskLog.error("Could not determine the native library directory; using the system driver");
        return std::nullopt;
    }

    std::error_code ec;
    const fs::path tmpDir = CachePath / kAdrenotoolsTmpDirName;
    fs::create_directories(tmpDir, ec);

    void* vulkanHandle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
        dir_string(tmpDir).c_str(), dir_string(*nativeLibDir).c_str(), dir_string(driverDir).c_str(),
        info->libraryName.c_str(), nullptr, nullptr);
    if (vulkanHandle == nullptr) {
        DuskLog.error("adrenotools failed to load GPU driver '{}' ({}); using the system driver",
            info->name, info->libraryName);
        return std::nullopt;
    }
    auto* getInstanceProcAddr = dlsym(vulkanHandle, "vkGetInstanceProcAddr");
    if (getInstanceProcAddr == nullptr) {
        DuskLog.error("vkGetInstanceProcAddr missing from custom loader: {}", dlerror());
        return std::nullopt;
    }

    const fs::path shimDir = CachePath / kShimDirName;
    const fs::path shimLink = shimDir / "libvulkan.so";
    fs::create_directories(shimDir, ec);
    fs::remove(shimLink, ec);
    fs::create_symlink(*nativeLibDir / kShimLibraryName, shimLink, ec);
    if (ec) {
        DuskLog.error("Could not create {}: {}", shimLink.string(), ec.message());
        return std::nullopt;
    }
    void* shimHandle = dlopen(shimLink.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (shimHandle == nullptr) {
        DuskLog.error("Could not load the Vulkan shim: {}", dlerror());
        return std::nullopt;
    }
    using SetProcAddr = void (*)(void*);
    auto setProcAddr = reinterpret_cast<SetProcAddr>(dlsym(shimHandle, "dusk_vulkan_shim_set_proc_addr"));
    if (setProcAddr == nullptr) {
        DuskLog.error("Vulkan shim is missing its entry point: {}", dlerror());
        return std::nullopt;
    }
    setProcAddr(getInstanceProcAddr);

    s_activeId = std::string{id};
    DuskLog.info("Using custom GPU driver '{}' ({}, {})", info->name, info->libraryName, info->driverVersion);
    return dir_string(shimDir);
}

#else

std::optional<std::string> activate(std::string_view) {
    return std::nullopt;
}

#endif

}  // namespace dusk::gpu_driver
