#include "AppLauncher.h"

#include "SimpleJson.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace app_launcher {
namespace {

constexpr wchar_t kManifestFileName[] = L"manifest.json";
constexpr wchar_t kConfigFileName[] = L"config.json";
constexpr wchar_t kVersionsDirectoryName[] = L"versions";
constexpr wchar_t kRunAppArgument[] = L"--run-app";
constexpr wchar_t kInstallModeArgument[] = L"--install-mode";
constexpr ULONGLONG kFnvOffsetBasis = 1469598103934665603ull;
constexpr ULONGLONG kFnvPrime = 1099511628211ull;

struct SourceBundle {
  std::wstring version;
  std::wstring app_name;
  std::wstring executable_path;
  std::wstring manifest_path;
  std::string manifest_text;
  std::wstring config_path;
  std::string config_text;
};

struct InstalledVersionInfo {
  bool exists = false;
  std::wstring executable_path;
  std::wstring manifest_path;
  std::wstring config_path;
  std::wstring version;
};

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }

  int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                   static_cast<int>(value.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }

  std::wstring converted(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                      converted.data(), length);
  return converted;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
  if (left.empty()) {
    return right;
  }
  if (right.empty()) {
    return left;
  }
  if (left.back() == L'\\' || left.back() == L'/') {
    return left + right;
  }
  return left + L"\\" + right;
}

std::wstring GetDirectoryName(const std::wstring& path) {
  size_t index = path.find_last_of(L"\\/");
  if (index == std::wstring::npos) {
    return std::wstring();
  }
  return path.substr(0, index);
}

std::wstring GetFileName(const std::wstring& path) {
  size_t index = path.find_last_of(L"\\/");
  if (index == std::wstring::npos) {
    return path;
  }
  return path.substr(index + 1);
}

std::wstring NormalizePath(const std::wstring& path) {
  if (path.empty()) {
    return std::wstring();
  }

  DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return path;
  }

  std::wstring normalized(needed, L'\0');
  DWORD written = GetFullPathNameW(path.c_str(), needed, normalized.data(), nullptr);
  if (written == 0 || written >= needed) {
    return path;
  }

  normalized.resize(written);
  while (!normalized.empty() && normalized.back() == L'\0') {
    normalized.pop_back();
  }
  return normalized;
}

bool EqualsPathInsensitive(const std::wstring& left, const std::wstring& right) {
  std::wstring left_normalized = NormalizePath(left);
  std::wstring right_normalized = NormalizePath(right);
  if (left_normalized.size() != right_normalized.size()) {
    return false;
  }

  for (size_t i = 0; i < left_normalized.size(); ++i) {
    if (towlower(left_normalized[i]) != towlower(right_normalized[i])) {
      return false;
    }
  }
  return true;
}

std::wstring GetModulePath(HINSTANCE instance) {
  std::wstring path(MAX_PATH, L'\0');
  DWORD copied = 0;
  while (true) {
    copied = GetModuleFileNameW(instance, path.data(), static_cast<DWORD>(path.size()));
    if (copied == 0) {
      return std::wstring();
    }
    if (copied < path.size() - 1) {
      break;
    }
    path.resize(path.size() * 2);
  }
  path.resize(copied);
  return path;
}

bool FileExists(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryExists(const std::wstring& path) {
  if (path.empty() || DirectoryExists(path)) {
    return true;
  }

  std::wstring parent = GetDirectoryName(path);
  if (!parent.empty() && !DirectoryExists(parent) && !EnsureDirectoryExists(parent)) {
    return false;
  }

  if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
    return true;
  }

  return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>* bytes) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER size = {};
  if (GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0) {
    CloseHandle(handle);
    return false;
  }

  bytes->assign(static_cast<size_t>(size.QuadPart), 0);
  DWORD total_read = 0;
  while (total_read < bytes->size()) {
    DWORD chunk_read = 0;
    if (ReadFile(handle, bytes->data() + total_read,
                 static_cast<DWORD>(bytes->size() - total_read), &chunk_read,
                 nullptr) == FALSE) {
      CloseHandle(handle);
      return false;
    }
    if (chunk_read == 0) {
      break;
    }
    total_read += chunk_read;
  }

  bytes->resize(total_read);
  CloseHandle(handle);
  return true;
}

