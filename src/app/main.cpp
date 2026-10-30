// Zalie desktop application: GLFW window, OpenGL 3 renderer, event loop sleeping while nothing happens.

// clang-format off
#include <GLFW/glfw3.h>  // GLFW_INCLUDE_NONE: the GL declarations come from the Dear ImGui loader
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_opengl3_loader.h>
// clang-format on
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gl-screenshot.hpp"
#include "zalie/collection.hpp"
#include "zalie/desktop-entry.hpp"
#include "zalie/exchange.hpp"
#include "zalie/executor.hpp"
#include "zalie/file-io.hpp"
#include "zalie/paths.hpp"
#include "zalie/raw-chars.hpp"
#include "zalie/resolved-request.hpp"
#include "zalie/ui/app.hpp"
#include "zalie/ui/embedded.hpp"
#include "zalie/ui/theme.hpp"
#include "zalie/variables.hpp"
#include "zalie/workspace.hpp"

namespace zalie::embedded {
std::span<const unsigned char> Icon32Png() noexcept;
std::span<const unsigned char> Icon48Png() noexcept;
std::span<const unsigned char> Icon128Png() noexcept;
}  // namespace zalie::embedded

namespace {

using namespace zalie;

constexpr std::string_view kUsage =
    "Usage: zalie [options] [collection paths...]\n"
    "\n"
    "A light, fast and friendly HTTP client.\n"
    "\n"
    "Paths given on the command line are opened (*.zalie.json) or imported (Bruno folders, Postman exports).\n"
    "\n"
    "Options:\n"
    "  --home <dir>          data directory (settings, history, collections). Default: $ZALIE_HOME or the\n"
    "                        platform configuration directory\n"
    "  --theme <coton|nuit>  start with this theme\n"
    "  --screenshot <file>   render a hidden window and save it as PNG, then exit (documentation)\n"
    "  --size <WxH>          window size (default 1440x900)\n"
    "  --frames <n>          frames rendered before the screenshot (default 10)\n"
    "  --demo <response|pending>  demo state for the screenshot\n"
    "  --no-desktop-entry    Linux: do not write ~/.local/share/applications/zalie.desktop (written at launch\n"
    "                        when no desktop entry exists, for the icon of the task bar)\n"
    "  --version             print the version and exit\n"
    "  --help                print this help and exit\n";

struct Options {
  std::filesystem::path home;
  std::filesystem::path screenshot;
  std::vector<std::filesystem::path> paths;
  std::string_view theme;
  std::string_view demo;
  int width{1440};
  int height{900};
  int frames{10};
  bool desktopEntry{true};
};

bool ParseInt(std::string_view str, int& value) {
  const auto [ptr, errc] = std::from_chars(str.data(), str.data() + str.size(), value);
  return errc == std::errc{} && ptr == str.data() + str.size();
}

// Returns false (after printing why) to exit.
bool ParseOptions(int argc, char** argv, Options& options, int& exitCode) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&] -> std::string_view {
      return i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
    };
    if (arg == "--help" || arg == "-h") {
      std::fwrite(kUsage.data(), 1, kUsage.size(), stdout);
      exitCode = EXIT_SUCCESS;
      return false;
    }
    if (arg == "--version") {
      std::puts("zalie " ZALIE_VERSION_STR);
      exitCode = EXIT_SUCCESS;
      return false;
    }
    if (arg == "--home") {
      options.home = PathFromUtf8(next());
    } else if (arg == "--screenshot") {
      options.screenshot = PathFromUtf8(next());
    } else if (arg == "--theme") {
      options.theme = next();
    } else if (arg == "--demo") {
      options.demo = next();
    } else if (arg == "--no-desktop-entry") {
      options.desktopEntry = false;
    } else if (arg == "--frames") {
      if (!ParseInt(next(), options.frames) || options.frames < 1) {
        std::fputs("zalie: invalid --frames\n", stderr);
        exitCode = EXIT_FAILURE;
        return false;
      }
    } else if (arg == "--size") {
      const std::string_view size = next();
      const std::size_t xPos = size.find('x');
      if (xPos == std::string_view::npos || !ParseInt(size.substr(0, xPos), options.width) ||
          !ParseInt(size.substr(xPos + 1), options.height) || options.width < 320 || options.height < 240) {
        std::fputs("zalie: invalid --size, expected WxH (at least 320x240)\n", stderr);
        exitCode = EXIT_FAILURE;
        return false;
      }
    } else if (arg.starts_with("--")) {
      std::fprintf(stderr, "zalie: unknown option %.*s (see --help)\n", static_cast<int>(arg.size()), arg.data());
      exitCode = EXIT_FAILURE;
      return false;
    } else {
      options.paths.push_back(PathFromUtf8(arg));
    }
  }
  return true;
}

