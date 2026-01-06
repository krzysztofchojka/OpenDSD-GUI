// --- FIX FOR MACOS OPENGL WARNINGS ---
#define GL_SILENCE_DEPRECATION

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <vector>
#include <string>
#include <iostream>
#include <mutex>
#include <cmath>
#include <algorithm>

// Miniaudio implementation
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// DSDcc Headers
#include <dsdcc/dsd_decoder.h>

// GLFW
#include <GLFW/glfw3.h> 

// --- RING BUFFER ---
struct RingBuffer {
    std::vector<float> buffer;
    size_t size;
    size_t writeHead;
    size_t readHead;
    size_t available;

    void init(size_t bufferSize) {
        size = bufferSize;
        buffer.resize(size, 0.0f);
        writeHead = 0;
        readHead = 0;
        available = 0;
    }

    void write(const float* data, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            buffer[writeHead] = data[i];
            writeHead = (writeHead + 1) % size;
            if (available < size) available++;
            else readHead = (readHead + 1) % size;
        }
    }

    void read(float* output, size_t count) {
        if (available < count) {
            memset(output, 0, count * sizeof(float));
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            output[i] = buffer[readHead];
            readHead = (readHead + 1) % size;
            available--;
        }
    }
};

// --- STATE ---
struct AppState {
    ma_context context;
    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info* pCaptureInfos;
    ma_uint32 captureCount;
    int selectedCaptureIndex = 0;
    int selectedPlaybackIndex = 0;

    ma_device device;
    bool isAudioRunning = false;

    RingBuffer ringBuffer;

    // DSP Settings
    bool polarityInvert = false;
    bool autoGain = true;
    bool analogPassthrough = false; 
    float inputGain = 1.0f;
    float currentPeak = 0.01f; 
    
    // Protocol Settings
    bool enableDMR = true;
    bool enableNXDN = false;
    bool enabledPMR = false; 
    bool enableP25 = false;
    
    // Interpolation State
    float lastSampleS1 = 0.0f;
    float lastSampleS2 = 0.0f;

    // UI & Logic State
    std::string decoderStatus = "WAITING";
    std::string activeSlot = "--";
    int statusHoldCounter = 0; 
    int digitalPrecedenceTimer = 0; // NOWOŚĆ: Timer priorytetu cyfrowego
    
    std::vector<float> inputScope; 
    
    DSDcc::DSDDecoder dsdDecoder;
    std::mutex audioMutex;

    AppState() {
        // ZMNIEJSZAMY BUFOR do 0.5 sekundy (24000 próbek)
        // Dzięki temu po wyłączeniu analogu reakcja będzie natychmiastowa
        ringBuffer.init(48000 / 2); 
    }
    
    void updateDecoderSettings() {
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeAuto, false);
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeDMR, enableDMR);
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNXDN48, enableNXDN);
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNXDN96, enableNXDN);
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeDPMR, enabledPMR);
        dsdDecoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeP25P1, enableP25);
    }

    void setupInitial() {
        dsdDecoder.enableMbelib(true);
        dsdDecoder.enableAudioOut(true);
        dsdDecoder.setUpsampling(1); 
        dsdDecoder.setVerbosity(0); 
        updateDecoderSettings();
    }
};

AppState app;

