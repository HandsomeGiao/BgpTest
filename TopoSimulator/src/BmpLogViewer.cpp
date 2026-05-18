#include "toposim/BmpLogViewer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <tchar.h>
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "toposim/BmpLogManager.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace toposim {
namespace {

std::thread g_viewer_thread;
std::mutex g_thread_mutex;
std::atomic_bool g_running{false};
std::atomic_bool g_stop_requested{false};

ID3D11Device *g_pd3dDevice = nullptr;
ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
IDXGISwapChain *g_pSwapChain = nullptr;
ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

bool createDeviceD3D(HWND hWnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  constexpr D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL feature_level{};
  const HRESULT result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, feature_levels,
      static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, &sd,
      &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
  return result == S_OK;
}

void configureDpiAwareness() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

float dpiScaleForWindow(HWND hwnd) {
  const UINT dpi = GetDpiForWindow(hwnd);
  return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

void loadReadableFont(ImGuiIO &io, float dpi_scale) {
  ImFontConfig config;
  config.OversampleH = 3;
  config.OversampleV = 2;
  config.PixelSnapH = true;

  const float font_size = 20.0f * dpi_scale;
  const auto windows_dir = [] {
    std::array<char, MAX_PATH> buffer{};
    const UINT size =
        GetWindowsDirectoryA(buffer.data(), static_cast<UINT>(buffer.size()));
    if (size > 0 && size < buffer.size()) {
      return std::filesystem::path{buffer.data()};
    }
    return std::filesystem::path{};
  }();
  const std::array<const char *, 3> fonts = {
      "msyh.ttc",
      "segoeui.ttf",
      "arial.ttf",
  };
  for (const auto *font : fonts) {
    const auto font_path = windows_dir / "Fonts" / font;
    const auto font_path_text = font_path.string();
    if (!windows_dir.empty() && std::filesystem::exists(font_path) &&
        io.Fonts->AddFontFromFileTTF(font_path_text.c_str(), font_size, &config,
                                     io.Fonts->GetGlyphRangesChineseFull())) {
      return;
    }
  }

  io.Fonts->AddFontDefault();
}

void applyReadableStyle(float dpi_scale) {
  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(dpi_scale);
  style.WindowRounding = 0.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameRounding = 3.0f * dpi_scale;
  style.GrabRounding = 3.0f * dpi_scale;
  style.CellPadding = ImVec2(8.0f * dpi_scale, 5.0f * dpi_scale);
  style.ItemSpacing = ImVec2(9.0f * dpi_scale, 7.0f * dpi_scale);
}

void cleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

void createRenderTarget() {
  ID3D11Texture2D *back_buffer = nullptr;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  if (back_buffer) {
    g_pd3dDevice->CreateRenderTargetView(back_buffer, nullptr,
                                         &g_mainRenderTargetView);
    back_buffer->Release();
  }
}

void cleanupDeviceD3D() {
  cleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
    return true;
  }

  switch (msg) {
  case WM_SIZE:
    if (wParam == SIZE_MINIMIZED) {
      return 0;
    }
    g_resizeWidth = LOWORD(lParam);
    g_resizeHeight = HIWORD(lParam);
    return 0;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU) {
      return 0;
    }
    break;
  case WM_DESTROY:
    g_stop_requested = true;
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

std::string upperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::string recordActionLabel(const BmpLogRecord &record) {
  return record.action;
}

enum class BmpViewerColumn : std::size_t {
  Id,
  Time,
  Event,
  Router,
  From,
  FromAs,
  To,
  ToAs,
  Action,
  Prefixes,
  AsPath,
  NextHop,
  LocalPref,
  Med,
  Sequence,
  RawJson,
  Count
};

constexpr std::size_t columnIndex(BmpViewerColumn column) {
  return static_cast<std::size_t>(column);
}

constexpr std::size_t kBmpViewerColumnCount =
    columnIndex(BmpViewerColumn::Count);

struct BmpViewerColumnDefinition {
  BmpViewerColumn column;
  const char *label;
  float width;
  bool default_visible;
};

constexpr std::array<BmpViewerColumnDefinition, kBmpViewerColumnCount>
    kBmpViewerColumns = {{
        {BmpViewerColumn::Id, "ID", 64.0f, true},
        {BmpViewerColumn::Time, "Time", 170.0f, true},
        {BmpViewerColumn::Event, "Event", 150.0f, true},
        {BmpViewerColumn::Router, "Router", 90.0f, false},
        {BmpViewerColumn::From, "From", 80.0f, true},
        {BmpViewerColumn::FromAs, "From AS", 85.0f, false},
        {BmpViewerColumn::To, "To", 80.0f, true},
        {BmpViewerColumn::ToAs, "To AS", 85.0f, false},
        {BmpViewerColumn::Action, "Action", 125.0f, true},
        {BmpViewerColumn::Prefixes, "Prefixes", 0.0f, true},
        {BmpViewerColumn::AsPath, "AS_PATH", 0.0f, true},
        {BmpViewerColumn::NextHop, "Next Hop", 110.0f, true},
        {BmpViewerColumn::LocalPref, "Local Pref", 95.0f, false},
        {BmpViewerColumn::Med, "MED", 70.0f, false},
        {BmpViewerColumn::Sequence, "Sequence", 95.0f, false},
        {BmpViewerColumn::RawJson, "Raw JSON", 360.0f, false},
    }};

using BmpViewerColumnVisibility =
    std::array<bool, kBmpViewerColumnCount>;

BmpViewerColumnVisibility defaultColumnVisibility() {
  BmpViewerColumnVisibility visibility{};
  for (const auto &definition : kBmpViewerColumns) {
    visibility[columnIndex(definition.column)] = definition.default_visible;
  }
  return visibility;
}

void setAllColumns(BmpViewerColumnVisibility &visibility, bool value) {
  visibility.fill(value);
}

int visibleColumnCount(const BmpViewerColumnVisibility &visibility) {
  return static_cast<int>(
      std::count(visibility.begin(), visibility.end(), true));
}

std::string prefixColumnValue(const BmpLogRecord &record) {
  if (record.prefixes.empty()) {
    return record.withdrawn;
  }
  if (record.withdrawn.empty()) {
    return record.prefixes;
  }
  return record.prefixes + " | withdrawn: " + record.withdrawn;
}

std::string columnValue(const BmpLogRecord &record, BmpViewerColumn column) {
  switch (column) {
  case BmpViewerColumn::Id:
    return std::to_string(record.id);
  case BmpViewerColumn::Time:
    return record.timestamp;
  case BmpViewerColumn::Event:
    return record.event;
  case BmpViewerColumn::Router:
    return record.router;
  case BmpViewerColumn::From:
    return record.from;
  case BmpViewerColumn::FromAs:
    return record.from_as ? std::to_string(*record.from_as) : "";
  case BmpViewerColumn::To:
    return record.to;
  case BmpViewerColumn::ToAs:
    return record.to_as ? std::to_string(*record.to_as) : "";
  case BmpViewerColumn::Action:
    return recordActionLabel(record);
  case BmpViewerColumn::Prefixes:
    return prefixColumnValue(record);
  case BmpViewerColumn::AsPath:
    return record.as_path;
  case BmpViewerColumn::NextHop:
    return record.next_hop;
  case BmpViewerColumn::LocalPref:
    return record.local_pref ? std::to_string(*record.local_pref) : "";
  case BmpViewerColumn::Med:
    return record.med ? std::to_string(*record.med) : "";
  case BmpViewerColumn::Sequence:
    return std::to_string(record.sequence);
  case BmpViewerColumn::RawJson:
    return record.raw_json;
  case BmpViewerColumn::Count:
    break;
  }
  return {};
}

struct MessageFilterState {
  std::array<char, 192> routers{};
  std::array<char, 192> from_routers{};
  std::array<char, 192> to_routers{};
  std::array<char, 192> actions{};
  std::array<char, 192> from_asns{};
  std::array<char, 192> to_asns{};
  int history_limit = 500;
};

std::vector<std::string> splitFilterTokens(const std::string &text) {
  std::vector<std::string> tokens;
  std::string token;
  auto flush = [&] {
    if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  };
  for (const char ch : text) {
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' ||
        ch == ';') {
      flush();
    } else {
      token.push_back(ch);
    }
  }
  flush();
  return tokens;
}

std::vector<std::uint32_t> parseAsnTokens(const std::string &text) {
  std::vector<std::uint32_t> asns;
  for (const auto &token : splitFilterTokens(text)) {
    try {
      asns.push_back(static_cast<std::uint32_t>(std::stoul(token)));
    } catch (const std::exception &) {
    }
  }
  return asns;
}

std::string normalizeActionToken(std::string value) {
  return upperAscii(std::move(value));
}

std::vector<std::string> parseActionTokens(const std::string &text) {
  std::vector<std::string> actions;
  for (auto token : splitFilterTokens(text)) {
    actions.push_back(normalizeActionToken(std::move(token)));
  }
  return actions;
}

bool matchesAnyString(const std::vector<std::string> &needles,
                      std::initializer_list<std::string_view> values) {
  if (needles.empty()) {
    return true;
  }
  for (const auto &needle : needles) {
    for (const auto value : values) {
      if (needle == value) {
        return true;
      }
    }
  }
  return false;
}

bool matchesAnyAsn(const std::vector<std::uint32_t> &asns,
                   std::optional<std::uint32_t> value) {
  if (asns.empty()) {
    return true;
  }
  return value &&
         std::find(asns.begin(), asns.end(), *value) != asns.end();
}

BmpLogQuery queryFromFilter(const MessageFilterState &filter) {
  BmpLogQuery query;
  query.routers = splitFilterTokens(filter.routers.data());
  query.from_routers = splitFilterTokens(filter.from_routers.data());
  query.to_routers = splitFilterTokens(filter.to_routers.data());
  query.actions = parseActionTokens(filter.actions.data());
  query.from_asns = parseAsnTokens(filter.from_asns.data());
  query.to_asns = parseAsnTokens(filter.to_asns.data());
  query.limit = static_cast<std::size_t>((std::max)(1, filter.history_limit));
  return query;
}

bool liveRecordMatches(const BmpLogRecord &record, const BmpLogQuery &query) {
  if (!matchesAnyString(query.routers, {record.router, record.from, record.to})) {
    return false;
  }
  if (!matchesAnyString(query.from_routers, {record.from})) {
    return false;
  }
  if (!matchesAnyString(query.to_routers, {record.to})) {
    return false;
  }
  if (!query.actions.empty()) {
    const auto action = normalizeActionToken(recordActionLabel(record));
    if (std::find(query.actions.begin(), query.actions.end(), action) ==
        query.actions.end()) {
      return false;
    }
  }
  if (!matchesAnyAsn(query.from_asns, record.from_as)) {
    return false;
  }
  if (!matchesAnyAsn(query.to_asns, record.to_as)) {
    return false;
  }
  return true;
}

void drawColumnController(BmpViewerColumnVisibility &visible_columns) {
  if (ImGui::Button("Columns")) {
    ImGui::OpenPopup("bmp-column-controller");
  }
  if (!ImGui::BeginPopup("bmp-column-controller")) {
    return;
  }

  ImGui::TextUnformatted("Visible columns");
  ImGui::Separator();
  for (const auto &definition : kBmpViewerColumns) {
    ImGui::Checkbox(definition.label,
                    &visible_columns[columnIndex(definition.column)]);
  }

  ImGui::Separator();
  if (ImGui::Button("Show All")) {
    setAllColumns(visible_columns, true);
  }
  ImGui::SameLine();
  if (ImGui::Button("Hide All")) {
    setAllColumns(visible_columns, false);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset")) {
    visible_columns = defaultColumnVisibility();
  }

  ImGui::EndPopup();
}

void drawMessageFilter(MessageFilterState &filter) {
  if (ImGui::Button("MessageFilter")) {
    ImGui::OpenPopup("bmp-message-filter");
  }
  if (!ImGui::BeginPopup("bmp-message-filter")) {
    return;
  }

  ImGui::TextUnformatted("Routers");
  ImGui::InputText("Contains", filter.routers.data(), filter.routers.size());
  ImGui::InputText("From", filter.from_routers.data(),
                   filter.from_routers.size());
  ImGui::InputText("To", filter.to_routers.data(), filter.to_routers.size());

  ImGui::Separator();
  ImGui::TextUnformatted("Action");
  ImGui::InputText("Actions", filter.actions.data(), filter.actions.size());

  ImGui::Separator();
  ImGui::TextUnformatted("AS");
  ImGui::InputText("From AS", filter.from_asns.data(),
                   filter.from_asns.size());
  ImGui::InputText("To AS", filter.to_asns.data(), filter.to_asns.size());

  ImGui::Separator();
  ImGui::InputInt("History Limit", &filter.history_limit);
  if (filter.history_limit < 1) {
    filter.history_limit = 1;
  }
  if (ImGui::Button("Clear")) {
    const auto limit = filter.history_limit;
    filter = MessageFilterState{};
    filter.history_limit = limit;
  }

  ImGui::EndPopup();
}

void drawRecordTable(const std::vector<BmpLogRecord> &records,
                     int &selected_index, float height,
                     const BmpViewerColumnVisibility &visible_columns) {
  constexpr ImGuiTableFlags flags =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

  const int column_count = visibleColumnCount(visible_columns);
  if (column_count == 0) {
    ImGui::TextDisabled("No columns selected.");
    return;
  }

  if (!ImGui::BeginTable("bmp-records", column_count, flags,
                         ImVec2(0, height))) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  for (const auto &definition : kBmpViewerColumns) {
    if (!visible_columns[columnIndex(definition.column)]) {
      continue;
    }
    const ImGuiTableColumnFlags column_flags =
        definition.width > 0.0f ? ImGuiTableColumnFlags_WidthFixed
                                : ImGuiTableColumnFlags_None;
    ImGui::TableSetupColumn(definition.label, column_flags, definition.width);
  }
  ImGui::TableHeadersRow();

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(records.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto &record = records[static_cast<std::size_t>(row)];
      ImGui::TableNextRow();
      int visible_index = 0;
      bool first_visible_column = true;
      for (const auto &definition : kBmpViewerColumns) {
        if (!visible_columns[columnIndex(definition.column)]) {
          continue;
        }
        ImGui::TableSetColumnIndex(visible_index++);
        auto value = columnValue(record, definition.column);
        if (first_visible_column) {
          if (value.empty()) {
            value = "(empty)";
          }
          const auto label = value + "##row-" + std::to_string(row);
          if (ImGui::Selectable(label.c_str(), selected_index == row,
                                ImGuiSelectableFlags_SpanAllColumns)) {
            selected_index = row;
          }
          first_visible_column = false;
        } else {
          ImGui::TextUnformatted(value.c_str());
        }
      }
    }
  }
  ImGui::EndTable();
}