bool ReadFileTextUtf8(const std::wstring& path, std::string* text) {
  std::vector<BYTE> bytes;
  if (!ReadFileBytes(path, &bytes)) {
    return false;
  }
  text->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool WriteBytesToFile(const std::wstring& path, const BYTE* data, size_t size) {
  std::wstring directory = GetDirectoryName(path);
  if (!directory.empty() && !EnsureDirectoryExists(directory)) {
    return false;
  }

  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  size_t total_written = 0;
  while (total_written < size) {
    DWORD chunk_written = 0;
    if (WriteFile(handle, data + total_written,
                  static_cast<DWORD>(size - total_written), &chunk_written,
                  nullptr) == FALSE) {
      CloseHandle(handle);
      return false;
    }
    total_written += chunk_written;
  }

  CloseHandle(handle);
  return true;
}

bool WriteUtf8TextFile(const std::wstring& path, const std::string& text) {
  return WriteBytesToFile(path, reinterpret_cast<const BYTE*>(text.data()), text.size());
}

ULONGLONG HashBytes(const BYTE* data, size_t size) {
  ULONGLONG hash = kFnvOffsetBasis;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<ULONGLONG>(data[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

bool HashFile(const std::wstring& path, ULONGLONG* hash) {
  std::vector<BYTE> bytes;
  if (!ReadFileBytes(path, &bytes)) {
    return false;
  }
  *hash = HashBytes(bytes.data(), bytes.size());
  return true;
}

bool HashText(const std::string& text, ULONGLONG* hash) {
  *hash = HashBytes(reinterpret_cast<const BYTE*>(text.data()), text.size());
  return true;
}

std::wstring GetErrorMessage(DWORD error_code) {
  LPWSTR buffer = nullptr;
  DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring message;
  if (length != 0 && buffer != nullptr) {
    message.assign(buffer, length);
    LocalFree(buffer);
  }
  return message;
}

void ShowErrorBox(const std::wstring& title, const std::wstring& message, DWORD error_code) {
  std::wstring full_message = message;
  if (error_code != ERROR_SUCCESS) {
    full_message += L"\n\nError " + std::to_wstring(error_code) + L": " +
                    GetErrorMessage(error_code);
  }
  MessageBoxW(nullptr, full_message.c_str(), title.c_str(),
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

bool LoadResourceText(HINSTANCE instance, int resource_id, std::string* text) {
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
  if (resource == nullptr) {
    return false;
  }

  HGLOBAL loaded = LoadResource(instance, resource);
  if (loaded == nullptr) {
    return false;
  }

  DWORD size = SizeofResource(instance, resource);
  const void* data = LockResource(loaded);
  if (data == nullptr || size == 0) {
    return false;
  }

  text->assign(static_cast<const char*>(data), static_cast<size_t>(size));
  return true;
}

bool LoadSourceBundle(const LauncherSettings& settings, HINSTANCE instance,
                      const std::wstring& source_directory,
                      const std::wstring& executable_path, SourceBundle* bundle,
                      DWORD* error_code) {
  bundle->executable_path = executable_path;

  bundle->manifest_path = JoinPath(source_directory, kManifestFileName);
  if (!ReadFileTextUtf8(bundle->manifest_path, &bundle->manifest_text)) {
    if (!LoadResourceText(instance, settings.fallback_manifest_resource_id,
                          &bundle->manifest_text)) {
      *error_code = GetLastError();
      return false;
    }
    bundle->manifest_path.clear();
  }

  bundle->config_path = JoinPath(source_directory, kConfigFileName);
  if (!ReadFileTextUtf8(bundle->config_path, &bundle->config_text)) {
    if (!LoadResourceText(instance, settings.fallback_config_resource_id,
                          &bundle->config_text)) {
      *error_code = GetLastError();
      return false;
    }
    bundle->config_path.clear();
  }

  simple_json::Document document;
  std::string parse_error;
  if (!document.Parse(bundle->manifest_text, &parse_error)) {
    *error_code = ERROR_BAD_FORMAT;
    return false;
  }

  std::optional<std::string> version = document.GetString("version");
  if (!version.has_value() || version->empty()) {
    *error_code = ERROR_BAD_FORMAT;
    return false;
  }
  bundle->version = Utf8ToWide(*version);

  std::optional<std::string> app_name = document.GetString("appName");
  bundle->app_name = app_name.has_value() ? Utf8ToWide(*app_name)
                                          : settings.application_name;
  if (bundle->app_name.empty()) {
    bundle->app_name = L"Application";
  }

  return true;
}

InstalledVersionInfo LoadInstalledVersionInfo(const LauncherSettings& settings) {
  InstalledVersionInfo info;
  info.executable_path = JoinPath(settings.install_directory, settings.executable_name);
  info.manifest_path = JoinPath(settings.install_directory, kManifestFileName);
  info.config_path = JoinPath(settings.install_directory, kConfigFileName);

  if (!FileExists(info.manifest_path) || !FileExists(info.executable_path)) {
    return info;
  }

  std::string manifest_text;
  if (!ReadFileTextUtf8(info.manifest_path, &manifest_text)) {
    return info;
  }

  simple_json::Document document;
  if (!document.Parse(manifest_text, nullptr)) {
    return info;
  }

  std::optional<std::string> version = document.GetString("version");
  if (!version.has_value() || version->empty()) {
    return info;
  }

  info.exists = true;
  info.version = Utf8ToWide(*version);
  return info;
}

std::vector<int> ParseVersion(const std::wstring& version) {
  std::vector<int> parts;
  std::wstring current;
  for (wchar_t ch : version) {
    if (ch == L'.') {
      parts.push_back(current.empty() ? 0 : _wtoi(current.c_str()));
      current.clear();
      continue;
    }
    if (ch >= L'0' && ch <= L'9') {
      current.push_back(ch);
    }
  }
  parts.push_back(current.empty() ? 0 : _wtoi(current.c_str()));
  return parts;
}

int CompareVersions(const std::wstring& left, const std::wstring& right) {
  std::vector<int> left_parts = ParseVersion(left);
  std::vector<int> right_parts = ParseVersion(right);
  size_t count = std::max(left_parts.size(), right_parts.size());
  left_parts.resize(count, 0);
  right_parts.resize(count, 0);
  for (size_t i = 0; i < count; ++i) {
    if (left_parts[i] < right_parts[i]) {
      return -1;
    }
    if (left_parts[i] > right_parts[i]) {
      return 1;
    }
  }
  return 0;
}

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
    return false;
  }

  TOKEN_ELEVATION elevation = {};
  DWORD length = 0;
  BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &length);
  CloseHandle(token);
  return ok == TRUE && elevation.TokenIsElevated != 0;
}

std::wstring GetKnownFolderPath(REFKNOWNFOLDERID folder_id) {
  PWSTR path = nullptr;
  if (SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &path) != S_OK) {
    return std::wstring();
  }
  std::wstring result(path);
  CoTaskMemFree(path);
  return result;
}

bool PathStartsWithInsensitive(const std::wstring& path, const std::wstring& prefix) {
  if (prefix.empty()) {
    return false;
  }

  std::wstring normalized_path = NormalizePath(path);
  std::wstring normalized_prefix = NormalizePath(prefix);
  if (normalized_path.size() < normalized_prefix.size()) {
    return false;
  }

  for (size_t i = 0; i < normalized_prefix.size(); ++i) {
    if (towlower(normalized_path[i]) != towlower(normalized_prefix[i])) {
      return false;
    }
  }
  return true;
}

bool NeedsElevationForInstall(const std::wstring& install_directory) {
  std::wstring program_files = GetKnownFolderPath(FOLDERID_ProgramFiles);
  std::wstring program_files_x86 = GetKnownFolderPath(FOLDERID_ProgramFilesX86);
  return PathStartsWithInsensitive(install_directory, program_files) ||
         PathStartsWithInsensitive(install_directory, program_files_x86);
}

bool HasArgument(const LaunchContext& context, const std::wstring& argument) {
  return std::find(context.arguments.begin(), context.arguments.end(), argument) !=
         context.arguments.end();
}

bool RelaunchElevated(const std::wstring& executable_path,
                     const std::wstring& working_directory, DWORD* error_code) {
  HINSTANCE result = ShellExecuteW(nullptr, L"runas", executable_path.c_str(),
                                   kInstallModeArgument, working_directory.c_str(),
                                   SW_SHOWNORMAL);
  INT_PTR code = reinterpret_cast<INT_PTR>(result);
  if (code <= 32) {
    *error_code = static_cast<DWORD>(code);
    return false;
  }
  return true;
}

bool CopyFileOrReturnTrueIfUnchanged(const std::wstring& source_path,
                                     const std::wstring& destination_path,
                                     DWORD* error_code) {
  if (FileExists(destination_path)) {
    ULONGLONG source_hash = 0;
    ULONGLONG destination_hash = 0;
    if (HashFile(source_path, &source_hash) && HashFile(destination_path, &destination_hash) &&
        source_hash == destination_hash) {
      return true;
    }
  }

  std::wstring directory = GetDirectoryName(destination_path);
  if (!directory.empty() && !EnsureDirectoryExists(directory)) {
    *error_code = GetLastError();
    return false;
  }

  if (CopyFileW(source_path.c_str(), destination_path.c_str(), FALSE) == FALSE) {
    *error_code = GetLastError();
    return false;
  }
  return true;
}

bool WriteTextOrReturnTrueIfUnchanged(const std::string& text,
                                      const std::wstring& destination_path,
                                      DWORD* error_code) {
  if (FileExists(destination_path)) {
    ULONGLONG new_hash = 0;
    ULONGLONG existing_hash = 0;
    std::string existing_text;
    if (HashText(text, &new_hash) && ReadFileTextUtf8(destination_path, &existing_text) &&
        HashText(existing_text, &existing_hash) && new_hash == existing_hash) {
      return true;
    }
  }

  if (!WriteUtf8TextFile(destination_path, text)) {
    *error_code = GetLastError();
    return false;
  }
  return true;
}

bool VerifyArchivedVersion(const SourceBundle& source, const std::wstring& archive_directory) {
  std::wstring archived_executable = JoinPath(archive_directory, GetFileName(source.executable_path));
  std::wstring archived_manifest = JoinPath(archive_directory, kManifestFileName);
  std::wstring archived_config = JoinPath(archive_directory, kConfigFileName);
  if (!FileExists(archived_executable) || !FileExists(archived_manifest) ||
      !FileExists(archived_config)) {
    return false;
  }

  ULONGLONG source_hash = 0;
  ULONGLONG archive_hash = 0;
  if (!HashFile(source.executable_path, &source_hash) ||
      !HashFile(archived_executable, &archive_hash) || source_hash != archive_hash) {
    return false;
  }

  ULONGLONG manifest_hash = 0;
  ULONGLONG archived_manifest_hash = 0;
  HashText(source.manifest_text, &manifest_hash);
  if (!HashFile(archived_manifest, &archived_manifest_hash) ||
      manifest_hash != archived_manifest_hash) {
    return false;
  }

  ULONGLONG config_hash = 0;
  ULONGLONG archived_config_hash = 0;
  HashText(source.config_text, &config_hash);
  if (!HashFile(archived_config, &archived_config_hash) ||
      config_hash != archived_config_hash) {
    return false;
  }

  return true;
}

class ProgressDialog {
 public:
  bool Create(const std::wstring& title) {
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = &ProgressDialog::WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"AppLauncherProgressDialog";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window_class);

    window_ = CreateWindowExW(
        WS_EX_DLGMODALFRAME, window_class.lpszClassName, title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 150, nullptr, nullptr,
        window_class.hInstance, nullptr);
    if (window_ == nullptr) {
      return false;
    }

    status_ = CreateWindowW(L"STATIC", L"Preparing installation...",
                            WS_CHILD | WS_VISIBLE,
                            20, 20, 360, 20, window_, nullptr,
                            window_class.hInstance, nullptr);
    progress_bar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                    WS_CHILD | WS_VISIBLE,
                                    20, 60, 360, 24, window_, nullptr,
                                    window_class.hInstance, nullptr);
    SendMessageW(progress_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(progress_bar_, PBM_SETSTEP, 1, 0);

    ShowWindow(window_, SW_SHOWNORMAL);
    UpdateWindow(window_);
    PumpMessages();
    return true;
  }

  void Update(int progress, const std::wstring& text) {
    if (window_ == nullptr) {
      return;
    }
    SetWindowTextW(status_, text.c_str());
    SendMessageW(progress_bar_, PBM_SETPOS, progress, 0);
    PumpMessages();
  }

  void Close() {
    if (window_ != nullptr) {
      DestroyWindow(window_);
      window_ = nullptr;
      status_ = nullptr;
      progress_bar_ = nullptr;
    }
  }

  ~ProgressDialog() { Close(); }

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    switch (message) {
      case WM_CLOSE:
        return 0;
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  void PumpMessages() {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  HWND window_ = nullptr;
  HWND status_ = nullptr;
  HWND progress_bar_ = nullptr;
};

bool ArchiveVersionIfNeeded(const LauncherSettings& settings, const SourceBundle& source,
                            DWORD* error_code) {
  std::wstring versions_directory = JoinPath(settings.install_directory, kVersionsDirectoryName);
  std::wstring archive_directory = JoinPath(versions_directory, L"v" + source.version);

  if (DirectoryExists(archive_directory)) {
    if (VerifyArchivedVersion(source, archive_directory)) {
      return true;
    }
    *error_code = ERROR_ALREADY_EXISTS;
    return false;
  }

  if (!EnsureDirectoryExists(archive_directory)) {
    *error_code = GetLastError();
    return false;
  }

  if (!CopyFileOrReturnTrueIfUnchanged(source.executable_path,
                                       JoinPath(archive_directory,
                                                GetFileName(source.executable_path)),
                                       error_code)) {
    return false;
  }
  if (!WriteTextOrReturnTrueIfUnchanged(source.manifest_text,
                                        JoinPath(archive_directory, kManifestFileName),
                                        error_code)) {
    return false;
  }
  if (!WriteTextOrReturnTrueIfUnchanged(source.config_text,
                                        JoinPath(archive_directory, kConfigFileName),
                                        error_code)) {
    return false;
  }
  return true;
}

std::wstring GetTimestampString() {
  SYSTEMTIME system_time = {};
  GetLocalTime(&system_time);
  wchar_t buffer[32] = {};
  StringCchPrintfW(buffer, ARRAYSIZE(buffer), L"%04u%02u%02u_%02u%02u%02u",
                   system_time.wYear, system_time.wMonth, system_time.wDay,
                   system_time.wHour, system_time.wMinute, system_time.wSecond);
  return std::wstring(buffer);
}

bool BackupInstalledConfigIfPresent(const InstalledVersionInfo& installed,
                                    const std::wstring& install_directory,
                                    const std::string& new_config_text,
                                    bool* notify_difference, DWORD* error_code) {
  *notify_difference = false;
  if (!FileExists(installed.config_path)) {
    return true;
  }

  std::wstring version_suffix = installed.version.empty() ? L"unknown" : installed.version;
  std::wstring backup_file_name = L"config.json.backup.v" + version_suffix + L"." +
                                  GetTimestampString() + L".json";
  std::wstring backup_path = JoinPath(install_directory, backup_file_name);
  if (CopyFileW(installed.config_path.c_str(), backup_path.c_str(), TRUE) == FALSE) {
    *error_code = GetLastError();
    return false;
  }

  ULONGLONG current_hash = 0;
  ULONGLONG new_hash = 0;
  std::string current_config_text;
  if (ReadFileTextUtf8(installed.config_path, &current_config_text) &&
      HashText(current_config_text, &current_hash) &&
      HashText(new_config_text, &new_hash) && current_hash != new_hash) {
    *notify_difference = true;
  }
  return true;
}

bool CreateShortcut(const std::wstring& shortcut_path, const std::wstring& target_path,
                    const std::wstring& working_directory,
                    const std::wstring& description) {
  std::wstring shortcut_directory = GetDirectoryName(shortcut_path);
  if (!shortcut_directory.empty() && !EnsureDirectoryExists(shortcut_directory)) {
    return false;
  }

  IShellLinkW* shell_link = nullptr;
  if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&shell_link)) != S_OK) {
    return false;
  }

  shell_link->SetPath(target_path.c_str());
  shell_link->SetWorkingDirectory(working_directory.c_str());
  shell_link->SetDescription(description.c_str());

  IPersistFile* persist_file = nullptr;
  HRESULT result = shell_link->QueryInterface(IID_PPV_ARGS(&persist_file));
  if (FAILED(result)) {
    shell_link->Release();
    return false;
  }

  result = persist_file->Save(shortcut_path.c_str(), TRUE);
  persist_file->Release();
  shell_link->Release();
  return SUCCEEDED(result);
}

