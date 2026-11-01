#include "window.h"

#include <algorithm>

#include <glad/gles2.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include "paths.h"
#include "platform.h"

namespace cartucho {

namespace {
void GlAttributes(bool desktop) {
    SDL_GL_ResetAttributes();
    if (desktop) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}
}  // namespace

bool Window::Open(const std::string& title, bool fullscreen, std::string* error, bool desktop_gl) {
    if (const char* force = SDL_getenv("CARTUCHO_DESKTOP_GL"))
        desktop_gl = desktop_gl || force[0] == '1';
    desktop_gl_ = desktop_gl;
    GlAttributes(desktop_gl_);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
    if (!window_) {
        *error = std::string("Não foi possível abrir a janela: ") + SDL_GetError();
        return false;
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        // The other kind: some drivers offer only one of them.
        const std::string first = SDL_GetError();
        desktop_gl_ = !desktop_gl_;
        GlAttributes(desktop_gl_);
        SDL_DestroyWindow(window_);
        window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
        gl_ = window_ ? SDL_GL_CreateContext(window_) : nullptr;
        if (!gl_) {
            *error = "A placa de vídeo não oferece OpenGL 4.3 nem OpenGL ES 3.0 (" + first +
                     "). Atualize o driver de vídeo.";
            return false;
        }
    }
    SDL_GL_MakeCurrent(window_, gl_);
    if (!gladLoadGLES2(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        *error = "Não foi possível carregar as funções do OpenGL.";
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    PlatformLog(1, (std::string("GPU: ") + reinterpret_cast<const char*>(glGetString(GL_RENDERER)) + " / " +
                    reinterpret_cast<const char*>(glGetString(GL_VERSION))).c_str());
    SetupImGui();
    return true;
}

void Window::SetupImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    // Scale with the display: 1080p is 1.0.
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window_), &mode) == 0)
        scale_ = std::max(0.75f, std::min(2.5f, mode.h / 1080.0f));
    const std::string fonts = JoinPath(Paths::Get().assets, "fonts");
    const std::string regular = JoinPath(fonts, "Inter-Regular.ttf");
    const std::string bold = JoinPath(fonts, "Inter-Bold.ttf");
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2300, 0x23FF,
                                     0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    auto load = [&](const std::string& path, float px) -> ImFont* {
        if (!FileExists(path))
            return io.Fonts->AddFontDefault();
        return io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale_, &cfg, ranges);
    };
    fonts_.body = load(regular, 19);
    fonts_.small = load(regular, 15);
    fonts_.bold = load(bold, 21);
    fonts_.title = load(bold, 31);
    fonts_.huge = load(bold, 46);

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 16 * scale_;
    st.FrameRounding = 10 * scale_;
    st.PopupRounding = 12 * scale_;
    st.GrabRounding = 10 * scale_;
    st.ScrollbarRounding = 10 * scale_;
    st.ChildRounding = 14 * scale_;
    st.TabRounding = 10 * scale_;
    st.WindowPadding = ImVec2(18 * scale_, 16 * scale_);
    st.FramePadding = ImVec2(12 * scale_, 8 * scale_);
    st.ItemSpacing = ImVec2(10 * scale_, 10 * scale_);
    st.ScrollbarSize = 12 * scale_;
    st.WindowBorderSize = 0;
    st.PopupBorderSize = 0;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.14f, 0.97f);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.10f, 0.17f, 0.98f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.94f, 0.98f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.66f, 0.65f, 0.74f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.15f, 0.23f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.20f, 0.33f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.24f, 0.42f, 1);
    c[ImGuiCol_Button] = ImVec4(0.42f, 0.31f, 1.0f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.40f, 1.0f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.25f, 0.88f, 1);
    c[ImGuiCol_Header] = ImVec4(0.42f, 0.31f, 1.0f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.42f, 0.31f, 1.0f, 0.75f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.42f, 0.31f, 1.0f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.71f, 0.28f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.71f, 0.28f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.78f, 0.4f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.15f, 0.23f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.50f, 0.40f, 1.0f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.42f, 0.31f, 1.0f, 1);
    c[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.71f, 0.28f, 1);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.05f, 0.6f);

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 300 es");
}

void Window::Close() {
    if (gl_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(gl_);
        gl_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void Window::ToggleFullscreen() {
    const bool fs = fullscreen();
    SDL_SetWindowFullscreen(window_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
}

bool Window::fullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

void Window::Size(int* w, int* h) const { SDL_GL_GetDrawableSize(window_, w, h); }

void Window::BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void Window::Render() {
    ImGui::Render();
    int w, h;
    Size(&w, &h);
    glViewport(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Window::Swap() { SDL_GL_SwapWindow(window_); }

}  // namespace cartucho
