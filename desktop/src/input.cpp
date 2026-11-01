#include "input.h"

#include <cstdlib>

#include "settings.h"

namespace cartucho {

const char* PadName(int id) {
    static const char* n[] = {"B", "Y", "Select", "Start", "Cima", "Baixo", "Esquerda", "Direita",
                              "A", "X", "L", "R", "L2", "R2", "L3", "R3"};
    return (id >= 0 && id < kPadCount) ? n[id] : "?";
}

const char* ActionName(Action a) {
    switch (a) {
        case Action::kMenu: return "Menu";
        case Action::kFastForward: return "Acelerar";
        case Action::kRewind: return "Voltar no tempo (segurar)";
        case Action::kQuickSave: return "Salvar rápido";
        case Action::kQuickLoad: return "Carregar rápido";
        case Action::kFullscreen: return "Tela cheia";
        case Action::kScreenshot: return "Captura de tela";
        default: return "";
    }
}

void Input::ResetKeys() {
    keys = {
        {kUp, SDL_SCANCODE_UP}, {kDown, SDL_SCANCODE_DOWN}, {kLeft, SDL_SCANCODE_LEFT}, {kRight, SDL_SCANCODE_RIGHT},
        {kA, SDL_SCANCODE_X}, {kB, SDL_SCANCODE_Z}, {kX, SDL_SCANCODE_S}, {kY, SDL_SCANCODE_A},
        {kL, SDL_SCANCODE_Q}, {kR, SDL_SCANCODE_W}, {kL2, SDL_SCANCODE_1}, {kR2, SDL_SCANCODE_3},
        {kStart, SDL_SCANCODE_RETURN}, {kSelect, SDL_SCANCODE_RSHIFT},
    };
    action_keys = {
        {Action::kMenu, SDL_SCANCODE_ESCAPE}, {Action::kFastForward, SDL_SCANCODE_SPACE},
        {Action::kRewind, SDL_SCANCODE_BACKSPACE}, {Action::kQuickSave, SDL_SCANCODE_F5},
        {Action::kQuickLoad, SDL_SCANCODE_F8}, {Action::kFullscreen, SDL_SCANCODE_F11},
        {Action::kScreenshot, SDL_SCANCODE_F12},
    };
}

void Input::ResetPad() {
    // Nintendo-style positions: the bottom button is B, the right one A.
    pad_buttons = {
        {kB, SDL_CONTROLLER_BUTTON_A}, {kA, SDL_CONTROLLER_BUTTON_B}, {kY, SDL_CONTROLLER_BUTTON_X},
        {kX, SDL_CONTROLLER_BUTTON_Y}, {kL, SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {kR, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {kL3, SDL_CONTROLLER_BUTTON_LEFTSTICK}, {kR3, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
        {kStart, SDL_CONTROLLER_BUTTON_START}, {kSelect, SDL_CONTROLLER_BUTTON_BACK},
        {kUp, SDL_CONTROLLER_BUTTON_DPAD_UP}, {kDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {kLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {kRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    };
    action_buttons = {{Action::kMenu, SDL_CONTROLLER_BUTTON_GUIDE}};
}

void Input::Load() {
    ResetKeys();
    ResetPad();
    auto& s = Settings::Get();
    auto k = s.Raw("pc_keys");
    if (k.is_object())
        for (auto& [id, sc] : k.items())
            keys[std::atoi(id.c_str())] = static_cast<SDL_Scancode>(sc.get<int>());
    auto ak = s.Raw("pc_action_keys");
    if (ak.is_object())
        for (auto& [id, sc] : ak.items())
            action_keys[static_cast<Action>(std::atoi(id.c_str()))] = static_cast<SDL_Scancode>(sc.get<int>());
    auto pb = s.Raw("pc_pad");
    if (pb.is_object())
        for (auto& [id, b] : pb.items())
            pad_buttons[std::atoi(id.c_str())] = static_cast<SDL_GameControllerButton>(b.get<int>());
    auto ab = s.Raw("pc_pad_actions");
    if (ab.is_object())
        for (auto& [id, b] : ab.items())
            action_buttons[static_cast<Action>(std::atoi(id.c_str()))] = static_cast<SDL_GameControllerButton>(b.get<int>());
    OpenControllers();
}

void Input::Save() {
    nlohmann::json k = nlohmann::json::object(), ak = nlohmann::json::object();
    nlohmann::json pb = nlohmann::json::object(), ab = nlohmann::json::object();
    for (auto& [id, sc] : keys) k[std::to_string(id)] = static_cast<int>(sc);
    for (auto& [a, sc] : action_keys) ak[std::to_string(static_cast<int>(a))] = static_cast<int>(sc);
    for (auto& [id, b] : pad_buttons) pb[std::to_string(id)] = static_cast<int>(b);
    for (auto& [a, b] : action_buttons) ab[std::to_string(static_cast<int>(a))] = static_cast<int>(b);
    auto& s = Settings::Get();
    s.Set("pc_keys", k);
    s.Set("pc_action_keys", ak);
    s.Set("pc_pad", pb);
    s.Set("pc_pad_actions", ab);
}

void Input::OpenControllers() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        SDL_GameController* c = SDL_GameControllerOpen(i);
        if (c)
            pads_.push_back(c);
    }
}

std::string Input::pad_name() const {
    for (auto* p : pads_)
        if (SDL_GameControllerGetAttached(p))
            return SDL_GameControllerName(p) ? SDL_GameControllerName(p) : "Controle";
    return "";
}

Input::Event Input::Handle(const SDL_Event& e) {
    Event out;
    switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which);
            if (c)
                pads_.push_back(c);
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            pad_mask_ = 0;
            pad_lx_ = pad_ly_ = 0;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool down = e.type == SDL_KEYDOWN;
            pad_active_ = false;
            const SDL_Scancode sc = e.key.keysym.scancode;
            for (auto& [id, k] : keys)
                if (k == sc) {
                    if (down) key_mask_ |= 1u << id; else key_mask_ &= ~(1u << id);
                }
            for (auto& [a, k] : action_keys)
                if (k == sc && (!down || e.key.repeat == 0) && (down || a == Action::kRewind)) {
                    out.action = a;
                    out.pressed = down;
                }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            pad_active_ = true;
            const auto b = static_cast<SDL_GameControllerButton>(e.cbutton.button);
            for (auto& [id, pb] : pad_buttons)
                if (pb == b) {
                    if (down) pad_mask_ |= 1u << id; else pad_mask_ &= ~(1u << id);
                }
            for (auto& [a, pb] : action_buttons)
                if (pb == b && (down || a == Action::kRewind)) {
                    out.action = a;
                    out.pressed = down;
                }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            const int v = e.caxis.value;
            if (std::abs(v) > 12000)
                pad_active_ = true;
            switch (e.caxis.axis) {
                case SDL_CONTROLLER_AXIS_LEFTX: pad_lx_ = static_cast<int16_t>(v); break;
                case SDL_CONTROLLER_AXIS_LEFTY: pad_ly_ = static_cast<int16_t>(v); break;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: l2_down_ = v > 16000; break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: r2_down_ = v > 16000; break;
                default: break;
            }
            break;
        }
        default:
            break;
    }
    return out;
}

uint32_t Input::buttons() const {
    uint32_t m = key_mask_ | pad_mask_;
    if (l2_down_) m |= 1u << kL2;
    if (r2_down_) m |= 1u << kR2;
    if (stick_as_dpad) {
        const int dz = 16000;
        if (pad_lx_ < -dz) m |= 1u << kLeft;
        if (pad_lx_ > dz) m |= 1u << kRight;
        if (pad_ly_ < -dz) m |= 1u << kUp;
        if (pad_ly_ > dz) m |= 1u << kDown;
    }
    return m;
}

void Input::stick(int16_t* x, int16_t* y) const {
    *x = pad_lx_;
    *y = pad_ly_;
    // Keyboard arrows also move the stick (PlayStation analog, 3DS circle pad).
    if (*x == 0 && *y == 0) {
        if (key_mask_ & (1u << kLeft)) *x = -32767;
        if (key_mask_ & (1u << kRight)) *x = 32767;
        if (key_mask_ & (1u << kUp)) *y = -32767;
        if (key_mask_ & (1u << kDown)) *y = 32767;
    }
}

}  // namespace cartucho
