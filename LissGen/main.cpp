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

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 4096
#define BUFFER_SIZE 4096
#define PI 3.14159265358979323846

// Shaders
const char* vertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec2 aPos;
    layout (location = 1) in vec4 aColor;
    out vec4 vertexColor;
    uniform mat4 projection;
    void main() {
        gl_Position = projection * vec4(aPos.x, aPos.y, 0.0, 1.0);
        vertexColor = aColor;
    }
)";
const char* fragmentShaderSource = R"(
    #version 330 core
    out vec4 FragColor;
    in vec4 vertexColor;
    void main() {
       FragColor = vertexColor;
    }
)";

// Estruturas de dados
struct FrequencyRow {
    float freq;
    bool muted;
    double phase = 0.0;

    FrequencyRow(float f) : freq(f), muted(false) {}
};

struct AudioState {
    std::vector<FrequencyRow> channelL;
    std::vector<FrequencyRow> channelR;
    std::vector<float> lissajousL;
    std::vector<float> lissajousR;
    std::mutex bufferMutex;
    int trailPercent = 100;
    int targetFPS = 240;
    bool running = false;
    bool shiftPressed = false;
    bool ctrlPressed = false;
    bool showStartEndPoints = true;
    // REMOVED: bool laserMode = false;
    bool audioMuted = false;
};

// Funções auxiliares
GLuint createShaderProgram() {
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vertexShader, 1, &vertexShaderSource, NULL); glCompileShader(vertexShader);
    int success; char infoLog[512]; glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) { glGetShaderInfoLog(vertexShader, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::VERTEX::COMPILATION_FAILED\n" << infoLog << std::endl; }
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL); glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) { glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n" << infoLog << std::endl; }
    GLuint shaderProgram = glCreateProgram(); glAttachShader(shaderProgram, vertexShader); glAttachShader(shaderProgram, fragmentShader); glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) { glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog); std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl; }
    glDeleteShader(vertexShader); glDeleteShader(fragmentShader); return shaderProgram;
}
float getStep(bool shift, bool ctrl) { if (ctrl && shift) return 0.01f; if (shift) return 0.1f; return 1.0f; }

// Callback de Áudio
int audioCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData) {
    AudioState* state = (AudioState*)userData;
    float* out = (float*)outputBuffer;
    for (unsigned long i = 0; i < framesPerBuffer; i++) {
        float sumL = 0.0f, sumR = 0.0f; int countL = 0, countR = 0;
        for (auto& row : state->channelL) {
            if (!row.muted) { sumL += std::sin(row.phase); countL++; }
            double phaseIncrement = 2.0 * PI * row.freq / SAMPLE_RATE; row.phase += phaseIncrement;
            if (row.phase >= 2.0 * PI) { row.phase -= 2.0 * PI; }
        }
        for (auto& row : state->channelR) {
            if (!row.muted) { sumR += std::sin(row.phase); countR++; }
            double phaseIncrement = 2.0 * PI * row.freq / SAMPLE_RATE; row.phase += phaseIncrement;
            if (row.phase >= 2.0 * PI) { row.phase -= 2.0 * PI; }
        }
        float sampleL = countL > 0 ? sumL / countL : 0.0f; float sampleR = countR > 0 ? sumR / countR : 0.0f;
        if (state->audioMuted) { *out++ = 0.0f; *out++ = 0.0f; }
        else { *out++ = sampleL * 0.3f; *out++ = sampleR * 0.3f; }
        if (i % 2 == 0) {
            std::lock_guard<std::mutex> lock(state->bufferMutex);
            state->lissajousL.push_back(sampleL); state->lissajousR.push_back(sampleR);
            if (state->lissajousL.size() > BUFFER_SIZE) { state->lissajousL.erase(state->lissajousL.begin()); state->lissajousR.erase(state->lissajousR.begin()); }
        }
    }
    return paContinue;
}