// --- AUDIO CALLBACK ---
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    AppState* state = (AppState*)pDevice->pUserData;
    if (!state->audioMutex.try_lock()) {
        memset(pOutput, 0, frameCount * sizeof(float)); 
        return;
    }

    const float* inBuffer = (const float*)pInput;
    float* outBuffer = (float*)pOutput;
    
    std::vector<float> processedInput(frameCount);

    // 1. INPUT PROCESSING
    for (ma_uint32 i = 0; i < frameCount; ++i) {
        float sample = inBuffer[i];

        if (state->autoGain) {
            float absSample = fabsf(sample);
            state->currentPeak *= 0.999f;
            if (absSample > state->currentPeak) state->currentPeak = absSample;
            if (state->currentPeak > 0.001f) {
                float targetGain = 0.8f / state->currentPeak;
                state->inputGain = state->inputGain * 0.95f + targetGain * 0.05f;
            }
        }
        sample *= state->inputGain;

        if (state->polarityInvert) sample = -sample;
        processedInput[i] = sample;

        if (state->inputScope.size() < 200 && (i % 20 == 0)) {
            state->inputScope.push_back(sample);
        }

        short sampleShort = (short)(sample * 32000.0f);
        state->dsdDecoder.run(sampleShort);
    }
    
    // 2. SPRAWDŹ STATUS SYNC (PRZED GENEROWANIEM AUDIO)
    DSDcc::DSDDecoder::DSDSyncType sync = state->dsdDecoder.getSyncType();
    
    // Jeśli wykryto jakikolwiek ślad cyfry, ustaw timer priorytetu
    // Blokuje to analog na około 300ms (przy 60fps callbacku to ok. 20)
    if (sync != DSDcc::DSDDecoder::DSDSyncNone) {
        state->digitalPrecedenceTimer = 20; 
    } else {
        if (state->digitalPrecedenceTimer > 0) state->digitalPrecedenceTimer--;
    }

    // 3. ODBIÓR AUDIO CYFROWEGO
    int samplesS1 = 0;
    int samplesS2 = 0;
    
    short* audioS1 = state->dsdDecoder.getAudio1(samplesS1);
    short* audioS2 = state->dsdDecoder.getAudio2(samplesS2);
    
    bool gotDigitalVoice = false;

    auto processSmooth = [&](short* rawAudio, int count, float& lastSampleState) {
        for (int k = 0; k < count; k++) {
            float targetSample = (float)rawAudio[k] / 32768.0f;
            targetSample *= 3.5f; 
            if (targetSample > 1.0f) targetSample = 1.0f;
            if (targetSample < -1.0f) targetSample = -1.0f;

            for(int u=1; u<=6; u++) {
                float t = (float)u / 6.0f;
                float smoothVal = lastSampleState + (targetSample - lastSampleState) * t;
                state->ringBuffer.write(&smoothVal, 1);
            }
            lastSampleState = targetSample;
        }
    };

    if (samplesS1 > 0 && audioS1 != nullptr) {
        state->activeSlot = "SLOT 1 / CH 1";
        gotDigitalVoice = true;
        processSmooth(audioS1, samplesS1, state->lastSampleS1);
        state->dsdDecoder.resetAudio1();
    }

    if (samplesS2 > 0 && audioS2 != nullptr) {
        state->activeSlot = "SLOT 2";
        gotDigitalVoice = true;
        processSmooth(audioS2, samplesS2, state->lastSampleS2);
        state->dsdDecoder.resetAudio2();
    }

    // 4. ANALOG PASSTHROUGH (ZABEZPIECZONY)
    if (!gotDigitalVoice) {
        // Warunek: Włączony passthrough ORAZ brak śladu cyfry w ostatnim czasie
        if (state->analogPassthrough && state->digitalPrecedenceTimer == 0) {
             for (float s : processedInput) {
                 state->ringBuffer.write(&s, 1);
             }
        }
    }

    // 5. STATUS UI LOGIC
    if (gotDigitalVoice) {
        state->decoderStatus = "DECODING VOICE!";
        state->statusHoldCounter = 30; 
    } else {
        if (state->statusHoldCounter > 0) {
            state->statusHoldCounter--;
        } else {
            // Jeśli timer priorytetu jest aktywny, pokazujemy że walczymy o sync
            if (state->digitalPrecedenceTimer > 0) {
                state->activeSlot = "--";
                switch(sync) {
                    case DSDcc::DSDDecoder::DSDSyncDMRDataP:
                    case DSDcc::DSDDecoder::DSDSyncDMRDataMS:
                    case DSDcc::DSDDecoder::DSDSyncDMRVoiceP:
                    case DSDcc::DSDDecoder::DSDSyncDMRVoiceMS:
                        state->decoderStatus = "SYNC: DMR (Muting Analog)"; break;
                    case DSDcc::DSDDecoder::DSDSyncNXDNP:
                    case DSDcc::DSDDecoder::DSDSyncNXDNN:
                        state->decoderStatus = "SYNC: NXDN (Muting Analog)"; break;
                    case DSDcc::DSDDecoder::DSDSyncP25p1P:
                        state->decoderStatus = "SYNC: P25 (Muting Analog)"; break;
                    case DSDcc::DSDDecoder::DSDSyncDPMR:
                        state->decoderStatus = "SYNC: dPMR (Muting Analog)"; break;
                    default:
                        state->decoderStatus = "SYNC: DATA (Muting Analog)"; break;
                }
            } else {
                if (state->analogPassthrough) state->decoderStatus = "ANALOG MONITOR (NFM)";
                else state->decoderStatus = "Searching...";
                state->activeSlot = "--";
            }
        }
    }

    // 6. PLAYBACK
    state->ringBuffer.read(outBuffer, frameCount);
    
    state->audioMutex.unlock();
}

