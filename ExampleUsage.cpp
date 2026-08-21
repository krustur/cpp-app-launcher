#include "AppLauncher.h"
#include "resource.h"

#include <windows.h>

namespace {

int RunApplication(const app_launcher::LaunchContext&) {
  MessageBoxW(nullptr, L"MyApp is running.", L"MyApp", MB_OK | MB_ICONINFORMATION);
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  app_launcher::LauncherSettings settings;
  settings.install_directory = L"C:\\Program Files\\MyApp";
  settings.executable_name = L"MyApp.exe";
  settings.application_name = L"MyApp";
  settings.fallback_manifest_resource_id = IDR_FALLBACK_MANIFEST;
  settings.fallback_config_resource_id = IDR_FALLBACK_CONFIG;

  app_launcher::LaunchContext context =
      app_launcher::BuildLaunchContext(instance, show_command);
  app_launcher::LaunchResult result =
      app_launcher::RunLauncher(settings, context, &RunApplication);

  return result.application_exit_code;
}