void viewerLoop() {
  g_stop_requested = false;
  configureDpiAwareness();

  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0L, 0L,
                    GetModuleHandleW(nullptr), nullptr, nullptr, nullptr,
                    nullptr, L"TopoSimulatorBmpViewer", nullptr};
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(
      wc.lpszClassName, L"TopoSimulator BMP Log Viewer",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 900, nullptr,
      nullptr,
      wc.hInstance, nullptr);

  if (!createDeviceD3D(hwnd)) {
    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_running = false;
    return;
  }

  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  const float dpi_scale = dpiScaleForWindow(hwnd);
  loadReadableFont(io, dpi_scale);
  ImGui::StyleColorsDark();
  applyReadableStyle(dpi_scale);

  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  bool live_mode = true;
  bool follow_live = true;
  int selected_index = -1;
  MessageFilterState message_filter;
  std::vector<BmpLogRecord> visible_records;
  std::vector<BmpLogRecord> history_records;
  auto visible_columns = defaultColumnVisibility();

  while (!g_stop_requested) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) {
        g_stop_requested = true;
      }
    }
    if (g_stop_requested) {
      break;
    }

    if (g_resizeWidth != 0 && g_resizeHeight != 0) {
      cleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                  DXGI_FORMAT_UNKNOWN, 0);
      g_resizeWidth = g_resizeHeight = 0;
      createRenderTarget();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("BMP Logs", nullptr, window_flags);

    ImGui::Text("JSONL: %s", BmpLogManager::instance().logFile().string().c_str());
    ImGui::Text("SQLite: %s",
                BmpLogManager::instance().databaseFile().string().c_str());
    ImGui::Text("Events: %llu",
                static_cast<unsigned long long>(
                    BmpLogManager::instance().totalEvents()));

    ImGui::Separator();
    ImGui::Checkbox("Live", &live_mode);
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_live);
    ImGui::SameLine();
    if (ImGui::Button("Query History")) {
      live_mode = false;
      const auto query = queryFromFilter(message_filter);
      history_records = BmpLogManager::instance().queryHistory(query);
      selected_index = history_records.empty() ? -1 : 0;
    }
    ImGui::SameLine();
    drawMessageFilter(message_filter);
    ImGui::SameLine();
    drawColumnController(visible_columns);

    const auto live_query = queryFromFilter(message_filter);

    if (live_mode) {
      visible_records.clear();
      auto live_records = BmpLogManager::instance().liveSnapshot();
      for (const auto &record : live_records) {
        if (liveRecordMatches(record, live_query)) {
          visible_records.push_back(record);
        }
      }
    } else {
      visible_records = history_records;
    }

    ImGui::Text("%s records: %d", live_mode ? "Live" : "History",
                static_cast<int>(visible_records.size()));
    if (follow_live && live_mode && !visible_records.empty()) {
      selected_index = static_cast<int>(visible_records.size()) - 1;
    } else if (selected_index >= static_cast<int>(visible_records.size())) {
      selected_index = visible_records.empty() ? -1 : 0;
    }

    const float raw_json_input_height = 180.0f * dpi_scale;
    const float detail_region_height =
        raw_json_input_height + ImGui::GetTextLineHeightWithSpacing() +
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const float table_height =
        (std::max)(220.0f * dpi_scale,
                   ImGui::GetContentRegionAvail().y - detail_region_height);
    drawRecordTable(visible_records, selected_index, table_height,
                    visible_columns);

    ImGui::Separator();
    ImGui::TextUnformatted("Selected Raw JSON");
    const char *raw = "";
    if (selected_index >= 0 &&
        selected_index < static_cast<int>(visible_records.size())) {
      raw = visible_records[static_cast<std::size_t>(selected_index)]
                .raw_json.c_str();
    }
    ImGui::InputTextMultiline("##raw-json", const_cast<char *>(raw),
                              std::strlen(raw) + 1,
                              ImVec2(-1.0f, raw_json_input_height),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::End();

    ImGui::Render();
    constexpr float clear_color[4] = {0.05f, 0.06f, 0.08f, 1.0f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                            nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                               clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  cleanupDeviceD3D();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  g_running = false;
}

} // namespace

bool BmpLogViewer::startDetached() {
  std::lock_guard lock(g_thread_mutex);
  if (g_viewer_thread.joinable()) {
    if (g_running) {
      return false;
    }
    g_viewer_thread.join();
  }
  if (g_running) {
    return false;
  }
  g_stop_requested = false;
  g_running = true;
  g_viewer_thread = std::thread(viewerLoop);
  return true;
}

void BmpLogViewer::stopAndJoin() {
  {
    std::lock_guard lock(g_thread_mutex);
    if (!g_viewer_thread.joinable()) {
      return;
    }
    g_stop_requested = true;
    PostThreadMessageW(GetThreadId(g_viewer_thread.native_handle()), WM_QUIT,
                       0, 0);
  }
  g_viewer_thread.join();
  g_running = false;
}

bool BmpLogViewer::isRunning() { return g_running.load(); }

} // namespace toposim
