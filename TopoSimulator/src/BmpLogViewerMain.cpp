#include "toposim/BmpLogViewer.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>

#include <windows.h>
#include <commdlg.h>

namespace {

void printUsage() {
  std::wcerr << L"Usage: BmpLogViewer.exe [bmp_collector.sqlite]\n";
}

std::optional<std::filesystem::path> chooseDatabaseFile() {
  std::array<wchar_t, MAX_PATH> file_name{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.lpstrFile = file_name.data();
  dialog.nMaxFile = static_cast<DWORD>(file_name.size());
  dialog.lpstrFilter =
      L"SQLite BMP logs (*.sqlite;*.db)\0*.sqlite;*.db\0All files (*.*)\0*.*\0";
  dialog.lpstrTitle = L"Open BMP SQLite log";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

  if (!GetOpenFileNameW(&dialog)) {
    return std::nullopt;
  }
  return std::filesystem::path{file_name.data()};
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc > 2) {
      printUsage();
      return 2;
    }
    if (argc == 2) {
      const std::wstring arg = argv[1];
      if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
        printUsage();
        return 0;
      }
      return toposim::BmpLogViewer::runStandalone(
          std::filesystem::path{arg});
    }

    const auto database_file = chooseDatabaseFile();
    if (!database_file) {
      return 0;
    }
    return toposim::BmpLogViewer::runStandalone(*database_file);
  } catch (const std::exception &error) {
    std::cerr << "BmpLogViewer failed: " << error.what() << '\n';
    MessageBoxA(nullptr, error.what(), "BmpLogViewer failed",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