bool CreateOrUpdateShortcuts(const LauncherSettings& settings,
                             const std::wstring& target_executable) {
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  std::wstring start_menu = GetKnownFolderPath(FOLDERID_Programs);
  std::wstring desktop = GetKnownFolderPath(FOLDERID_Desktop);
  if (start_menu.empty() || desktop.empty()) {
    if (SUCCEEDED(init)) {
      CoUninitialize();
    }
    return false;
  }
  std::wstring link_name = settings.application_name.empty() ? L"Application.lnk"
                                                             : settings.application_name + L".lnk";
  bool success = CreateShortcut(JoinPath(start_menu, link_name), target_executable,
                                settings.install_directory,
                                settings.application_name.empty() ? L"Application"
                                                                  : settings.application_name);
  success = CreateShortcut(JoinPath(desktop, link_name), target_executable,
                           settings.install_directory,
                           settings.application_name.empty() ? L"Application"
                                                             : settings.application_name) && success;

  if (SUCCEEDED(init)) {
    CoUninitialize();
  }
  return success;
}

bool CopyPayloadToInstallDirectory(const LauncherSettings& settings,
                                   const SourceBundle& source, DWORD* error_code) {
  if (!EnsureDirectoryExists(settings.install_directory)) {
    *error_code = GetLastError();
    return false;
  }

  if (!CopyFileOrReturnTrueIfUnchanged(
          source.executable_path,
          JoinPath(settings.install_directory, settings.executable_name), error_code)) {
    return false;
  }
  if (!WriteTextOrReturnTrueIfUnchanged(
          source.manifest_text,
          JoinPath(settings.install_directory, kManifestFileName), error_code)) {
    return false;
  }
  if (!WriteTextOrReturnTrueIfUnchanged(
          source.config_text,
          JoinPath(settings.install_directory, kConfigFileName), error_code)) {
    return false;
  }
  return true;
}