void SetWindowIcon(GLFWwindow* pWindow) {
  std::array<GLFWimage, 3> images{};
  std::array<unsigned char*, 3> pixels{};
  const std::array sources{embedded::Icon32Png(), embedded::Icon48Png(), embedded::Icon128Png()};
  int count = 0;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    int channels = 0;
    pixels[i] = stbi_load_from_memory(sources[i].data(), static_cast<int>(sources[i].size()), &images[i].width,
                                      &images[i].height, &channels, 4);
    if (pixels[i] != nullptr) {
      images[static_cast<std::size_t>(count)] = {images[i].width, images[i].height, pixels[i]};
      ++count;
    }
  }
  if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {  // Wayland has no per-window icon (uses the .desktop file)
    glfwSetWindowIcon(pWindow, count, images.data());
  }
  for (unsigned char* pPixels : pixels) {
    stbi_image_free(pPixels);
  }
}

// Linux desktops (GNOME...) show the icon of the desktop entry matching a window, not the icon of the window itself:
// without an installed entry, Zalie gets a generic icon. When none exists, one is written for this executable.
void EnsureDesktopIntegration() {
#if defined(__linux__)  // NOLINT(readability-use-concise-preprocessor-directives): same style as the #elif below
  std::error_code errc;
  const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", errc);
  if (errc) {
    return;
  }
  const XdgDataDirs dirs = XdgDataDirsFromEnvironment();
  const auto status = EnsureDesktopEntry(dirs, executable, embedded::LogoPng());
  if (!status) {
    std::fprintf(stderr, "zalie: cannot write the desktop entry: %.*s\n", static_cast<int>(status.error().size()),
                 status.error().data());
  } else if (*status == DesktopEntryStatus::created || *status == DesktopEntryStatus::updated) {
    const RawChars32 path = PathToUtf8(dirs.dataHome / "applications" / "zalie.desktop");
    std::printf("zalie: desktop entry %s: %.*s (--no-desktop-entry to skip)\n",
                *status == DesktopEntryStatus::created ? "created" : "updated", static_cast<int>(path.size()),
                path.data());
  }
#endif
}

