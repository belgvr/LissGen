#include <SDL2/SDL.h>
#include <portaudio.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <GL/gl3w.h>
#include <vector>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")
#endif

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 4096
#define BUFFER_SIZE 4096
#define PI 3.14159265358979323846

const char* vertexShaderSource = R"(#version 330 core
    layout (location = 0) in vec2 aPos; layout (location = 1) in vec4 aColor;
    out vec4 vertexColor; uniform mat4 projection;
    void main() { gl_Position = projection * vec4(aPos.x, aPos.y, 0.0, 1.0); vertexColor = aColor; })";
const char* fragmentShaderSource = R"(#version 330 core
    out vec4 FragColor; in vec4 vertexColor;
    void main() { FragColor = vertexColor; })";

enum WaveType { SINE, SQUARE, SAWTOOTH };

struct FrequencyRow {
    float freq;
    bool muted;
    double phase = 0.0;
    WaveType type = SINE;
    FrequencyRow(float f) : freq(f), muted(false) {}
};

struct WavePreset {
    std::vector<FrequencyRow> freqsL;
    std::vector<FrequencyRow> freqsR;
};

struct PlaylistItem {
    WavePreset preset;
    float duration = 5.0f;
};

struct AudioState {
    std::vector<FrequencyRow> channelL, channelR;
    std::vector<float> lissajousL, lissajousR;
    std::mutex bufferMutex;
    int trailPercent = 100, targetFPS = 240;
    bool running = false, shiftPressed = false, ctrlPressed = false;
    bool showStartEndPoints = false, audioMuted = false;
    std::vector<PlaylistItem> playlist;
    int currentPlaylistItem = -1;
    float playlistTimer = 0.0f;
    bool playlistPlaying = false, loopPlaylist = true;
    std::string currentWaveFile = "Untitled.lsj";
    std::string currentPlaylistFile = "Untitled.lsjp";
    char waveTextBuffer[2048] = { 0 };
    std::string parseErrorMsg;
    bool waveDataIsDirty = true;
    bool showHelpWindow = false;
};

struct DragPayload {
    int sourceIndex;
    char sourceChannel;
};