bool InstallFromSource(const LauncherSettings& settings, const SourceBundle& source,
                       const InstalledVersionInfo& installed,
                       bool* notify_config_difference, DWORD* error_code) {
  ProgressDialog progress_dialog;
  progress_dialog.Create(L"Installing " +
                         (source.app_name.empty() ? settings.application_name
                                                  : source.app_name));
  progress_dialog.Update(10, L"Verifying version archive...");
  if (!ArchiveVersionIfNeeded(settings, source, error_code)) {
    progress_dialog.Close();
    return false;
  }

  progress_dialog.Update(40, L"Backing up configuration...");
  if (!BackupInstalledConfigIfPresent(installed, settings.install_directory,
                                      source.config_text,
                                      notify_config_difference, error_code)) {
    progress_dialog.Close();
    return false;
  }

  progress_dialog.Update(70, L"Copying files to install location...");
  if (!CopyPayloadToInstallDirectory(settings, source, error_code)) {
    progress_dialog.Close();
    return false;
  }

  progress_dialog.Update(90, L"Updating shortcuts...");
  if (!CreateOrUpdateShortcuts(settings,
                               JoinPath(settings.install_directory,
                                        settings.executable_name))) {
    *error_code = GetLastError();
    progress_dialog.Close();
    return false;
  }

  progress_dialog.Update(100, L"Installation complete.");
  Sleep(250);
  progress_dialog.Close();
  return true;
}

