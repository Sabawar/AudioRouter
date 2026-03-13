#include "AudioEngine.h"
#include "AudioMath.h"
#include "NoiseFilter.h"
#include "Logger.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cassert>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>  // timeBeginPeriod / timeEndPeriod
#include <avrt.h>      // AvSetMmThreadCharacteristicsW
#pragma comment(lib, "winmm.lib")

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(RoutingGraph* graph) {
    m_graph = graph;
    LOG_INFO("AudioEngine: initialising WASAPI...");
    if (!m_wasapi.init()) { LOG_ERROR("AudioEngine: WASAPI init failed"); return false; }
    m_srcBuf.resize(mixBufferSize * 8, 0.0f);
    m_mixBuf.resize(mixBufferSize * 8, 0.0f);
    LOG_INFO("AudioEngine: ready (mixRate=%d bufSize=%d)", mixSampleRate, mixBufferSize);
    return true;
}

void AudioEngine::shutdown() {
    LOG_INFO("AudioEngine: shutdown");
    stop();
    m_wasapi.shutdown();
    m_asio.unloadDriver();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Sync RoutingGraph → active streams
// ──────────────────────────────────────────────────────────────────────────────
void AudioEngine::syncToGraph() {
    if (!m_graph) return;
    std::lock_guard<std::mutex> lk(m_graph->mutex);

    // Close streams for removed nodes
    // (simplified: rebuild all active streams)

    for (auto& node : m_graph->nodes()) {
        if (!node.enabled) {
            m_wasapi.closeStream(node.id);
            continue;
        }
        m_nodeGains[node.id] = std::pow(10.0f, node.gainDb / 20.0f);

        switch (node.type) {
        case DeviceType::WasapiCapture: {
            for (auto& d : m_wasapi.deviceList()) {
                if (d.id == std::wstring(node.deviceId.begin(), node.deviceId.end())) {
                    m_wasapi.openCaptureStream(d, node.id, false,
                                               node.deviceChannels,
                                               node.deviceSampleRate);
                    break;
                }
            }
            break;
        }
        case DeviceType::WasapiLoopback: {
            for (auto& d : m_wasapi.deviceList()) {
                if (d.isLoopback &&
                    d.id == std::wstring(node.deviceId.begin(), node.deviceId.end())) {
                    m_wasapi.openCaptureStream(d, node.id, true,
                                               node.deviceChannels,
                                               node.deviceSampleRate);
                    break;
                }
            }
            break;
        }
        case DeviceType::WasapiRender:
        case DeviceType::VirtualSink: {
            for (auto& d : m_wasapi.deviceList()) {
                if (!d.isCapture && !d.isLoopback &&
                    d.id == std::wstring(node.deviceId.begin(), node.deviceId.end())) {
                    m_wasapi.openRenderStream(d, node.id, node.deviceChannels);
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Start / Stop
// ──────────────────────────────────────────────────────────────────────────────
bool AudioEngine::start() {
    if (m_running) return true;
    syncToGraph();
    m_stopFlag = false;
    m_running  = true;
    m_mixThread = std::thread(&AudioEngine::mixThread, this);
    return true;
}

void AudioEngine::stop() {
    if (!m_running) return;
    m_stopFlag = true;
    if (m_mixThread.joinable()) m_mixThread.join();
    m_running = false;
    m_wasapi.closeAllStreams();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Mix thread
// ──────────────────────────────────────────────────────────────────────────────
void AudioEngine::mixThread() {
    // Windows default timer resolution is ~15ms which causes buffer underruns.
    // timeBeginPeriod(1) raises resolution to 1ms for accurate sleep_for(10ms).
    timeBeginPeriod(1);

    // MMCSS: tell Windows this thread does real-time audio work
    DWORD taskIdx = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);

    // 480 frames / 48000 Hz = 10ms per cycle — matches mixBufferSize
    const auto kInterval = std::chrono::milliseconds(10);
    auto nextWake = std::chrono::steady_clock::now();

    while (!m_stopFlag) {
        processMixCycle();
        nextWake += kInterval;
        std::this_thread::sleep_until(nextWake); // drift-free: uses absolute time
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    timeEndPeriod(1);
}

void AudioEngine::processMixCycle() {
    if (!m_graph) return;
    std::lock_guard<std::mutex> lk(m_graph->mutex);

    const int frames = mixBufferSize;
    const int ch     = 2;

    // ── Pass 1: Process Mixer nodes ──────────────────────────────────────────
    // Each Mixer collects audio from its sources and stores in m_mixerBufs.
    // This allows sinks to read from Mixer outputs in Pass 2.
    for (auto& node : m_graph->nodes()) {
        if (node.type != DeviceType::Mixer || !node.enabled) continue;

        auto incoming = m_graph->incomingEdges(node.id);
        auto& mixOut  = m_mixerBufs[node.id];
        mixOut.assign(frames * ch, 0.0f);

        bool anyData = false;
        for (auto* edge : incoming) {
            int srcId = edge->fromNodeId;
            auto* srcNode = m_graph->findNode(srcId);
            if (!srcNode || !srcNode->enabled) continue;

            float srcGain = std::pow(10.0f, srcNode->gainDb / 20.0f);
            size_t got = readFromSource(srcId, frames, ch);
            if (got > 0) {
                anyData = true;
                AudioMath::mixAdd(mixOut.data(), m_srcBuf.data(), got * ch, srcGain);
            }
        }
        // Apply mixer node gain
        if (anyData) {
            float mixGain = std::pow(10.0f, node.gainDb / 20.0f);
            if (fabsf(mixGain - 1.0f) > 0.001f)
                for (auto& s : mixOut) s *= mixGain;
            // Clip guard
            for (auto& s : mixOut) s = std::clamp(s, -1.0f, 1.0f);
        }
        // Update peak meter for mixer node
        node.peakL = 0; node.peakR = 0;
        for (int i = 0; i < frames && anyData; ++i) {
            node.peakL = std::max(node.peakL, std::abs(mixOut[i*ch]));
            node.peakR = std::max(node.peakR, std::abs(mixOut[i*ch+1]));
        }
    }

    // ── Pass 2: Push audio to hardware sinks ─────────────────────────────────
    for (auto& node : m_graph->nodes()) {
        bool isSink = (node.type == DeviceType::WasapiRender ||
                       node.type == DeviceType::AsioOutput   ||
                       node.type == DeviceType::VirtualSink);
        if (!isSink || !node.enabled) continue;
        mixToSink(node.id);
    }

    // ── Update peak meters on all non-mixer nodes ─────────────────────────────
    for (auto& node : m_graph->nodes()) {
        if (node.type == DeviceType::Mixer) continue; // already done above
        node.peakL = m_wasapi.peakL(node.id);
        node.peakR = m_wasapi.peakR(node.id);
    }
}

// Read `frames` from any source type into m_srcBuf (always stereo interleaved).
// Returns number of stereo frames produced.
size_t AudioEngine::readFromSource(int srcNodeId, int frames, int ch) {
    m_srcBuf.assign(frames * ch, 0.0f);

    auto* srcNode = m_graph->findNode(srcNodeId);
    if (!srcNode) return 0;

    if (srcNode->type == DeviceType::WasapiCapture  ||
        srcNode->type == DeviceType::WasapiLoopback ||
        srcNode->type == DeviceType::AsioInput)
    {
        // Drain stale data to keep latency minimal
        m_wasapi.drainExcess(srcNodeId, frames * 3);

        // Find the native channel count of this stream
        int nativeCh = m_wasapi.streamChannels(srcNodeId); // 1=mono, 2=stereo
        if (nativeCh <= 0) nativeCh = 2;

        size_t got = 0;
        if (nativeCh == 1) {
            // Read mono into a temporary mono buffer then upmix to stereo
            m_monoTmp.assign(frames, 0.0f);
            got = m_wasapi.readFromCaptureRaw(srcNodeId, m_monoTmp.data(), (size_t)frames, 1);
            // Upmix: mono L+R = same sample
            for (size_t i = 0; i < got; ++i) {
                m_srcBuf[i*2]   = m_monoTmp[i];
                m_srcBuf[i*2+1] = m_monoTmp[i];
            }
        } else {
            got = m_wasapi.readFromCapture(srcNodeId, m_srcBuf.data(), (size_t)frames);
        }

        // ── Apply noise filter (capture nodes only, not loopback) ───────────
        if (got > 0 && srcNode->type == DeviceType::WasapiCapture) {
            NoiseFilterMode mode = (NoiseFilterMode)srcNode->noiseFilterMode;
            if (mode != NoiseFilterMode::Off) {
                if (!srcNode->noiseFilter) {
                    srcNode->noiseFilter = std::make_shared<NoiseFilter>();
                    srcNode->noiseFilter->init(48000.0f);
                    srcNode->noiseFilter->setMode(mode);
                    LOG_INFO("NoiseFilter: created mode=%d for node %d", (int)mode, srcNodeId);
                }
                if (srcNode->noiseFilter->getMode() != mode) {
                    srcNode->noiseFilter->setMode(mode);
                    LOG_DEBUG("NoiseFilter: mode changed to %d for node %d", (int)mode, srcNodeId);
                }
                srcNode->noiseFilter->settings().gateThreshDb = srcNode->noiseGateThreshDb;
                srcNode->noiseFilter->processStereo(m_srcBuf.data(), (int)got);
            } else {
                if (srcNode->noiseFilter) srcNode->noiseFilter.reset();
            }
        }

        return got;
    }
    else if (srcNode->type == DeviceType::Mixer) {
        auto it = m_mixerBufs.find(srcNodeId);
        if (it != m_mixerBufs.end() && !it->second.empty()) {
            size_t n = std::min((size_t)(frames * ch), it->second.size());
            std::copy(it->second.begin(), it->second.begin() + n, m_srcBuf.begin());
            return n / ch;
        }
    }
    return 0;
}

void AudioEngine::mixToSink(int sinkNodeId) {
    auto incoming = m_graph->incomingEdges(sinkNodeId);
    if (incoming.empty()) return;

    int   frames   = mixBufferSize;
    int   ch       = 2;
    float sinkGain = 1.0f;
    {
        auto* sinkNode = m_graph->findNode(sinkNodeId);
        if (sinkNode) sinkGain = std::pow(10.0f, sinkNode->gainDb / 20.0f);
    }

    std::fill(m_mixBuf.begin(), m_mixBuf.begin() + frames * ch, 0.0f);

    bool anyData = false;
    for (auto* edge : incoming) {
        int srcId = edge->fromNodeId;
        auto* srcNode = m_graph->findNode(srcId);
        if (!srcNode || !srcNode->enabled) continue;

        float srcGain = std::pow(10.0f, srcNode->gainDb / 20.0f);
        size_t got = readFromSource(srcId, frames, ch);
        if (got > 0) {
            anyData = true;
            AudioMath::mixAdd(m_mixBuf.data(), m_srcBuf.data(), got * ch, srcGain);
        }
    }

    if (!anyData) return;

    // Sink gain + hard clip
    if (fabsf(sinkGain - 1.0f) > 0.001f)
        for (int i = 0; i < frames * ch; ++i)
            m_mixBuf[i] *= sinkGain;
    for (int i = 0; i < frames * ch; ++i)
        m_mixBuf[i] = std::clamp(m_mixBuf[i], -1.0f, 1.0f);

    auto* sinkNode = m_graph->findNode(sinkNodeId);
    if (sinkNode && (sinkNode->type == DeviceType::WasapiRender ||
                     sinkNode->type == DeviceType::VirtualSink  ||
                     sinkNode->type == DeviceType::AsioOutput))
        m_wasapi.writeToRender(sinkNodeId, m_mixBuf.data(), (size_t)frames);
}

// ──────────────────────────────────────────────────────────────────────────────
//  ASIO helpers
// ──────────────────────────────────────────────────────────────────────────────
std::vector<AsioDeviceInfo> AudioEngine::enumerateAsioDevices() {
    return AsioManager::enumerateDrivers();
}

bool AudioEngine::loadAsioDriver(const AsioDeviceInfo& devIn, HWND hwnd) {
    AsioDeviceInfo dev = devIn;

    // Find if any AsioInput/Output node has useMinBuffer preference
    if (m_graph) {
        std::lock_guard<std::mutex> lk(m_graph->mutex);
        for (auto& node : m_graph->nodes()) {
            if (node.type == DeviceType::AsioInput ||
                node.type == DeviceType::AsioOutput) {
                dev.useMinBuffer = node.asioMinBuffer;
                break;
            }
        }
    }

    if (!m_asio.loadDriver(dev, hwnd)) return false;

    int numIn  = m_asio.numInputChannels();
    int numOut = m_asio.numOutputChannels();
    int sr     = (int)m_asio.sampleRate();
    int inCh   = std::max(1, std::min(numIn,  2));
    int outCh  = std::max(1, std::min(numOut, 2));
    // Note: virtual stream registration happens AFTER node IDs are assigned
    // (called via registerAsioStreams() from UI after addNode())

    // ── Новая архитектура: весь роутинг ASIO делается ПРЯМО в callback ────────
    // Это единственный надёжный способ — именно так работает Reaper и все DAW.
    // Mix engine thread используется только для WASAPI↔WASAPI маршрутов.
    // Для ASIO: input → graph edges → output обрабатывается синхронно в callback.
    m_asioResampleRatio = 1.0; // не используется больше для ASIO→ASIO
    m_asioResamplePrev.assign(inCh, 0.0f);
    m_asioResampleBuf.resize((size_t)(numIn * m_asio.bufferSize() * 4));

    m_asio.createBuffersAndStart(
        numIn, numOut,
        [this, inCh, outCh, sr](long /*bufIdx*/,
               const std::vector<float>& inputs,
               std::vector<float>& outputs,
               int numFrames, int /*numIn*/, int /*numOut*/)
        {
            // ── 1. Обнуляем выход ────────────────────────────────────────────
            std::fill(outputs.begin(), outputs.end(), 0.0f);

            if (!m_graph) return;

            int inNodeId  = m_asio.inputNodeId;
            int outNodeId = m_asio.outputNodeId;

            // ── 2. Пишем ASIO вход в кольцевой буфер (для WASAPI-синков) ─────
            if (inNodeId >= 0 && !inputs.empty() &&
                (int)inputs.size() >= numFrames * inCh)
            {
                m_wasapi.writeToRender(inNodeId, inputs.data(), (size_t)numFrames);
            }

            // ── 3. Прямой роутинг ASIO IN → ASIO OUT внутри callback ─────────
            // Ищем в графе: есть ли ребро from=inNodeId to=outNodeId?
            if (inNodeId >= 0 && outNodeId >= 0 &&
                !inputs.empty() && !outputs.empty())
            {
                bool directEdge = false;
                {
                    std::lock_guard<std::mutex> lk(m_graph->mutex);
                    for (auto& e : m_graph->edges()) {
                        if (e.fromNodeId == inNodeId && e.toNodeId == outNodeId) {
                            directEdge = true;
                            break;
                        }
                    }
                }

                if (directEdge) {
                    // Прямой пассинг: вход → выход (с учётом gain нодов)
                    float inGain = 1.0f, outGain = 1.0f;
                    {
                        std::lock_guard<std::mutex> lk(m_graph->mutex);
                        auto* nIn  = m_graph->findNode(inNodeId);
                        auto* nOut = m_graph->findNode(outNodeId);
                        if (nIn  && nIn->enabled)
                            inGain  = std::pow(10.0f, nIn->gainDb  / 20.0f);
                        else if (nIn && !nIn->enabled)
                            inGain = 0.0f;
                        if (nOut && nOut->enabled)
                            outGain = std::pow(10.0f, nOut->gainDb / 20.0f);
                        else if (nOut && !nOut->enabled)
                            outGain = 0.0f;
                    }
                    float gain = inGain * outGain;
                    int n = std::min((int)inputs.size(), (int)outputs.size());
                    for (int i = 0; i < n; ++i)
                        outputs[i] += std::clamp(inputs[i] * gain, -1.0f, 1.0f);

                    LOG_DEBUG("ASIO direct route: %d frames gain=%.2f", numFrames, gain);
                }
            }

            // ── 4. ASIO OUT ← кольцевой буфер (для WASAPI→ASIO маршрутов) ───
            // Добавляем к уже имеющемуся (от прямого роутинга)
            if (outNodeId >= 0) {
                // Читаем то что mix engine успел положить
                std::vector<float> fromMix(numFrames * outCh, 0.0f);
                size_t got = m_wasapi.readFromCapture(outNodeId,
                                                       fromMix.data(),
                                                       (size_t)numFrames);
                if (got > 0) {
                    int n = std::min((int)fromMix.size(), (int)outputs.size());
                    for (int i = 0; i < n; ++i)
                        outputs[i] = std::clamp(outputs[i] + fromMix[i], -1.0f, 1.0f);
                }
            }
        });

    LOG_INFO("AudioEngine: ASIO callback ready. Direct routing mode. sr=%d buf=%d",
             sr, m_asio.bufferSize());
    return true;
}

void AudioEngine::registerAsioStreams() {
    if (!m_asio.isLoaded()) return;
    // Ring buffers store data at MIX rate (after resampling), not ASIO rate
    int regSr  = mixSampleRate;
    int inCh  = std::max(1, std::min(m_asio.numInputChannels(),  2));
    int outCh = std::max(1, std::min(m_asio.numOutputChannels(), 2));
    if (m_asio.inputNodeId  >= 0)
        m_wasapi.registerVirtualStream(m_asio.inputNodeId,  inCh,  regSr);
    if (m_asio.outputNodeId >= 0)
        m_wasapi.registerVirtualStream(m_asio.outputNodeId, outCh, regSr);
    LOG_INFO("AudioEngine: ASIO streams registered in=%d out=%d mixSR=%d ch_in=%d ch_out=%d",
             m_asio.inputNodeId, m_asio.outputNodeId, regSr, inCh, outCh);
}

void AudioEngine::unloadAsioDriver() {
    m_asio.stopAndDisposeBuffers();
    // Unregister virtual streams before unloading the driver
    if (m_asio.inputNodeId  >= 0) m_wasapi.unregisterVirtualStream(m_asio.inputNodeId);
    if (m_asio.outputNodeId >= 0) m_wasapi.unregisterVirtualStream(m_asio.outputNodeId);
    m_asio.unloadDriver();
}

int AudioEngine::addWasapiNode(const WasapiDeviceInfo& dev) {
    DeviceType type;
    if (dev.isLoopback)      type = DeviceType::WasapiLoopback;
    else if (dev.isCapture)  type = DeviceType::WasapiCapture;
    else                     type = DeviceType::WasapiRender;

    // Device IDs are ASCII GUIDs — direct narrow conversion is safe
    std::string devIdUtf8;
    devIdUtf8.reserve(dev.id.size());
    for (wchar_t c : dev.id) devIdUtf8 += (char)(c & 0xFF);

    // Friendly name: must be proper UTF-8 so Cyrillic renders in ImGui
    std::string nameUtf8;
    if (!dev.friendlyName.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0,
                                    dev.friendlyName.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            nameUtf8.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0,
                                dev.friendlyName.c_str(), -1,
                                &nameUtf8[0], n, nullptr, nullptr);
        }
    }

    return m_graph->addNode(nameUtf8, devIdUtf8, type);
}