// Função de Desenho (Lógica do Laser Mode removida)
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
            float rad = angle * PI / 180.0f;
            circleVertices.push_back(centerX + r * std::cos(rad)); circleVertices.push_back(centerY + r * std::sin(rad));
        }
        glBufferData(GL_ARRAY_BUFFER, circleVertices.size() * sizeof(float), circleVertices.data(), GL_DYNAMIC_DRAW); glDrawArrays(GL_LINE_STRIP, 0, circleVertices.size() / 2);
    }
    float maxVal = 0.001f;
    for (size_t i = 0; i < dataL.size(); i++) { maxVal = (std::max)(maxVal, std::abs(dataL[i])); maxVal = (std::max)(maxVal, std::abs(dataR[i])); }

    // Lógica de desenho principal (antigo "else" do Laser Mode)
    glEnableVertexAttribArray(1);
    size_t numPoints = (size_t)(dataL.size() * state.trailPercent / 100.0f); if (numPoints < 2) numPoints = 2; size_t start = dataL.size() - numPoints;
    std::vector<float> lissajousVertices; lissajousVertices.reserve(numPoints * 6);
    for (size_t i = start; i < dataL.size(); i++) {
        lissajousVertices.push_back(centerX + (dataL[i] / maxVal) * scale); lissajousVertices.push_back(centerY - (dataR[i] / maxVal) * scale);
        float progress = (float)(i - start) / (float)(numPoints - 1); float alpha = progress * progress;
        lissajousVertices.push_back(0.0f); lissajousVertices.push_back(1.0f); lissajousVertices.push_back(0.0f); lissajousVertices.push_back(alpha);
    }
    glLineWidth(2.0f); glBufferData(GL_ARRAY_BUFFER, lissajousVertices.size() * sizeof(float), lissajousVertices.data(), GL_DYNAMIC_DRAW); glDrawArrays(GL_LINE_STRIP, 0, numPoints);
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