void saveWaveToFile(const std::string& path, AudioState& state);
void loadWaveFromFile(const std::string& path, AudioState& state);
void savePlaylistToFile(const std::string& path, AudioState& state);
void loadPlaylistFromFile(const std::string& path, AudioState& state);
void formatWaveToTextBuffer(AudioState& state);
bool parseTextBufferToWave(AudioState& state);
void loadPlaylistItem(AudioState& state, int index);
GLuint createShaderProgram();
float getStep(bool shift, bool ctrl);
int audioCallback(const void* inputBuffer, void* outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData);
void drawLissajousGL(AudioState& state, int x, int y, int width, int height, GLuint shaderProgram, GLuint vao, GLuint vbo);
#ifdef _WIN32
std::string openFileDialog(const char* filter, const char* defExt);
std::string saveFileDialog(const char* filter, const char* defExt);
#endif

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0); SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* window = SDL_CreateWindow("Lissajous Generator C++ [GPU Accelerated]", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 850, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window); SDL_GL_MakeCurrent(window, gl_context); SDL_GL_SetSwapInterval(0);
    if (gl3wInit()) { fprintf(stderr, "failed to initialize OpenGL\n"); return -1; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); ImGui::StyleColorsDark(); ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f; style.FrameRounding = 4.0f; style.GrabRounding = 4.0f; style.WindowBorderSize = 0.0f; style.FrameBorderSize = 0.0f;
    ImVec4* colors = style.Colors; colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f); colors[ImGuiCol_Border] = ImVec4(0.2f, 0.3f, 0.4f, 0.5f); colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f); colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f); colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.20f, 0.25f, 1.0f); colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.0f); colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.24f, 1.0f); colors[ImGuiCol_Button] = ImVec4(0.15f, 0.30f, 0.45f, 1.0f); colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.0f); colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.25f, 0.40f, 1.0f); colors[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.50f, 0.80f, 1.0f); colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.60f, 0.90f, 1.0f); colors[ImGuiCol_Header] = ImVec4(0.15f, 0.30f, 0.45f, 1.0f); colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.0f); colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.35f, 0.55f, 1.0f);
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context); ImGui_ImplOpenGL3_Init("#version 330");
    AudioState state; state.channelL.push_back(FrequencyRow(60.0f)); state.channelR.push_back(FrequencyRow(61.0f));
    Pa_Initialize(); PaStream* stream;
    Pa_OpenDefaultStream(&stream, 0, 2, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, audioCallback, &state);
    GLuint shaderProgram = createShaderProgram(); GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    size_t stride = 6 * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)0); glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(2 * sizeof(float))); glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindVertexArray(0);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_LINE_SMOOTH); glEnable(GL_PROGRAM_POINT_SIZE);

    bool quit = false; SDL_Event event; Uint32 lastTime = SDL_GetTicks();

    while (!quit) {
        Uint32 currentTime = SDL_GetTicks(); Uint32 frameTime = 1000 / state.targetFPS; Uint32 elapsed = currentTime - lastTime;
        if (elapsed < frameTime) { SDL_Delay(frameTime - elapsed); } lastTime = SDL_GetTicks();
        while (SDL_PollEvent(&event)) { ImGui_ImplSDL2_ProcessEvent(&event); if (event.type == SDL_QUIT) quit = true; if (event.type == SDL_KEYDOWN) { if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) state.shiftPressed = true; if (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL) state.ctrlPressed = true; } if (event.type == SDL_KEYUP) { if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) state.shiftPressed = false; if (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL) state.ctrlPressed = false; } }

        if (state.playlistPlaying && state.running && !state.playlist.empty()) {
            state.playlistTimer -= io.DeltaTime;
            if (state.playlistTimer <= 0.0f) {
                state.currentPlaylistItem++;
                if (state.currentPlaylistItem >= (int)state.playlist.size()) {
                    if (state.loopPlaylist) state.currentPlaylistItem = 0;
                    else { state.playlistPlaying = false; state.currentPlaylistItem = -1; }
                }
                if (state.playlistPlaying) {
                    loadPlaylistItem(state, state.currentPlaylistItem);
                    state.playlistTimer = state.playlist[state.currentPlaylistItem].duration;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();

        if (state.showHelpWindow) {
            ImGui::Begin("Help and Credits", &state.showHelpWindow);
            ImGui::TextWrapped("This is an audio and visual generator based on Lissajous Curves.");
            ImGui::Separator();
            if (ImGui::CollapsingHeader("General Controls")) {
                ImGui::BulletText("Play/Stop: Starts or stops the audio and visual generation.");
                ImGui::BulletText("Mute Audio: Mutes the sound but keeps the visualization.");
                ImGui::BulletText("Step: Shows the frequency increment. Hold Shift (0.1) or Ctrl+Shift (0.01) for fine-tuning.");
            }
            if (ImGui::CollapsingHeader("Frequency Channels (L and R)")) {
                ImGui::BulletText("Bulk operations (+ All, - All, x2 All, /2 All): Apply operation to ALL frequencies in that channel.");
                ImGui::BulletText("Controls (+, -, x2, /2): Change the row's frequency.");
                ImGui::BulletText("Waveform Buttons (S, Q, W): Select the waveform type: (S)ine, (Q)uare, or sa(W)tooth.");
                ImGui::BulletText("M: Mutes only the frequency of that row.");
                ImGui::BulletText("D: Duplicates the row.");
                ImGui::BulletText("X: Deletes the row.");
            }
            if (ImGui::CollapsingHeader("Drag-and-Drop")) {
                ImGui::BulletText("Use the ':::' grip to drag a frequency.");
                ImGui::BulletText("Dragging onto another row IN THE SAME CHANNEL: Reorders the frequencies.");
                ImGui::BulletText("Dragging onto the header of the OTHER CHANNEL: Moves the frequency to the other channel.");
                ImGui::BulletText("Holding SHIFT while dragging to the other channel: CLONES (copies) the frequency instead of moving it.");
            }
            if (ImGui::CollapsingHeader("Text Editor")) {
                ImGui::BulletText("The text box below the controls allows for direct editing.");
                ImGui::BulletText("Use the format: L:{S440,Q220(M),...} R:{...}");
                ImGui::BulletText("S, Q, W are the prefixes for the waveform type. (M) indicates it is muted.");
                ImGui::BulletText("Click 'Apply Text' for the changes to take effect.");
                ImGui::BulletText("The program will warn you if there is a syntax error in the text.");
            }
            if (ImGui::CollapsingHeader("Playlist")) {
                ImGui::BulletText("Allows creating a sequence of waves with different durations.");
                ImGui::BulletText("'Add Current -> Playlist' adds the current configuration to the list.");
            }
            ImGui::Separator();
            ImGui::Text("Credits:");
            ImGui::Text("By: belgvr");
            ImGui::End();
        }

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(600, 830), ImGuiCond_FirstUseEver);
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoCollapse);

        if (state.running) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(150 / 255.0f, 0 / 255.0f, 0 / 255.0f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(180 / 255.0f, 30 / 255.0f, 30 / 255.0f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(130 / 255.0f, 0 / 255.0f, 0 / 255.0f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(120, 40))) { Pa_StopStream(stream); state.running = false; state.playlistPlaying = false; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops the audio and visual generation.");
            ImGui::PopStyleColor(3);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(50 / 255.0f, 130 / 255.0f, 0 / 255.0f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(70 / 255.0f, 160 / 255.0f, 20 / 255.0f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(40 / 255.0f, 110 / 255.0f, 0 / 255.0f, 1.0f));
            if (ImGui::Button("Play", ImVec2(120, 40))) { for (auto& row : state.channelL) row.phase = 0.0; for (auto& row : state.channelR) row.phase = 0.0; Pa_StartStream(stream); state.running = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts the audio and visual generation.");
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine(); ImGui::Text("  Step: %.2fHz %s", getStep(state.shiftPressed, state.ctrlPressed), state.ctrlPressed && state.shiftPressed ? "(Ctrl+Shift)" : state.shiftPressed ? "(Shift)" : "");

        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.6f, 1.0f));
        if (ImGui::Button("Help", ImVec2(70, 40))) { state.showHelpWindow = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the help and credits window.");
        ImGui::PopStyleColor();

        ImGui::Separator();

        // LEFT CHANNEL
        bool leftHeaderOpen = ImGui::CollapsingHeader("Left Channel (X)", ImGuiTreeNodeFlags_DefaultOpen);

        // Drop target for the LEFT header (always active)
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FREQ_ROW")) {
                const DragPayload& payload_n = *(const DragPayload*)payload->Data;
                if (payload_n.sourceChannel == 'R') {
                    state.channelL.push_back(state.channelR[payload_n.sourceIndex]);
                    if (!state.shiftPressed) { state.channelR.erase(state.channelR.begin() + payload_n.sourceIndex); }
                    state.waveDataIsDirty = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Bulk operations OUTSIDE collapsing header
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.45f, 1.0f));

        if (ImGui::Button("+ All##L", ImVec2(60, 0))) {
            for (auto& row : state.channelL) row.freq += getStep(state.shiftPressed, state.ctrlPressed);
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add step to ALL frequencies in Left channel");

        ImGui::SameLine();
        if (ImGui::Button("- All##L", ImVec2(60, 0))) {
            for (auto& row : state.channelL) row.freq -= getStep(state.shiftPressed, state.ctrlPressed);
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtract step from ALL frequencies in Left channel");

        ImGui::SameLine();
        if (ImGui::Button("x2 All##L", ImVec2(60, 0))) {
            for (auto& row : state.channelL) row.freq *= 2.0f;
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply ALL frequencies by 2 (up one octave)");

        ImGui::SameLine();
        if (ImGui::Button("/2 All##L", ImVec2(60, 0))) {
            for (auto& row : state.channelL) row.freq /= 2.0f;
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide ALL frequencies by 2 (down one octave)");
        ImGui::PopStyleColor(3);

        if (leftHeaderOpen) {
            for (int i = 0; i < (int)state.channelL.size(); i++) {
                ImGui::PushID(i);
                ImGui::Button(":::");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to reorder in the same channel.\nDrag to the other channel's header to move.\nHold SHIFT while dragging to copy it to the opposite channel");
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    DragPayload payload_data = { i, 'L' };
                    ImGui::SetDragDropPayload("FREQ_ROW", &payload_data, sizeof(DragPayload));
                    ImGui::Text("Move %.2f Hz (%c)", state.channelL[i].freq, "SQW"[(int)state.channelL[i].type]);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FREQ_ROW")) {
                        const DragPayload& payload_n = *(const DragPayload*)payload->Data;
                        if (payload_n.sourceChannel == 'L' && payload_n.sourceIndex != i) {
                            FrequencyRow temp = state.channelL[payload_n.sourceIndex];
                            state.channelL.erase(state.channelL.begin() + payload_n.sourceIndex);
                            state.channelL.insert(state.channelL.begin() + i, temp);
                            state.waveDataIsDirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                if (ImGui::InputFloat("Hz", &state.channelL[i].freq, 0.0f, 0.0f, "%.3f")) state.waveDataIsDirty = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frequency in Hertz for this oscillator.");
                ImGui::SameLine();
                if (ImGui::Button("+")) { state.channelL[i].freq += getStep(state.shiftPressed, state.ctrlPressed); state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increase frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");
                ImGui::SameLine();
                if (ImGui::Button("-")) { state.channelL[i].freq -= getStep(state.shiftPressed, state.ctrlPressed); state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrease frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");
                ImGui::SameLine();
                if (ImGui::Button("x2")) { state.channelL[i].freq *= 2.0f; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply frequency by 2 (goes up one octave).");
                ImGui::SameLine();
                if (ImGui::Button("/2")) { state.channelL[i].freq /= 2.0f; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide frequency by 2 (goes down one octave).");
                ImGui::SameLine();
                bool isSine = state.channelL[i].type == SINE;
                if (isSine) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                if (ImGui::Button("S")) { state.channelL[i].type = SINE; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Sine.");
                if (isSine) ImGui::PopStyleColor();
                ImGui::SameLine(0, 2);
                bool isSquare = state.channelL[i].type == SQUARE;
                if (isSquare) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Q")) { state.channelL[i].type = SQUARE; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Square.");
                if (isSquare) ImGui::PopStyleColor();
                ImGui::SameLine(0, 2);
                bool isSaw = state.channelL[i].type == SAWTOOTH;
                if (isSaw) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.6f, 1.0f));
                if (ImGui::Button("W")) { state.channelL[i].type = SAWTOOTH; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Sawtooth.");
                if (isSaw) ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Checkbox("M", &state.channelL[i].muted)) state.waveDataIsDirty = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute only this frequency.");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(50 / 255.0f, 130 / 255.0f, 0 / 255.0f, 1.0f));
                if (ImGui::Button("D")) { state.channelL.insert(state.channelL.begin() + i + 1, state.channelL[i]); state.waveDataIsDirty = true; ImGui::PopStyleColor(); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate this frequency row.");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("X")) { state.channelL.erase(state.channelL.begin() + i); state.waveDataIsDirty = true; ImGui::PopStyleColor(); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this frequency row.");
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (ImGui::Button("+ Add Frequency##L")) { state.channelL.push_back(FrequencyRow(440.0f)); state.waveDataIsDirty = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new oscillator to this channel.");
        }
        ImGui::Separator();

        // RIGHT CHANNEL
        bool rightHeaderOpen = ImGui::CollapsingHeader("Right Channel (Y)", ImGuiTreeNodeFlags_DefaultOpen);

        // Drop target for the RIGHT header (always active)
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FREQ_ROW")) {
                const DragPayload& payload_n = *(const DragPayload*)payload->Data;
                if (payload_n.sourceChannel == 'L') {
                    state.channelR.push_back(state.channelL[payload_n.sourceIndex]);
                    if (!state.shiftPressed) { state.channelL.erase(state.channelL.begin() + payload_n.sourceIndex); }
                    state.waveDataIsDirty = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Bulk operations OUTSIDE collapsing header
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.4f, 0.4f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.25f, 0.25f, 1.0f));

        if (ImGui::Button("+ All##R", ImVec2(60, 0))) {
            for (auto& row : state.channelR) row.freq += getStep(state.shiftPressed, state.ctrlPressed);
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add step to ALL frequencies in Right channel");

        ImGui::SameLine();
        if (ImGui::Button("- All##R", ImVec2(60, 0))) {
            for (auto& row : state.channelR) row.freq -= getStep(state.shiftPressed, state.ctrlPressed);
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtract step from ALL frequencies in Right channel");

        ImGui::SameLine();
        if (ImGui::Button("x2 All##R", ImVec2(60, 0))) {
            for (auto& row : state.channelR) row.freq *= 2.0f;
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply ALL frequencies by 2 (up one octave)");

        ImGui::SameLine();
        if (ImGui::Button("/2 All##R", ImVec2(60, 0))) {
            for (auto& row : state.channelR) row.freq /= 2.0f;
            state.waveDataIsDirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide ALL frequencies by 2 (down one octave)");
        ImGui::PopStyleColor(3);

        if (rightHeaderOpen) {
            for (int i = 0; i < (int)state.channelR.size(); i++) {
                ImGui::PushID(1000 + i);
                ImGui::Button(":::");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to reorder in the same channel.\nDrag to the other channel's header to move.\nHold SHIFT while dropping to clone.");
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    DragPayload payload_data = { i, 'R' };
                    ImGui::SetDragDropPayload("FREQ_ROW", &payload_data, sizeof(DragPayload));
                    ImGui::Text("Move %.2f Hz (%c)", state.channelR[i].freq, "SQW"[(int)state.channelR[i].type]);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FREQ_ROW")) {
                        const DragPayload& payload_n = *(const DragPayload*)payload->Data;
                        if (payload_n.sourceChannel == 'R' && payload_n.sourceIndex != i) {
                            FrequencyRow temp = state.channelR[payload_n.sourceIndex];
                            state.channelR.erase(state.channelR.begin() + payload_n.sourceIndex);
                            state.channelR.insert(state.channelR.begin() + i, temp);
                            state.waveDataIsDirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine(); ImGui::SetNextItemWidth(80);
                if (ImGui::InputFloat("Hz", &state.channelR[i].freq, 0.0f, 0.0f, "%.3f")) state.waveDataIsDirty = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frequency in Hertz for this oscillator.");
                ImGui::SameLine();
                if (ImGui::Button("+")) { state.channelR[i].freq += getStep(state.shiftPressed, state.ctrlPressed); state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increase frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");
                ImGui::SameLine();
                if (ImGui::Button("-")) { state.channelR[i].freq -= getStep(state.shiftPressed, state.ctrlPressed); state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrease frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");
                ImGui::SameLine();
                if (ImGui::Button("x2")) { state.channelR[i].freq *= 2.0f; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply frequency by 2 (goes up one octave).");
                ImGui::SameLine();
                if (ImGui::Button("/2")) { state.channelR[i].freq /= 2.0f; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide frequency by 2 (goes down one octave).");
                ImGui::SameLine();
                bool isSine = state.channelR[i].type == SINE;
                if (isSine) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                if (ImGui::Button("S")) { state.channelR[i].type = SINE; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Sine.");
                if (isSine) ImGui::PopStyleColor();
                ImGui::SameLine(0, 2);
                bool isSquare = state.channelR[i].type == SQUARE;
                if (isSquare) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Q")) { state.channelR[i].type = SQUARE; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Square.");
                if (isSquare) ImGui::PopStyleColor();
                ImGui::SameLine(0, 2);
                bool isSaw = state.channelR[i].type == SAWTOOTH;
                if (isSaw) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.6f, 1.0f));
                if (ImGui::Button("W")) { state.channelR[i].type = SAWTOOTH; state.waveDataIsDirty = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set waveform to Sawtooth.");
                if (isSaw) ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Checkbox("M", &state.channelR[i].muted)) state.waveDataIsDirty = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute only this frequency.");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(50 / 255.0f, 130 / 255.0f, 0 / 255.0f, 1.0f));
                if (ImGui::Button("D")) { state.channelR.insert(state.channelR.begin() + i + 1, state.channelR[i]); state.waveDataIsDirty = true; ImGui::PopStyleColor(); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate this frequency row.");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("X")) { state.channelR.erase(state.channelR.begin() + i); state.waveDataIsDirty = true; ImGui::PopStyleColor(); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this frequency row.");
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (ImGui::Button("+ Add Frequency##R")) { state.channelR.push_back(FrequencyRow(440.0f)); state.waveDataIsDirty = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new oscillator to this channel.");
        }
        ImGui::Separator();

        ImGui::SliderInt("Trail %", &state.trailPercent, 1, 100);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Defines the length of the wave's trail.");
        ImGui::Separator();

        ImGui::Checkbox("Show Start/End Points", &state.showStartEndPoints);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show/hide the red (start) and white (end) points of the trail.");
        ImGui::Separator();

        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(255.0f / 255.0f, 80.0f / 255.0f, 0.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(255.0f / 255.0f, 110.0f / 255.0f, 0.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(230.0f / 255.0f, 70.0f / 255.0f, 0.0f / 255.0f, 1.0f));
            if (ImGui::Button("Swap Channels (X <-> Y)")) { std::swap(state.channelL, state.channelR); state.waveDataIsDirty = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap all frequencies between channel X and channel Y.");
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();
        if (state.audioMuted) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f)); }
        ImGui::Checkbox("Mute Audio", &state.audioMuted);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Completely mutes the audio output, keeping the visualization.");
        if (state.audioMuted) { ImGui::PopStyleColor(); }
        ImGui::Separator();

        ImGui::Text("FPS: %.1f / %d", io.Framerate, state.targetFPS);
        ImGui::SliderInt("Target FPS", &state.targetFPS, 60, 480);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the target FPS for rendering.\nHigher values may result in smoother animation.");

        ImGui::Separator();
        ImGui::BeginChild("Status", ImVec2(0, 180), false, ImGuiWindowFlags_None);

        if (state.waveDataIsDirty) {
            formatWaveToTextBuffer(state);
            state.waveDataIsDirty = false;
        }

        ImGui::InputTextMultiline("##WaveEditor", state.waveTextBuffer, sizeof(state.waveTextBuffer),
            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4), ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Directly edit the wave configuration here.\nFormat: L:{S440,Q220(M),...}\nThen press 'Apply Text'.");

        if (ImGui::Button("Apply Text")) { if (parseTextBufferToWave(state)) state.waveDataIsDirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parse the text above and apply changes to the wave.");

        ImGui::SameLine();
        if (ImGui::Button("Save Wave")) {
#ifdef _WIN32
            std::string path = saveFileDialog("Lissajous Wave (*.lsj)\0*.lsj\0All Files (*.*)\0*.*\0", "lsj");
            if (!path.empty()) saveWaveToFile(path, state);
#endif
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the current wave configuration to a .lsj file.");

        ImGui::SameLine();
        if (ImGui::Button("Load Wave")) {
#ifdef _WIN32
            std::string path = openFileDialog("Lissajous Wave (*.lsj)\0*.lsj\0All Files (*.*)\0*.*\0", "lsj");
            if (!path.empty()) loadWaveFromFile(path, state);
#endif
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load a wave configuration from a .lsj file.");

        if (!state.parseErrorMsg.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("Error: %s", state.parseErrorMsg.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Playlist", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button(state.playlistPlaying ? "Stop Playlist" : "Play Playlist")) {
                if (!state.playlist.empty()) {
                    state.playlistPlaying = !state.playlistPlaying;
                    if (state.playlistPlaying) {
                        if (!state.running) { Pa_StartStream(stream); state.running = true; }
                        state.currentPlaylistItem = 0;
                        loadPlaylistItem(state, 0);
                        state.playlistTimer = state.playlist[0].duration;
                    }
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts or stops the playlist sequence.");

            ImGui::SameLine();
            ImGui::Checkbox("Loop", &state.loopPlaylist);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If checked, the playlist will loop back to the start when it finishes.");

            ImGui::SameLine();
            if (ImGui::Button("Add Current -> Playlist")) {
                PlaylistItem newItem;
                newItem.preset.freqsL = state.channelL;
                newItem.preset.freqsR = state.channelR;
                state.playlist.push_back(newItem);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds the current wave configuration as a new item in the playlist.");
            ImGui::Separator();

            for (int i = 0; i < (int)state.playlist.size(); ++i) {
                ImGui::PushID(2000 + i);
                bool isCurrent = state.playlistPlaying && i == state.currentPlaylistItem;
                if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));

                std::stringstream label;
                label.precision(1);
                label << std::fixed << "Item " << i << " (" << state.playlist[i].duration << "s)";

                if (ImGui::CollapsingHeader(label.str().c_str())) {
                    ImGui::SliderFloat("Duration (s)", &state.playlist[i].duration, 0.1f, 60.0f, "%.2f s");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets how long this playlist item will play.");
                    ImGui::Separator();
                    if (ImGui::Button("Remove")) { state.playlist.erase(state.playlist.begin() + i); ImGui::PopID(); break; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this item from the playlist.");
                    ImGui::SameLine();
                    if (ImGui::Button("Up") && i > 0) { std::swap(state.playlist[i], state.playlist[i - 1]); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move this item up in the playlist order.");
                    ImGui::SameLine();
                    if (ImGui::Button("Down") && i < (int)state.playlist.size() - 1) { std::swap(state.playlist[i], state.playlist[i + 1]); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move this item down in the playlist order.");
                }

                if (isCurrent) ImGui::PopStyleColor();
                ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Button("Save Playlist")) {
#ifdef _WIN32
                std::string path = saveFileDialog("Lissajous Playlist (*.lsjp)\0*.lsjp\0All Files (*.*)\0*.*\0", "lsjp");
                if (!path.empty()) savePlaylistToFile(path, state);
#endif
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the entire playlist to a .lsjp file.");

            ImGui::SameLine();
            if (ImGui::Button("Load Playlist")) {
#ifdef _WIN32
                std::string path = openFileDialog("Lissajous Playlist (*.lsjp)\0*.lsjp\0All Files (*.*)\0*.*\0", "lsjp");
                if (!path.empty()) loadPlaylistFromFile(path, state);
#endif
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load a playlist from a .lsjp file.");

            ImGui::SameLine();
            if (ImGui::Button("Clear Playlist")) { state.playlist.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes all items from the current playlist.");
        }

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y); glClearColor(0.0f, 0.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        int w, h; SDL_GetWindowSize(window, &w, &h);
        int lissajous_size = (std::min)(w - 620, h - 90);
        drawLissajousGL(state, 620, 80, lissajous_size, lissajous_size, shaderProgram, vao, vbo);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    if (state.running) Pa_StopStream(stream); Pa_CloseStream(stream); Pa_Terminate();
    glDeleteVertexArrays(1, &vao); glDeleteBuffers(1, &vbo); glDeleteProgram(shaderProgram);
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}

#ifdef _WIN32
std::string openFileDialog(const char* filter, const char* defExt) {
    OPENFILENAMEA ofn; CHAR szFile[260] = { 0 }; ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME); ofn.hwndOwner = NULL; ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile); ofn.lpstrFilter = filter; ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = defExt;
    if (GetOpenFileNameA(&ofn) == TRUE) return ofn.lpstrFile; return std::string();
}
std::string saveFileDialog(const char* filter, const char* defExt) {
    OPENFILENAMEA ofn; CHAR szFile[260] = { 0 }; ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME); ofn.hwndOwner = NULL; ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile); ofn.lpstrFilter = filter; ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = defExt;
    if (GetSaveFileNameA(&ofn) == TRUE) return ofn.lpstrFile; return std::string();
}
#endif

std::string formatRowToString(const FrequencyRow& row) {
    std::stringstream ss;
    ss.precision(3); ss << std::fixed;
    if (row.type == SQUARE) ss << "Q"; else if (row.type == SAWTOOTH) ss << "W"; else ss << "S";
    ss << row.freq;
    if (row.muted) ss << "(M)";
    return ss.str();
}

FrequencyRow parseRowFromString(const std::string& token_str) {
    std::string token = token_str;
    bool isMuted = token.find("(M)") != std::string::npos;
    if (isMuted) token.erase(token.find("(M)"));
    WaveType type = SINE;
    if (!token.empty()) {
        char firstChar = (char)toupper(token[0]);
        if (firstChar == 'Q') { type = SQUARE; token = token.substr(1); }
        else if (firstChar == 'W') { type = SAWTOOTH; token = token.substr(1); }
        else if (firstChar == 'S') { type = SINE; token = token.substr(1); }
    }
    FrequencyRow newRow(std::stof(token));
    newRow.muted = isMuted;
    newRow.type = type;
    return newRow;
}

void saveWaveToFile(const std::string& path, AudioState& state) {
    std::ofstream file(path); if (!file.is_open()) return;
    file << "L:";
    for (size_t i = 0; i < state.channelL.size(); ++i) { file << formatRowToString(state.channelL[i]) << (i < state.channelL.size() - 1 ? "," : ""); }
    file << "\n";
    file << "R:";
    for (size_t i = 0; i < state.channelR.size(); ++i) { file << formatRowToString(state.channelR[i]) << (i < state.channelR.size() - 1 ? "," : ""); }
    file << "\n";
    state.currentWaveFile = path;
}

void loadWaveFromFile(const std::string& path, AudioState& state) {
    std::ifstream file(path); if (!file.is_open()) return;
    state.channelL.clear(); state.channelR.clear();
    std::string line;
    while (std::getline(file, line)) {
        std::vector<FrequencyRow>* targetChannel = nullptr;
        std::string data;
        if (line.rfind("L:", 0) == 0) { targetChannel = &state.channelL; data = line.substr(2); }
        else if (line.rfind("R:", 0) == 0) { targetChannel = &state.channelR; data = line.substr(2); }
        if (targetChannel) {
            std::stringstream ss(data); std::string token;
            while (std::getline(ss, token, ',')) {
                if (token.empty()) continue;
                try { targetChannel->push_back(parseRowFromString(token)); }
                catch (...) {}
            }
        }
    }
    state.currentWaveFile = path; state.waveDataIsDirty = true;
}

void savePlaylistToFile(const std::string& path, AudioState& state) {
    std::ofstream file(path); if (!file.is_open()) return;
    for (const auto& item : state.playlist) {
        file << "ITEM\n";
        file << "DURATION: " << item.duration << "\n";
        file << "L:";
        for (size_t i = 0; i < item.preset.freqsL.size(); ++i) { file << formatRowToString(item.preset.freqsL[i]) << (i < item.preset.freqsL.size() - 1 ? "," : ""); }
        file << "\n";
        file << "R:";
        for (size_t i = 0; i < item.preset.freqsR.size(); ++i) { file << formatRowToString(item.preset.freqsR[i]) << (i < item.preset.freqsR.size() - 1 ? "," : ""); }
        file << "\n";
    }
    state.currentPlaylistFile = path;
}

void loadPlaylistFromFile(const std::string& path, AudioState& state) {
    std::ifstream file(path); if (!file.is_open()) return;
    state.playlist.clear();
    std::string line;
    PlaylistItem currentItem;
    bool itemInProgress = false;
    auto finalizeItem = [&](PlaylistItem& item) {
        if (!item.preset.freqsL.empty() || !item.preset.freqsR.empty()) {
            state.playlist.push_back(item);
        }
        };
    while (std::getline(file, line)) {
        if (line.rfind("ITEM", 0) == 0) {
            if (itemInProgress) finalizeItem(currentItem);
            currentItem = PlaylistItem();
            itemInProgress = true;
        }
        else if (line.rfind("DURATION:", 0) == 0) {
            try { currentItem.duration = std::stof(line.substr(10)); }
            catch (...) {}
        }
        else {
            std::vector<FrequencyRow>* targetChannel = nullptr;
            std::string data;
            if (line.rfind("L:", 0) == 0) { targetChannel = &currentItem.preset.freqsL; data = line.substr(2); }
            else if (line.rfind("R:", 0) == 0) { targetChannel = &currentItem.preset.freqsR; data = line.substr(2); }
            if (targetChannel) {
                std::stringstream ss(data); std::string token;
                while (std::getline(ss, token, ',')) {
                    if (token.empty()) continue;
                    try { targetChannel->push_back(parseRowFromString(token)); }
                    catch (...) {}
                }
            }
        }
    }
    if (itemInProgress) finalizeItem(currentItem);
    state.currentPlaylistFile = path;
}

void formatWaveToTextBuffer(AudioState& state) {
    std::stringstream ss; ss.precision(3); ss << std::fixed;
    ss << "L:{";
    for (size_t i = 0; i < state.channelL.size(); ++i) { ss << formatRowToString(state.channelL[i]) << (i < state.channelL.size() - 1 ? "," : ""); }
    ss << "}\nR:{";
    for (size_t i = 0; i < state.channelR.size(); ++i) { ss << formatRowToString(state.channelR[i]) << (i < state.channelR.size() - 1 ? "," : ""); }
    ss << "}";
    strncpy_s(state.waveTextBuffer, sizeof(state.waveTextBuffer), ss.str().c_str(), _TRUNCATE);
}

bool parseTextBufferToWave(AudioState& state) {
    state.parseErrorMsg.clear();
    std::vector<FrequencyRow> newChannelL, newChannelR;
    std::string text(state.waveTextBuffer);
    size_t l_start = text.find("L:{"); size_t l_end = text.find("}", l_start);
    size_t r_start = text.find("R:{"); size_t r_end = text.find("}", r_start);
    if (l_start == std::string::npos || l_end == std::string::npos || r_start == std::string::npos || r_end == std::string::npos) {
        state.parseErrorMsg = "Invalid format. Use L:{...} and R:{...}"; return false;
    }
    std::string l_data = text.substr(l_start + 3, l_end - (l_start + 3));
    std::stringstream lss(l_data); std::string l_token; int l_item = 1;
    while (std::getline(lss, l_token, ',')) {
        if (l_token.empty() || l_token.find_first_not_of(" \t\n\v\f\r") == std::string::npos) continue;
        try { newChannelL.push_back(parseRowFromString(l_token)); }
        catch (...) { state.parseErrorMsg = "Channel L, item " + std::to_string(l_item) + ": '" + l_token + "' is invalid."; return false; }
        l_item++;
    }
    std::string r_data = text.substr(r_start + 3, r_end - (r_start + 3));
    std::stringstream rss(r_data); std::string r_token; int r_item = 1;
    while (std::getline(rss, r_token, ',')) {
        if (r_token.empty() || r_token.find_first_not_of(" \t\n\v\f\r") == std::string::npos) continue;
        try { newChannelR.push_back(parseRowFromString(r_token)); }
        catch (...) { state.parseErrorMsg = "Channel R, item " + std::to_string(r_item) + ": '" + r_token + "' is invalid."; return false; }
        r_item++;
    }
    state.channelL = newChannelL; state.channelR = newChannelR;
    return true;
}

GLuint createShaderProgram() {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vertexShader, 1, &vertexShaderSource, NULL); glCompileShader(vertexShader);
    int success; char infoLog[512]; glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n" << infoLog << std::endl;
    }
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL); glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) { glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n" << infoLog << std::endl; }
    GLuint shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, vertexShader); glAttachShader(shaderProgram, fragmentShader); glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) { glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl; }
    glDeleteShader(vertexShader); glDeleteShader(fragmentShader); return shaderProgram;
}

float getStep(bool shift, bool ctrl) { if (ctrl && shift) return 0.01f; if (shift) return 0.1f; return 1.0f; }

int audioCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData) {
    AudioState* state = (AudioState*)userData;
    float* out = (float*)outputBuffer;
    for (unsigned long i = 0; i < framesPerBuffer; i++) {
        float sumL = 0.0f, sumR = 0.0f; int countL = 0, countR = 0;
        for (auto& row : state->channelL) {
            float sample = 0.0f;
            switch (row.type) {
            case SINE: sample = std::sin(row.phase); break;
            case SQUARE: sample = (row.phase < PI) ? 0.5f : -0.5f; break;
            case SAWTOOTH: sample = (row.phase / (float)PI) - 1.0f; break;
            }
            if (!row.muted) { sumL += sample; countL++; }
            double phaseIncrement = 2.0 * PI * row.freq / SAMPLE_RATE; row.phase += phaseIncrement;
            if (row.phase >= 2.0 * PI) { row.phase -= 2.0 * PI; }
        }
        for (auto& row : state->channelR) {
            float sample = 0.0f;
            switch (row.type) {
            case SINE: sample = std::sin(row.phase); break;
            case SQUARE: sample = (row.phase < PI) ? 0.5f : -0.5f; break;
            case SAWTOOTH: sample = (row.phase / (float)PI) - 1.0f; break;
            }
            if (!row.muted) { sumR += sample; countR++; }
            double phaseIncrement = 2.0 * PI * row.freq / SAMPLE_RATE; row.phase += phaseIncrement;
            if (row.phase >= 2.0 * PI) { row.phase -= 2.0 * PI; }
        }
        float sampleL = countL > 0 ? sumL / countL : 0.0f; float sampleR = countR > 0 ? sumR / countR : 0.0f;
        if (state->audioMuted) { *out++ = 0.0f; *out++ = 0.0f; }
        else { *out++ = sampleL * 0.5f; *out++ = sampleR * 0.5f; }
        if (i % 2 == 0) {
            std::lock_guard<std::mutex> lock(state->bufferMutex);
            state->lissajousL.push_back(sampleL); state->lissajousR.push_back(sampleR);
            if (state->lissajousL.size() > BUFFER_SIZE) { state->lissajousL.erase(state->lissajousL.begin()); state->lissajousR.erase(state->lissajousR.begin()); }
        }
    }
    return paContinue;
}

void drawLissajousGL(AudioState& state, int x, int y, int width, int height, GLuint shaderProgram, GLuint vao, GLuint vbo) {
    std::vector<float> dataL, dataR;
    { std::lock_guard<std::mutex> lock(state.bufferMutex); dataL = state.lissajousL; dataR = state.lissajousR; }
    if (dataL.size() < 2) return;
    glViewport(x, y, width, height); glUseProgram(shaderProgram); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    float left = 0.0f, right = (float)width, bottom = (float)height, top = 0.0f;
    float proj[16] = { 2 / (right - left),0,0,0, 0,2 / (top - bottom),0,0, 0,0,-2 / (1.f - -1.f),0, -(right + left) / (right - left),-(top + bottom) / (top - bottom),-(1.f - 1.f) / (1.f - -1.f),1 };
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "projection"), 1, GL_FALSE, proj);
    float centerX = width / 2.0f; float centerY = height / 2.0f; float scale = (std::min)(width, height) / 2.0f - 20.0f;
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 0.15f, 0.15f, 0.15f, 1.0f); glLineWidth(1.0f);
    float gridVertices[] = { 0, centerY, (float)width, centerY, centerX, 0, centerX, (float)height };
    glBufferData(GL_ARRAY_BUFFER, sizeof(gridVertices), gridVertices, GL_DYNAMIC_DRAW); glDrawArrays(GL_LINES, 0, 4);
    glVertexAttrib4f(1, 0.12f, 0.12f, 0.12f, 1.0f);
    std::vector<float> circleVertices;
    for (float r = scale / 4.0f; r <= scale; r += scale / 4.0f) {
        circleVertices.clear();
        for (int angle = 0; angle <= 360; angle += 5) {
            float rad = (float)angle * (float)PI / 180.0f;
            circleVertices.push_back(centerX + r * std::cos(rad)); circleVertices.push_back(centerY + r * std::sin(rad));
        }
        glBufferData(GL_ARRAY_BUFFER, circleVertices.size() * sizeof(float), circleVertices.data(), GL_DYNAMIC_DRAW); glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)circleVertices.size() / 2);
    }
    float maxVal = 0.001f;
    for (size_t i = 0; i < dataL.size(); i++) { maxVal = (std::max)(maxVal, std::abs(dataL[i])); maxVal = (std::max)(maxVal, std::abs(dataR[i])); }
    glEnableVertexAttribArray(1);
    size_t numPoints = (size_t)(dataL.size() * state.trailPercent / 100.0f); if (numPoints < 2) numPoints = 2; size_t start = dataL.size() > numPoints ? dataL.size() - numPoints : 0;
    std::vector<float> lissajousVertices; lissajousVertices.reserve(numPoints * 6);
    for (size_t i = start; i < dataL.size(); i++) {
        lissajousVertices.push_back(centerX + (dataL[i] / maxVal) * scale); lissajousVertices.push_back(centerY - (dataR[i] / maxVal) * scale);
        float progress = (numPoints > 1) ? (float)(i - start) / (float)(numPoints - 1) : 1.0f; float alpha = progress * progress;
        lissajousVertices.push_back(0.0f); lissajousVertices.push_back(1.0f); lissajousVertices.push_back(0.0f); lissajousVertices.push_back(alpha);
    }
    glLineWidth(2.0f); glBufferData(GL_ARRAY_BUFFER, lissajousVertices.size() * sizeof(float), lissajousVertices.data(), GL_DYNAMIC_DRAW); glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)lissajousVertices.size() / 6);
    glDisableVertexAttribArray(1);
    if (state.showStartEndPoints) {
        if (!lissajousVertices.empty()) {
            float startPoint[] = { lissajousVertices[0], lissajousVertices[1] }; glPointSize(8.0f);
            glVertexAttrib4f(1, 1.0f, 0.0f, 0.0f, 1.0f); glBufferData(GL_ARRAY_BUFFER, sizeof(startPoint), startPoint, GL_DYNAMIC_DRAW); glDrawArrays(GL_POINTS, 0, 1);
        }
        if (lissajousVertices.size() >= 6) {
            float endPoint[] = { lissajousVertices[lissajousVertices.size() - 6], lissajousVertices[lissajousVertices.size() - 5] }; glPointSize(10.0f);
            glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f); glBufferData(GL_ARRAY_BUFFER, sizeof(endPoint), endPoint, GL_DYNAMIC_DRAW); glDrawArrays(GL_POINTS, 0, 1);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindVertexArray(0);
}

void loadPlaylistItem(AudioState& state, int index) {
    if (index < 0 || index >= (int)state.playlist.size()) return;
    std::lock_guard<std::mutex> lock(state.bufferMutex);
    const auto& item = state.playlist[index];
    state.channelL = item.preset.freqsL;
    state.channelR = item.preset.freqsR;
    state.waveDataIsDirty = true;
}