// A plausible response for the documentation screenshots, without network.
void SetupDemo(ui::App& app, std::string_view demo) {
  Workspace& workspace = app.workspace();
  if (workspace.size() == 0) {
    return;
  }
  OpenCollection& example = workspace[0];
  Folder& basics =
      example.collection.root.folders.empty() ? example.collection.root : example.collection.root.folders[0];
  if (basics.requests.size() < 2) {
    return;
  }
  app.openRequest(basics.requests[0].id);
  const Request& post = basics.requests[1];
  app.openRequest(post.id);
  ui::RequestTab* pTab = app.activeTab();
  if (pTab == nullptr) {
    return;
  }
  if (demo == "pending") {
    // A black-holed address (TEST-NET-1, RFC 5737): the request stays in flight until the screenshot is taken.
    Request& request = *app.requestOf(*pTab);
    const RawChars32 url = request.url;
    request.url.assign(std::string_view{"http://192.0.2.1/slow"});
    app.sendActiveTab();
    request.url = url;
    return;
  }
  Exchange exchange;
  const VariableScope scope = MakeScope(example.collection, example.activeEnvironment);
  exchange.request = ResolveRequest(post, example.collection, scope);
  exchange.requestId = post.id;
  Response& response = exchange.response;
  response.status = 200;
  response.reason = "OK";
  response.state = ExchangeState::completed;
  response.headers.push_back({"content-type", "application/json"});
  response.headers.push_back({"content-length", "512"});
  response.headers.push_back({"server", "gunicorn/19.9.0"});
  response.headers.push_back({"access-control-allow-origin", "*"});
  response.headers.push_back({"date", "Thu, 29 Oct 2026 13:37:00 GMT"});
  response.headBytes = 214;
  response.timings.startedAt = std::chrono::system_clock::now();
  response.timings.head = std::chrono::milliseconds{84};
  response.timings.total = std::chrono::milliseconds{91};
  response.body.raw.append(std::string_view{
      R"({"args":{"source":"zalie"},"data":"{\"name\":\"Zalie\",\"breed\":\"Coton de Tulear\",\"goodGirl\":true}",)"
      R"("files":{},"form":{},"headers":{"Accept-Encoding":"gzip, deflate, zstd, br","Content-Length":"68",)"
      R"("Content-Type":"application/json","Host":"httpbin.org","User-Agent":"zalie/0.1.0"},)"
      R"("json":{"breed":"Coton de Tulear","goodGirl":true,"name":"Zalie","tricks":["fetch","sit","zoomies"],)"
      R"("weightKg":4.2},"method":"POST","origin":"203.0.113.7","url":"https://httpbin.org/anything?source=zalie"})"});
  response.body.analyze("application/json");
  app.showExchange(*pTab, std::move(exchange));
}

void WakeEventLoop(void* /*pUserData*/) { glfwPostEmptyEvent(); }

void GlfwError(int code, const char* pDescription) { std::fprintf(stderr, "GLFW error %d: %s\n", code, pDescription); }

int Run(const Options& options) {
  const std::filesystem::path home = options.home.empty() ? DefaultHomeDirectory() : options.home;
  Workspace workspace(home);
  workspace.load();
  if (!options.theme.empty()) {
    workspace.settings().theme.assign(options.theme);
  }

  glfwSetErrorCallback(GlfwError);
  if (glfwInit() == GLFW_FALSE) {
    std::fputs("zalie: cannot initialize GLFW (no display?)\n", stderr);
    return EXIT_FAILURE;
  }
#ifdef __APPLE__
  const char* pGlslVersion = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  const char* pGlslVersion = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  const bool screenshot = !options.screenshot.empty();
  if (options.desktopEntry && !screenshot) {
    EnsureDesktopIntegration();  // before the window exists: the desktop matches it to the entry when it appears
  }
  glfwWindowHint(GLFW_VISIBLE, screenshot ? GLFW_FALSE : GLFW_TRUE);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  // Identity of the window for the desktop: it must match the desktop entry (packaging/zalie.desktop.in, or the one of
  // EnsureDesktopIntegration: StartupWMClass) for the task bar to show Zalie's icon. GNOME ignores the icon of the
  // window itself (_NET_WM_ICON) and shows a generic one for windows it cannot match to a desktop entry.
  glfwWindowHintString(GLFW_X11_CLASS_NAME, "Zalie");
  glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "zalie");
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, "zalie");
  GLFWwindow* pWindow = glfwCreateWindow(options.width, options.height, "Zalie", nullptr, nullptr);
  if (pWindow == nullptr) {
    std::fputs("zalie: cannot create the window (OpenGL 3 required)\n", stderr);
    glfwTerminate();
    return EXIT_FAILURE;
  }
  glfwMakeContextCurrent(pWindow);
  glfwSwapInterval(1);  // vsync: never render faster than the screen refreshes
  SetWindowIcon(pWindow);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Dear ImGui remembers the table column sizes between sessions, next to the other files of Zalie.
  const RawChars32 iniUtf8 = PathToUtf8(home / "imgui.ini");
  const std::string iniPath{std::string_view(iniUtf8)};
  io.IniFilename = screenshot ? nullptr : iniPath.c_str();
  ImGui_ImplGlfw_InitForOpenGL(pWindow, true);
  ImGui_ImplOpenGL3_Init(pGlslVersion);

  int exitCode = EXIT_SUCCESS;
  {
    Executor executor({.wakeFn = WakeEventLoop,
                       .pWakeUserData = nullptr,
                       .maxBodyBytes = static_cast<std::size_t>(workspace.settings().maxBodyMiB) << 20U,
                       .nbWorkers = workspace.settings().workers});
    ui::App app(workspace, executor);
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    glfwGetWindowContentScale(pWindow, &scaleX, &scaleY);
    app.init(scaleX);
    for (const std::filesystem::path& path : options.paths) {
      app.importPath(path);
    }
    if (!options.demo.empty()) {
      SetupDemo(app, options.demo);
    }

    glfwSetWindowUserPointer(pWindow, &app);
    glfwSetDropCallback(pWindow, [](GLFWwindow* pWin, int count, const char** pPaths) {
      std::vector<std::filesystem::path> paths;
      paths.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        paths.push_back(PathFromUtf8(pPaths[i]));
      }
      static_cast<ui::App*>(glfwGetWindowUserPointer(pWin))->onFilesDropped(paths);
    });
    glfwSetWindowContentScaleCallback(pWindow, [](GLFWwindow* pWin, float contentScaleX, float /*contentScaleY*/) {
      static_cast<ui::App*>(glfwGetWindowUserPointer(pWin))->setDpiScale(contentScaleX);
    });

    app::OffscreenTarget target;
    int frame = 0;
    int extraFrames = 3;  // frames rendered without waiting, to let Dear ImGui settle after an event
    while (glfwWindowShouldClose(pWindow) == GLFW_FALSE) {
      if (screenshot || extraFrames > 0 || app.animating()) {
        glfwPollEvents();
      } else {
        // Nothing moves: sleep until an event (input, completed request, or a periodic refresh of relative times).
        glfwWaitEventsTimeout(io.WantTextInput ? 0.5 : 2.0);
        extraFrames = 2;
      }
      extraFrames = std::max(0, extraFrames - 1);
      if (glfwGetWindowAttrib(pWindow, GLFW_ICONIFIED) != 0 && !screenshot) {
        glfwWaitEventsTimeout(0.25);
        continue;
      }
      app.beforeNewFrame();
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      app.frame();
      ImGui::Render();

      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(pWindow, &width, &height);
      const bool capture = screenshot && ++frame >= options.frames;
      if (capture && !target.create(width, height)) {
        std::fputs("zalie: framebuffer objects unavailable, cannot take the screenshot\n", stderr);
        exitCode = EXIT_FAILURE;
        break;
      }
      if (capture) {
        target.bind();
      }
      glViewport(0, 0, width, height);
      const ImVec4 background = ImGui::ColorConvertU32ToFloat4(ui::CurrentPalette().background);
      glClearColor(background.x, background.y, background.z, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (capture) {
        app::OffscreenTarget::Unbind();
        const RawChars32 file = PathToUtf8(options.screenshot);
        if (target.savePng(options.screenshot)) {
          std::printf("screenshot saved to %.*s\n", static_cast<int>(file.size()), file.data());
        } else {
          std::fprintf(stderr, "zalie: cannot write %.*s\n", static_cast<int>(file.size()), file.data());
          exitCode = EXIT_FAILURE;
        }
        break;
      }
      glfwSwapBuffers(pWindow);
    }
    if (!screenshot) {
      app.shutdown();
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(pWindow);
  glfwTerminate();
  return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  int exitCode = EXIT_SUCCESS;
  if (!ParseOptions(argc, argv, options, exitCode)) {
    return exitCode;
  }
  try {
    return Run(options);
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "zalie: fatal error: %s\n", ex.what());
    return EXIT_FAILURE;
  }
}
