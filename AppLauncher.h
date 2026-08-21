#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace app_launcher {

struct LauncherSettings {
  std::wstring install_directory;
  std::wstring executable_name;
  std::wstring application_name;
  int fallback_manifest_resource_id = 0;
  int fallback_config_resource_id = 0;
};

struct LaunchContext {
  HINSTANCE instance = nullptr;
  int show_command = SW_SHOWNORMAL;
  std::vector<std::wstring> arguments;
};

enum class LaunchAction {
  kRanApplication,
  kLaunchedInstalledApplication,
  kAwaitingElevatedInstaller,
  kCancelled,
  kFailed,
};

struct LaunchResult {
  LaunchAction action = LaunchAction::kFailed;
  DWORD error_code = ERROR_SUCCESS;
  int application_exit_code = 0;
};

using ApplicationEntryPoint = int (*)(const LaunchContext& context);

LaunchContext BuildLaunchContext(HINSTANCE instance, int show_command);
LaunchResult RunLauncher(const LauncherSettings& settings,
                         const LaunchContext& context,
                         ApplicationEntryPoint run_application);

}  // namespace app_launcher
