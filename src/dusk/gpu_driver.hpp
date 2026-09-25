#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Installs and activates Android GPU-driver packages through libadrenotools.
namespace dusk::gpu_driver {

struct DriverInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string vendor;
    std::string driverVersion;
    std::string libraryName;
    int minApi = 0;
};

struct InstallResult {
    bool ok = false;
    std::string message;
    DriverInfo driver;

    explicit operator bool() const { return ok; }
};

std::vector<DriverInfo> installed();
InstallResult install(std::string_view location);
bool remove(std::string_view id);

// Must run before aurora_initialize. Returns nullopt when using the system driver.
std::optional<std::string> activate(std::string_view id);
const std::string& active_id();

}  // namespace dusk::gpu_driver