// --- GUI ---
void RenderGUI() {
    ImGui::Begin("OpenDSD Control");

    if (ImGui::CollapsingHeader("Audio Config", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (app.isAudioRunning) ImGui::BeginDisabled();
        
        if (app.captureCount > 0) {
            if (ImGui::BeginCombo("Input", app.pCaptureInfos[app.selectedCaptureIndex].name)) {
                for (ma_uint32 i = 0; i < app.captureCount; i++) {
                    if (ImGui::Selectable(app.pCaptureInfos[i].name, app.selectedCaptureIndex == i))
                        app.selectedCaptureIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        
        if (app.playbackCount > 0) {
            if (ImGui::BeginCombo("Output", app.pPlaybackInfos[app.selectedPlaybackIndex].name)) {
                for (ma_uint32 i = 0; i < app.playbackCount; i++) {
                    if (ImGui::Selectable(app.pPlaybackInfos[i].name, app.selectedPlaybackIndex == i))
                        app.selectedPlaybackIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        if (app.isAudioRunning) ImGui::EndDisabled();

        if (app.isAudioRunning) {
            if (ImGui::Button("STOP SYSTEM", ImVec2(-1, 40))) {
                ma_device_uninit(&app.device);
                app.isAudioRunning = false;
            }
        } else {
            if (ImGui::Button("START DECODING", ImVec2(-1, 40))) {
                app.setupInitial(); 

                ma_device_config config = ma_device_config_init(ma_device_type_duplex);
                config.capture.pDeviceID  = &app.pCaptureInfos[app.selectedCaptureIndex].id;
                config.playback.pDeviceID = &app.pPlaybackInfos[app.selectedPlaybackIndex].id;
                config.capture.format     = ma_format_f32; 
                config.playback.format    = ma_format_f32;
                config.capture.channels   = 1;
                config.playback.channels  = 1; 
                config.sampleRate         = 48000;
                config.dataCallback       = data_callback;
                config.pUserData          = &app;

                if (ma_device_init(&app.context, &config, &app.device) == MA_SUCCESS) {
                    ma_device_start(&app.device);
                    app.isAudioRunning = true;
                }
            }
        }
    }

    ImGui::Separator();

    // --- PROTOCOLS & ANALOG ---
    if (ImGui::CollapsingHeader("Protocols & Modes", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Analog Passthrough (Mixed Mode)", &app.analogPassthrough);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plays raw audio when NO digital signal is present.");

        ImGui::Separator();
        ImGui::TextDisabled("Digital Standards:");
        
        if (ImGui::Checkbox("DMR", &app.enableDMR)) app.updateDecoderSettings();
        ImGui::SameLine();
        if (ImGui::Checkbox("NXDN", &app.enableNXDN)) app.updateDecoderSettings();
        ImGui::SameLine();
        if (ImGui::Checkbox("dPMR", &app.enabledPMR)) app.updateDecoderSettings();
        
        if (ImGui::Checkbox("P25", &app.enableP25)) app.updateDecoderSettings();
    }
    
    ImGui::Separator();
    
    if (ImGui::CollapsingHeader("Signal Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Auto Gain (AGC)", &app.autoGain);
        if (!app.autoGain) ImGui::SliderFloat("Manual Gain", &app.inputGain, 0.1f, 20.0f);
        else {
            ImGui::ProgressBar(app.currentPeak, ImVec2(-1, 0), "Input Level");
            ImGui::TextDisabled("Auto Gain: %.2fx", app.inputGain);
        }
        ImGui::Checkbox("Invert Polarity", &app.polarityInvert);
        
        ImGui::Separator();
        ImVec4 statusColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); 
        if (app.decoderStatus.find("DECODING") != std::string::npos) statusColor = ImVec4(0, 1, 0, 1);
        else if (app.decoderStatus.find("SYNC") != std::string::npos) statusColor = ImVec4(0, 1, 1, 1);
        else if (app.decoderStatus.find("ANALOG") != std::string::npos) statusColor = ImVec4(1, 0.6f, 0, 1);

        ImGui::TextColored(statusColor, "Status: %s", app.decoderStatus.c_str());
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Activity: %s", app.activeSlot.c_str());

        if (!app.inputScope.empty()) {
            ImGui::PlotLines("##scope", app.inputScope.data(), (int)app.inputScope.size(), 0, "Input Signal", -1.0f, 1.0f, ImVec2(-1, 80));
            app.inputScope.clear();
        }
    }

    ImGui::End();
}

int main(int, char**)
{
    if (!glfwInit()) return 1;
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(600, 600, "OpenDSD GUI (Fixed)", NULL, NULL);
    if (window == NULL) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); 

    if (ma_context_init(NULL, 0, NULL, &app.context) != MA_SUCCESS) return -1;
    ma_context_get_devices(&app.context, &app.pPlaybackInfos, &app.playbackCount, &app.pCaptureInfos, &app.captureCount);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; 

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        RenderGUI();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (app.isAudioRunning) ma_device_uninit(&app.device);
    ma_context_uninit(&app.context);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}