bool ConfirmInstallChange(const LauncherSettings& settings,
                          const InstalledVersionInfo& installed,
                          const SourceBundle& source) {
  if (!installed.exists) {
    return true;
  }

  int comparison = CompareVersions(source.version, installed.version);
  if (comparison == 0) {
    return true;
  }

  std::wstring operation = comparison > 0 ? L"upgrade" : L"downgrade";
  std::wstring message = L"Install " + source.app_name + L" version " + source.version +
                         L" and " + operation + L" from version " + installed.version + L"?";
  int result = MessageBoxW(nullptr, message.c_str(), settings.application_name.c_str(),
                           MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
  return result == IDYES;
}

}  // namespace

LaunchContext BuildLaunchContext(HINSTANCE instance, int show_command) {
  LaunchContext context;
  context.instance = instance;
  context.show_command = show_command;

  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments != nullptr) {
    for (int i = 1; i < argument_count; ++i) {
      context.arguments.emplace_back(arguments[i]);
    }
    LocalFree(arguments);
  }

  return context;
}

LaunchResult RunLauncher(const LauncherSettings& settings,
                         const LaunchContext& context,
                         ApplicationEntryPoint run_application) {
  LaunchResult result;
  std::wstring current_executable = GetModulePath(context.instance);
  std::wstring current_directory = GetDirectoryName(current_executable);
  std::wstring installed_executable =
      JoinPath(settings.install_directory, settings.executable_name);
  bool run_app_requested = HasArgument(context, kRunAppArgument);
  bool install_mode = HasArgument(context, kInstallModeArgument);

  if (run_app_requested || EqualsPathInsensitive(current_executable, installed_executable)) {
    result.action = LaunchAction::kRanApplication;
    result.application_exit_code = run_application(context);
    return result;
  }

  DWORD error_code = ERROR_SUCCESS;
  SourceBundle source;
  if (!LoadSourceBundle(settings, context.instance, current_directory,
                        current_executable, &source, &error_code)) {
    ShowErrorBox(settings.application_name, L"Unable to load launcher payload.",
                 error_code);
    result.error_code = error_code;
    return result;
  }

  InstalledVersionInfo installed = LoadInstalledVersionInfo(settings);
  if (installed.exists) {
    int comparison = CompareVersions(source.version, installed.version);
    if (comparison == 0) {
      MessageBoxW(nullptr, L"This version is already installed. Launching the installed application.",
                  settings.application_name.c_str(), MB_OK | MB_ICONINFORMATION);
      HINSTANCE launched = ShellExecuteW(nullptr, L"open", installed.executable_path.c_str(),
                                         kRunAppArgument, settings.install_directory.c_str(),
                                         SW_SHOWNORMAL);
      if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        result.error_code = static_cast<DWORD>(reinterpret_cast<INT_PTR>(launched));
        ShowErrorBox(settings.application_name,
                     L"Unable to start the installed application.",
                     result.error_code);
        return result;
      }
      result.action = LaunchAction::kLaunchedInstalledApplication;
      return result;
    }
  }

  if (!install_mode && !ConfirmInstallChange(settings, installed, source)) {
    result.action = LaunchAction::kCancelled;
    return result;
  }

  if (!install_mode && NeedsElevationForInstall(settings.install_directory) &&
      !IsProcessElevated()) {
    if (!RelaunchElevated(current_executable, current_directory, &error_code)) {
      ShowErrorBox(settings.application_name, L"Unable to request administrator access.",
                   error_code);
      result.error_code = error_code;
      return result;
    }
    result.action = LaunchAction::kAwaitingElevatedInstaller;
    return result;
  }

  bool notify_config_difference = false;
  if (!InstallFromSource(settings, source, installed, &notify_config_difference,
                         &error_code)) {
    ShowErrorBox(settings.application_name, L"Installation failed.", error_code);
    result.error_code = error_code;
    return result;
  }

  if (notify_config_difference) {
    MessageBoxW(nullptr,
                L"The existing configuration was backed up because it differed from the incoming configuration.",
                settings.application_name.c_str(), MB_OK | MB_ICONINFORMATION);
  }

  HINSTANCE launched = ShellExecuteW(nullptr, L"open", installed_executable.c_str(),
                                     kRunAppArgument, settings.install_directory.c_str(),
                                     SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(launched) <= 32) {
    result.error_code = static_cast<DWORD>(reinterpret_cast<INT_PTR>(launched));
    ShowErrorBox(settings.application_name,
                 L"Installation succeeded, but the installed application could not be started.",
                 result.error_code);
    return result;
  }

  result.action = LaunchAction::kLaunchedInstalledApplication;
  return result;
}

}  // namespace app_launcher
