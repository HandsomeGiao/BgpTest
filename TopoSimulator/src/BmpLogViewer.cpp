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

  const float font_size = 16.0f * dpi_scale;
  const std::array<const char *, 3> fonts = {
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\segoeui.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
  };
  for (const auto *font : fonts) {
    if (std::filesystem::exists(font) &&
        io.Fonts->AddFontFromFileTTF(font, font_size, &config,
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
  style.CellPadding = ImVec2(7.0f * dpi_scale, 4.0f * dpi_scale);
  style.ItemSpacing = ImVec2(8.0f * dpi_scale, 6.0f * dpi_scale);
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

bool containsCaseInsensitive(const std::string &value,
                             const std::string &needle) {
  if (needle.empty()) {
    return true;
  }
  auto lower_value = value;
  auto lower_needle = needle;
  std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(),
                 [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(),
                 [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  return lower_value.find(lower_needle) != std::string::npos;
}

std::string upperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::string recordActionLabel(const BmpLogRecord &record) {
  return record.action.empty() ? record.msg_type : record.action;
}

bool recordTypeMatches(const BmpLogRecord &record, const std::string &filter) {
  if (filter.empty()) {
    return true;
  }
  const auto requested = upperAscii(filter);
  const auto action = upperAscii(recordActionLabel(record));
  if (requested == "WITHDRAWAL") {
    return action == "WITHDRAW";
  }
  if (requested == "UPDATE") {
    return action == "UPDATE" || action == "UPDATE+WITHDRAW";
  }
  if (requested == "MIXED") {
    return action == "UPDATE+WITHDRAW";
  }
  return action == requested;
}

bool liveRecordMatches(const BmpLogRecord &record, const BmpLogQuery &query) {
  if (!query.router.empty() && record.router != query.router) {
    return false;
  }
  if (!query.peer.empty() && record.from != query.peer && record.to != query.peer) {
    return false;
  }
  if (!recordTypeMatches(record, query.msg_type)) {
    return false;
  }
  if (!query.prefix.empty() &&
      !containsCaseInsensitive(record.prefixes + "," + record.withdrawn,
                               query.prefix)) {
    return false;
  }
  if (!query.asn.empty() &&
      !containsCaseInsensitive(record.as_path, query.asn)) {
    return false;
  }
  if (!query.next_hop.empty() && record.next_hop != query.next_hop) {
    return false;
  }
  if (query.has_min_local_pref &&
      (!record.local_pref || *record.local_pref < query.min_local_pref)) {
    return false;
  }
  return true;
}

void drawRecordTable(const std::vector<BmpLogRecord> &records,
                     int &selected_index, float height) {
  constexpr ImGuiTableFlags flags =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

  if (!ImGui::BeginTable("bmp-records", 10, flags, ImVec2(0, height))) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 64.0f);
  ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 170.0f);
  ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, 150.0f);
  ImGui::TableSetupColumn("Router", ImGuiTableColumnFlags_WidthFixed, 90.0f);
  ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 80.0f);
  ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, 80.0f);
  ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 125.0f);
  ImGui::TableSetupColumn("Prefixes");
  ImGui::TableSetupColumn("AS_PATH");
  ImGui::TableSetupColumn("Next Hop", ImGuiTableColumnFlags_WidthFixed, 110.0f);
  ImGui::TableHeadersRow();

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(records.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const auto &record = records[static_cast<std::size_t>(row)];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const bool selected = selected_index == row;
      if (ImGui::Selectable(std::to_string(record.id).c_str(), selected,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        selected_index = row;
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(record.timestamp.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(record.event.c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(record.router.c_str());
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(record.from.c_str());
      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(record.to.c_str());
      ImGui::TableSetColumnIndex(6);
      const auto action = recordActionLabel(record);
      ImGui::TextUnformatted(action.c_str());
      ImGui::TableSetColumnIndex(7);
      ImGui::TextUnformatted(record.prefixes.empty() ? record.withdrawn.c_str()
                                                     : record.prefixes.c_str());
      ImGui::TableSetColumnIndex(8);
      ImGui::TextUnformatted(record.as_path.c_str());
      ImGui::TableSetColumnIndex(9);
      ImGui::TextUnformatted(record.next_hop.c_str());
    }
  }
  ImGui::EndTable();
}

std::string bufferText(const std::array<char, 128> &buffer) {
  return std::string(buffer.data());
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
  int history_limit = 500;
  int min_local_pref = 0;
  std::array<char, 128> router_filter{};
  std::array<char, 128> peer_filter{};
  std::array<char, 128> msg_type_filter{};
  std::array<char, 128> prefix_filter{};
  std::array<char, 128> asn_filter{};
  std::array<char, 128> next_hop_filter{};
  std::vector<BmpLogRecord> visible_records;
  std::vector<BmpLogRecord> history_records;

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
      BmpLogQuery query;
      query.router = bufferText(router_filter);
      query.peer = bufferText(peer_filter);
      query.msg_type = bufferText(msg_type_filter);
      query.prefix = bufferText(prefix_filter);
      query.asn = bufferText(asn_filter);
      query.next_hop = bufferText(next_hop_filter);
      query.limit = static_cast<std::size_t>((std::max)(1, history_limit));
      if (min_local_pref > 0) {
        query.has_min_local_pref = true;
        query.min_local_pref = static_cast<std::uint32_t>(min_local_pref);
      }
      history_records = BmpLogManager::instance().queryHistory(query);
      selected_index = history_records.empty() ? -1 : 0;
    }

    ImGui::InputText("Router", router_filter.data(), router_filter.size());
    ImGui::SameLine();
    ImGui::InputText("Peer", peer_filter.data(), peer_filter.size());
    ImGui::SameLine();
    ImGui::InputText("Type/Action", msg_type_filter.data(),
                     msg_type_filter.size());
    ImGui::InputText("Prefix", prefix_filter.data(), prefix_filter.size());
    ImGui::SameLine();
    ImGui::InputText("AS_PATH contains ASN", asn_filter.data(),
                     asn_filter.size());
    ImGui::SameLine();
    ImGui::InputText("Next Hop", next_hop_filter.data(),
                     next_hop_filter.size());
    ImGui::InputInt("Min Local Pref", &min_local_pref);
    ImGui::SameLine();
    ImGui::InputInt("History Limit", &history_limit);

    BmpLogQuery live_query;
    live_query.router = bufferText(router_filter);
    live_query.peer = bufferText(peer_filter);
    live_query.msg_type = bufferText(msg_type_filter);
    live_query.prefix = bufferText(prefix_filter);
    live_query.asn = bufferText(asn_filter);
    live_query.next_hop = bufferText(next_hop_filter);
    if (min_local_pref > 0) {
      live_query.has_min_local_pref = true;
      live_query.min_local_pref = static_cast<std::uint32_t>(min_local_pref);
    }

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

    constexpr float raw_json_height = 170.0f;
    const float table_height =
        (std::max)(220.0f, ImGui::GetContentRegionAvail().y -
                               raw_json_height - ImGui::GetFrameHeightWithSpacing() -
                               ImGui::GetStyle().ItemSpacing.y * 2.0f);
    drawRecordTable(visible_records, selected_index, table_height);

    ImGui::Separator();
    ImGui::TextUnformatted("Selected Raw JSON");
    const char *raw = "";
    if (selected_index >= 0 &&
        selected_index < static_cast<int>(visible_records.size())) {
      raw = visible_records[static_cast<std::size_t>(selected_index)]
                .raw_json.c_str();
    }
    ImGui::InputTextMultiline("##raw-json", const_cast<char *>(raw),
                              std::strlen(raw) + 1, ImVec2(-1.0f, 160.0f),
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