// Função Principal
int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0); SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window* window = SDL_CreateWindow("Lissajous Generator C++ [GPU Accelerated]", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window); SDL_GL_MakeCurrent(window, gl_context); SDL_GL_SetSwapInterval(0);
    if (gl3wInit()) { fprintf(stderr, "failed to initialize OpenGL\n"); return -1; }
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); ImGui::StyleColorsDark(); ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f; style.FrameRounding = 4.0f; style.GrabRounding = 4.0f; style.WindowBorderSize = 0.0f; style.FrameBorderSize = 0.0f;
    ImVec4* colors = style.Colors; colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f); colors[ImGuiCol_Border] = ImVec4(0.2f, 0.3f, 0.4f, 0.5f); colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f); colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f); colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.20f, 0.25f, 1.0f); colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.0f); colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.24f, 1.0f); colors[ImGuiCol_Button] = ImVec4(0.15f, 0.30f, 0.45f, 1.0f); colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.0f); colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.25f, 0.40f, 1.0f); colors[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.50f, 0.80f, 1.0f); colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.60f, 0.90f, 1.0f); colors[ImGuiCol_Header] = ImVec4(0.15f, 0.30f, 0.45f, 1.0f); colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.40f, 0.60f, 1.0f); colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.35f, 0.55f, 1.0f);
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context); ImGui_ImplOpenGL3_Init("#version 330");
    AudioState state; state.channelL.push_back(FrequencyRow(440.0f)); state.channelR.push_back(FrequencyRow(440.0f));
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

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(600, 680), ImGuiCond_FirstUseEver);
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoCollapse);

        if (state.running) { if (ImGui::Button("Stop", ImVec2(120, 40))) { Pa_StopStream(stream); state.running = false; } }
        else {
            if (ImGui::Button("Play", ImVec2(120, 40))) {
                for (auto& row : state.channelL) row.phase = 0.0; for (auto& row : state.channelR) row.phase = 0.0;
                Pa_StartStream(stream); state.running = true;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starts or stops the audio and visual generation.");

        ImGui::SameLine(); ImGui::Text("  Step: %.2fHz %s", getStep(state.shiftPressed, state.ctrlPressed), state.ctrlPressed && state.shiftPressed ? "(Ctrl+Shift)" : state.shiftPressed ? "(Shift)" : ""); ImGui::Separator();

        if (ImGui::CollapsingHeader("Left Channel (X)", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (size_t i = 0; i < state.channelL.size(); i++) {
                ImGui::PushID((int)i);
                ImGui::SetNextItemWidth(100);
                ImGui::InputFloat("Hz", &state.channelL[i].freq, 0.0f, 0.0f, "%.3f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frequency in Hertz for this oscillator.");

                ImGui::SameLine(); if (ImGui::Button("+")) state.channelL[i].freq += getStep(state.shiftPressed, state.ctrlPressed);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increase frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");

                ImGui::SameLine(); if (ImGui::Button("-")) state.channelL[i].freq -= getStep(state.shiftPressed, state.ctrlPressed);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrease frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");

                ImGui::SameLine(); if (ImGui::Button("x2")) { state.channelL[i].freq *= 2.0f; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply frequency by 2 (goes up one octave).");

                ImGui::SameLine(); if (ImGui::Button("/2")) { state.channelL[i].freq /= 2.0f; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide frequency by 2 (goes down one octave).");

                ImGui::SameLine(); ImGui::Checkbox("M", &state.channelL[i].muted);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute only this frequency.");

                ImGui::SameLine(); if (ImGui::Button("D")) { state.channelL.insert(state.channelL.begin() + i + 1, FrequencyRow(state.channelL[i].freq)); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate this frequency row.");

                ImGui::SameLine(); if (ImGui::Button("X")) { state.channelL.erase(state.channelL.begin() + i); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this frequency row.");

                ImGui::PopID();
            }
            if (ImGui::Button("+ Add Frequency##L")) { float lastFreq = state.channelL.empty() ? 440.0f : state.channelL.back().freq; state.channelL.push_back(FrequencyRow(lastFreq)); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new oscillator to this channel.");
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Right Channel (Y)", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (size_t i = 0; i < state.channelR.size(); i++) {
                ImGui::PushID(1000 + (int)i);
                ImGui::SetNextItemWidth(100);
                ImGui::InputFloat("Hz", &state.channelR[i].freq, 0.0f, 0.0f, "%.3f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Frequency in Hertz for this oscillator.");

                ImGui::SameLine(); if (ImGui::Button("+")) state.channelR[i].freq += getStep(state.shiftPressed, state.ctrlPressed);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increase frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");

                ImGui::SameLine(); if (ImGui::Button("-")) state.channelR[i].freq -= getStep(state.shiftPressed, state.ctrlPressed);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrease frequency by the Step value.\nHold Shift or Ctrl+Shift for fine tuning.");

                ImGui::SameLine(); if (ImGui::Button("x2")) { state.channelR[i].freq *= 2.0f; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiply frequency by 2 (goes up one octave).");

                ImGui::SameLine(); if (ImGui::Button("/2")) { state.channelR[i].freq /= 2.0f; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divide frequency by 2 (goes down one octave).");

                ImGui::SameLine(); ImGui::Checkbox("M", &state.channelR[i].muted);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute only this frequency.");

                ImGui::SameLine(); if (ImGui::Button("D")) { state.channelR.insert(state.channelR.begin() + i + 1, FrequencyRow(state.channelR[i].freq)); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate this frequency row.");

                ImGui::SameLine(); if (ImGui::Button("X")) { state.channelR.erase(state.channelR.begin() + i); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this frequency row.");

                ImGui::PopID();
            }
            if (ImGui::Button("+ Add Frequency##R")) { float lastFreq = state.channelR.empty() ? 440.0f : state.channelR.back().freq; state.channelR.push_back(FrequencyRow(lastFreq)); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new oscillator to this channel.");
        }
        ImGui::Separator();

        ImGui::SliderInt("Trail %", &state.trailPercent, 1, 100);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Defines the length of the wave's trail.");

        ImGui::Separator();
        ImGui::Checkbox("Show Start/End Points", &state.showStartEndPoints);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show/hide the red (start) and white (end) points of the trail.");

        // REMOVED: Laser Mode checkbox

        ImGui::Separator();
        if (ImGui::Button("Swap Channels (X <-> Y)")) { std::swap(state.channelL, state.channelR); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swap all frequencies from channel X with channel Y.");

        ImGui::SameLine();
        if (state.audioMuted) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f)); }
        ImGui::Checkbox("Mute Audio", &state.audioMuted);
        if (state.audioMuted) { ImGui::PopStyleColor(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Completely mutes the audio output, keeping the visualization.");

        ImGui::Separator();
        ImGui::Text("FPS: %.1f / %d", io.Framerate, state.targetFPS);
        ImGui::SliderInt("Target FPS", &state.targetFPS, 60, 480);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the target FPS for rendering.\nHigher values may result in smoother animation.");

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