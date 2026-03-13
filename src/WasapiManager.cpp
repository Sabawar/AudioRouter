#include "WasapiManager.h"
#include "Logger.h"
#include <initguid.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <combaseapi.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <cassert>

#pragma comment(lib,"avrt.lib")

// ──────────────────────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────────────────────
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ──────────────────────────────────────────────────────────────────────────────
//  AudioRingBuffer
// ──────────────────────────────────────────────────────────────────────────────
AudioRingBuffer::AudioRingBuffer(size_t capacitySamples)
    : m_buf(capacitySamples, 0.0f)
{}

size_t AudioRingBuffer::write(const float* data, size_t frames) {
    size_t n      = frames * channels;
    size_t cap    = m_buf.size();
    size_t wp     = m_writePos.load(std::memory_order_relaxed);
    size_t rp     = m_readPos.load(std::memory_order_acquire);
    size_t avail  = cap - ((wp - rp + cap) % cap) - channels; // keep 1 frame gap
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; ++i)
        m_buf[(wp + i) % cap] = data[i];
    m_writePos.store((wp + n) % cap, std::memory_order_release);
    return n / channels;
}

size_t AudioRingBuffer::read(float* data, size_t frames) {
    size_t n   = frames * channels;
    size_t cap = m_buf.size();
    size_t rp  = m_readPos.load(std::memory_order_relaxed);
    size_t wp  = m_writePos.load(std::memory_order_acquire);
    size_t avail = (wp - rp + cap) % cap;
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; ++i)
        data[i] = m_buf[(rp + i) % cap];
    // zero-pad the rest
    for (size_t i = n; i < frames * channels; ++i) data[i] = 0.0f;
    m_readPos.store((rp + n) % cap, std::memory_order_release);
    return n / channels;
}

size_t AudioRingBuffer::available() const {
    size_t cap = m_buf.size();
    size_t wp  = m_writePos.load(std::memory_order_acquire);
    size_t rp  = m_readPos.load(std::memory_order_acquire);
    return ((wp - rp + cap) % cap) / channels;
}

void AudioRingBuffer::reset() {
    m_writePos = 0; m_readPos = 0;
    std::fill(m_buf.begin(), m_buf.end(), 0.0f);
}

// ──────────────────────────────────────────────────────────────────────────────
//  WasapiManager
// ──────────────────────────────────────────────────────────────────────────────
WasapiManager::WasapiManager() = default;
WasapiManager::~WasapiManager() { shutdown(); }

bool WasapiManager::init() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&m_enumerator);
    if (FAILED(hr)) { LOG_ERROR("MMDeviceEnumerator CoCreateInstance: 0x%08X", (unsigned)hr); return false; }
    LOG_INFO("WASAPI: IMMDeviceEnumerator OK");

    // Get default device IDs
    auto getDefaultId = [&](EDataFlow flow) -> std::wstring {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &dev))) {
            LPWSTR id = nullptr;
            dev->GetId(&id);
            std::wstring s = id;
            CoTaskMemFree(id);
            dev->Release();
            return s;
        }
        return {};
    };
    m_defaultPlaybackId = getDefaultId(eRender);
    m_defaultCaptureId  = getDefaultId(eCapture);

    refreshDeviceList();
    return true;
}

void WasapiManager::shutdown() {
    closeAllStreams();
    if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    CoUninitialize();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Device enumeration
// ──────────────────────────────────────────────────────────────────────────────
std::vector<WasapiDeviceInfo> WasapiManager::enumerateDevices() {
    refreshDeviceList();
    return m_devices;
}

void WasapiManager::refreshDeviceList() {
    if (!m_enumerator) return;
    LOG_DEBUG("WASAPI: refreshing device list...");

    // Re-fetch default device IDs (they may change at runtime)
    auto getDefaultId = [&](EDataFlow flow) -> std::wstring {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &dev))) {
            LPWSTR id = nullptr; dev->GetId(&id);
            std::wstring s = id;
            CoTaskMemFree(id); dev->Release();
            return s;
        }
        return {};
    };
    m_defaultPlaybackId = getDefaultId(eRender);
    m_defaultCaptureId  = getDefaultId(eCapture);

    // Build new list off-thread-safe local vector, then swap atomically
    std::vector<WasapiDeviceInfo> newDevices;

    auto enumFlow = [&](EDataFlow flow, bool isCapture) {
        IMMDeviceCollection* col = nullptr;
        if (FAILED(m_enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)))
            return;
        UINT cnt = 0; col->GetCount(&cnt);
        for (UINT i = 0; i < cnt; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;

            WasapiDeviceInfo info;
            info.isCapture = isCapture;

            LPWSTR id = nullptr;
            dev->GetId(&id);
            info.id = id;
            CoTaskMemFree(id);
            info.isDefault = (isCapture ? info.id == m_defaultCaptureId
                                        : info.id == m_defaultPlaybackId);

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR)
                    info.friendlyName = pv.pwszVal;
                PropVariantClear(&pv);
                props->Release();
            }

            auto fn = info.friendlyName;
            std::transform(fn.begin(), fn.end(), fn.begin(), ::towlower);
            info.isVirtualCable = (fn.find(L"vb-audio") != std::wstring::npos ||
                                   fn.find(L"virtual cable") != std::wstring::npos ||
                                   fn.find(L"voicemeeter") != std::wstring::npos ||
                                   fn.find(L"cable") != std::wstring::npos);

            IAudioClient* ac = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
                WAVEFORMATEX* fmt = nullptr;
                if (SUCCEEDED(ac->GetMixFormat(&fmt))) {
                    info.channels   = fmt->nChannels;
                    info.sampleRate = fmt->nSamplesPerSec;
                    CoTaskMemFree(fmt);
                }
                ac->Release();
            }

            // Add loopback variant BEFORE the render device itself
            if (!isCapture) {
                WasapiDeviceInfo lb = info;
                lb.isLoopback    = true;
                lb.friendlyName += L" [Loopback]";
                newDevices.push_back(lb);
            }
            newDevices.push_back(info);
            dev->Release();
        }
        col->Release();
    };

    enumFlow(eCapture, true);
    enumFlow(eRender,  false);

    // Atomic swap so readers always see a complete list
    {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        m_devices = std::move(newDevices);
    }
    LOG_INFO("WASAPI: found %d devices total", (int)m_devices.size());
}

// ──────────────────────────────────────────────────────────────────────────────
//  Open / close streams
// ──────────────────────────────────────────────────────────────────────────────
bool WasapiManager::openCaptureStream(const WasapiDeviceInfo& dev, int nodeId,
                                      bool loopback,
                                      int requestedChannels,
                                      int requestedSampleRate) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    if (m_streams.count(nodeId)) return true;

    IMMDevice* device = nullptr;
    HRESULT hr = m_enumerator->GetDevice(dev.id.c_str(), &device);
    if (FAILED(hr)) return false;

    auto stream = std::make_unique<WasapiStream>();
    stream->info   = dev;
    stream->nodeId = nodeId;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          (void**)&stream->client);
    device->Release();
    if (FAILED(hr)) return false;

    // Get the device's mix format as baseline
    WAVEFORMATEX* mixFmt = nullptr;
    stream->client->GetMixFormat(&mixFmt);

    // Build requested format (always float32, vary SR and channels)
    WAVEFORMATEXTENSIBLE* extFmt = (WAVEFORMATEXTENSIBLE*)
        CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
    ZeroMemory(extFmt, sizeof(WAVEFORMATEXTENSIBLE));

    int sr  = (requestedSampleRate > 0) ? requestedSampleRate
                                        : (int)mixFmt->nSamplesPerSec;
    int ch  = (requestedChannels   > 0) ? requestedChannels
                                        : (int)mixFmt->nChannels;
    ch = std::max(1, std::min(ch, 2)); // clamp to 1-2

    extFmt->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    extFmt->Format.nChannels       = (WORD)ch;
    extFmt->Format.nSamplesPerSec  = (DWORD)sr;
    extFmt->Format.wBitsPerSample  = 32;
    extFmt->Format.nBlockAlign     = (WORD)(ch * 4);
    extFmt->Format.nAvgBytesPerSec = (DWORD)(sr * ch * 4);
    extFmt->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extFmt->Samples.wValidBitsPerSample = 32;
    extFmt->dwChannelMask = (ch == 1) ? SPEAKER_FRONT_CENTER
                                       : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    extFmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // Check if driver supports our requested format; fall back to mix format
    WAVEFORMATEX* closestFmt = nullptr;
    hr = stream->client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                           (WAVEFORMATEX*)extFmt, &closestFmt);
    WAVEFORMATEX* useFmt = nullptr;
    if (SUCCEEDED(hr) && hr != S_FALSE) {
        useFmt = (WAVEFORMATEX*)extFmt;
    } else {
        // Requested format not supported — use closest or mix format
        CoTaskMemFree(extFmt);
        useFmt = closestFmt ? closestFmt : mixFmt;
        LOG_INFO("WASAPI node %d: requested format not supported, using system format", nodeId);
    }

    stream->format = useFmt;
    if (useFmt != mixFmt) CoTaskMemFree(mixFmt);

    stream->nativeSampleRate = (int)useFmt->nSamplesPerSec;
    stream->targetSampleRate = 48000;
    stream->resamplePos      = 0.0;
    stream->resamplePrev.assign(useFmt->nChannels, 0.0f);
    if (stream->nativeSampleRate != 48000)
        LOG_INFO("WASAPI node %d: %d Hz -> resample to 48000 Hz",
                 nodeId, stream->nativeSampleRate);

    // Store runtime info into the node (best-effort — graph lock not held here)
    m_pendingRtInfo[nodeId] = { (int)useFmt->nSamplesPerSec,
                                (int)useFmt->wBitsPerSample,
                                (int)useFmt->nChannels };

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    hr = stream->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    200000, 0, useFmt, nullptr);
    if (FAILED(hr)) return false;

    stream->client->GetService(__uuidof(IAudioCaptureClient),
                               (void**)&stream->captureClient);

    stream->eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    stream->client->SetEventHandle(stream->eventHandle);

    stream->ringBuffer = std::make_shared<AudioRingBuffer>(
        !loopback ? useFmt->nSamplesPerSec / 4
                  : useFmt->nSamplesPerSec / 2);
    stream->ringBuffer->channels = useFmt->nChannels;

    stream->running = true;
    stream->client->Start();
    stream->thread = std::thread(&WasapiManager::captureThread, this,
                                 stream.get(), loopback);
    m_streams[nodeId] = std::move(stream);
    return true;
}

bool WasapiManager::openRenderStream(const WasapiDeviceInfo& dev, int nodeId,
                                     int requestedChannels) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    if (m_streams.count(nodeId)) return true;

    IMMDevice* device = nullptr;
    if (FAILED(m_enumerator->GetDevice(dev.id.c_str(), &device))) return false;

    auto stream = std::make_unique<WasapiStream>();
    stream->info   = dev;
    stream->nodeId = nodeId;

    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                     (void**)&stream->client);
    device->Release();

    WAVEFORMATEX* mixFmt = nullptr;
    stream->client->GetMixFormat(&mixFmt);

    WAVEFORMATEX* useFmt = mixFmt;  // default: use mix format as-is

    // If channel override requested, build a custom format
    if (requestedChannels > 0 && requestedChannels != (int)mixFmt->nChannels) {
        int ch = std::max(1, std::min(requestedChannels, 2));
        WAVEFORMATEXTENSIBLE* extFmt = (WAVEFORMATEXTENSIBLE*)
            CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
        ZeroMemory(extFmt, sizeof(WAVEFORMATEXTENSIBLE));
        extFmt->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        extFmt->Format.nChannels       = (WORD)ch;
        extFmt->Format.nSamplesPerSec  = mixFmt->nSamplesPerSec;
        extFmt->Format.wBitsPerSample  = 32;
        extFmt->Format.nBlockAlign     = (WORD)(ch * 4);
        extFmt->Format.nAvgBytesPerSec = mixFmt->nSamplesPerSec * ch * 4;
        extFmt->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        extFmt->Samples.wValidBitsPerSample = 32;
        extFmt->dwChannelMask = (ch == 1) ? SPEAKER_FRONT_CENTER
                                           : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        extFmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        WAVEFORMATEX* closestFmt = nullptr;
        HRESULT hr = stream->client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                                       (WAVEFORMATEX*)extFmt, &closestFmt);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            CoTaskMemFree(mixFmt);
            useFmt = (WAVEFORMATEX*)extFmt;
        } else {
            CoTaskMemFree(extFmt);
            if (closestFmt) { CoTaskMemFree(mixFmt); useFmt = closestFmt; }
        }
    }

    stream->format = useFmt;
    m_pendingRtInfo[nodeId] = { (int)useFmt->nSamplesPerSec,
                                (int)useFmt->wBitsPerSample,
                                (int)useFmt->nChannels };

    stream->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                200000, 0, useFmt, nullptr);
    stream->client->GetService(__uuidof(IAudioRenderClient),
                               (void**)&stream->renderClient);

    stream->eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    stream->client->SetEventHandle(stream->eventHandle);

    stream->ringBuffer = std::make_shared<AudioRingBuffer>(useFmt->nSamplesPerSec / 2); // 500ms
    stream->ringBuffer->channels = useFmt->nChannels;

    stream->running = true;
    stream->client->Start();
    stream->thread = std::thread(&WasapiManager::renderThread, this, stream.get());

    m_streams[nodeId] = std::move(stream);
    return true;
}

void WasapiManager::closeStream(int nodeId) {
    std::unique_ptr<WasapiStream> stream;
    {
        std::lock_guard<std::mutex> lk(m_streamMutex);
        auto it = m_streams.find(nodeId);
        if (it == m_streams.end()) return;
        stream = std::move(it->second);
        m_streams.erase(it);
    }
    if (stream) {
        stream->running = false;
        if (stream->eventHandle) SetEvent(stream->eventHandle);
        if (stream->thread.joinable()) stream->thread.join();
        if (stream->client) { stream->client->Stop(); stream->client->Release(); }
        if (stream->captureClient) stream->captureClient->Release();
        if (stream->renderClient)  stream->renderClient->Release();
        if (stream->format)        CoTaskMemFree(stream->format);
        if (stream->eventHandle)   CloseHandle(stream->eventHandle);
    }
}

void WasapiManager::closeAllStreams() {
    std::vector<int> ids;
    { std::lock_guard<std::mutex> lk(m_streamMutex);
      for (auto& kv : m_streams) ids.push_back(kv.first); }
    for (int id : ids) closeStream(id);
}

// ──────────────────────────────────────────────────────────────────────────────
//  Audio data exchange
// ──────────────────────────────────────────────────────────────────────────────
void WasapiManager::registerVirtualStream(int nodeId, int channels, int sampleRate) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    if (m_streams.count(nodeId)) return; // already registered

    auto stream = std::make_unique<WasapiStream>();
    stream->nodeId            = nodeId;
    stream->nativeSampleRate  = sampleRate;
    stream->targetSampleRate  = sampleRate;
    // Ring buffer: 1 second at given rate and channel count
    stream->ringBuffer = std::make_shared<AudioRingBuffer>(
        (size_t)(sampleRate * channels));
    stream->ringBuffer->channels = channels;
    // No WASAPI handles — client/thread/eventHandle stay null
    m_streams[nodeId] = std::move(stream);
    LOG_INFO("WasapiManager: virtual stream registered nodeId=%d ch=%d sr=%d",
             nodeId, channels, sampleRate);
}

void WasapiManager::unregisterVirtualStream(int nodeId) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return;
    // Only remove if it's a virtual stream (no WASAPI client)
    if (it->second->client == nullptr) {
        m_streams.erase(it);
        LOG_INFO("WasapiManager: virtual stream unregistered nodeId=%d", nodeId);
    }
}

size_t WasapiManager::readFromCapture(int nodeId, float* buf, size_t frames) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0;
    return it->second->ringBuffer->read(buf, frames);
}

// Read raw frames regardless of channel count (mono or stereo)
size_t WasapiManager::readFromCaptureRaw(int nodeId, float* buf,
                                          size_t frames, int /*expectCh*/) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0;
    return it->second->ringBuffer->read(buf, frames);
}

int WasapiManager::streamChannels(int nodeId) const {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0;
    return it->second->ringBuffer->channels;
}

size_t WasapiManager::writeToRender(int nodeId, const float* buf, size_t frames) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0;
    return it->second->ringBuffer->write(buf, frames);
}

void WasapiManager::drainExcess(int nodeId, size_t keepFrames) {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return;
    auto& rb = *it->second->ringBuffer;
    size_t avail = rb.available();
    if (avail > keepFrames) {
        // Discard oldest (avail-keepFrames) frames by advancing readPos
        size_t discard = avail - keepFrames;
        static std::vector<float> trash;
        trash.resize(discard * rb.channels);
        rb.read(trash.data(), discard);
    }
}

float WasapiManager::peakL(int nodeId) const {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0.0f;
    return it->second->peakL.load();
}
float WasapiManager::peakR(int nodeId) const {
    std::lock_guard<std::mutex> lk(m_streamMutex);
    auto it = m_streams.find(nodeId);
    if (it == m_streams.end()) return 0.0f;
    return it->second->peakR.load();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Thread workers
// ──────────────────────────────────────────────────────────────────────────────
void WasapiManager::convertToFloat(const BYTE* src, float* dst, size_t frames,
                                    int ch, WAVEFORMATEX* fmt)
{
    auto* exfmt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
    bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    IsEqualGUID(exfmt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    int bits = fmt->wBitsPerSample;

    for (size_t i = 0; i < frames * ch; ++i) {
        if (isFloat && bits == 32) {
            dst[i] = reinterpret_cast<const float*>(src)[i];
        } else if (bits == 16) {
            dst[i] = reinterpret_cast<const INT16*>(src)[i] / 32768.0f;
        } else if (bits == 24) {
            size_t off = i * 3;
            INT32 v = (INT32)(src[off] | (src[off+1]<<8) | (src[off+2]<<16));
            if (v & 0x800000) v |= 0xFF000000;
            dst[i] = v / 8388608.0f;
        } else if (bits == 32) {
            dst[i] = reinterpret_cast<const INT32*>(src)[i] / 2147483648.0f;
        }
    }
}

void WasapiManager::captureThread(WasapiStream* stream, bool /*loopback*/) {
    DWORD taskIdx = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);

    const int    nativeSR  = stream->nativeSampleRate;
    const int    targetSR  = stream->targetSampleRate;
    const bool   needResamp= (nativeSR != targetSR);
    const double ratio     = needResamp ? (double)nativeSR / targetSR : 1.0;
    int          ch        = stream->format->nChannels;

    std::vector<float> floatBuf;
    std::vector<float> resampOut; // resampled output before ring-buffer write

    while (stream->running) {
        DWORD res = WaitForSingleObject(stream->eventHandle, 200);
        if (res != WAIT_OBJECT_0 || !stream->running) continue;

        BYTE*  data    = nullptr;
        UINT32 frames  = 0;
        DWORD  flags   = 0;
        UINT64 pos     = 0, qpc = 0;

        while (SUCCEEDED(stream->captureClient->GetBuffer(
                              &data, &frames, &flags, &pos, &qpc))
               && frames > 0)
        {
            floatBuf.resize(frames * ch);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                std::fill(floatBuf.begin(), floatBuf.end(), 0.0f);
            else
                convertToFloat(data, floatBuf.data(), frames, ch, stream->format);

            // Apply gain
            if (stream->gainLinear != 1.0f)
                for (auto& s : floatBuf) s *= stream->gainLinear;

            // ── Linear resampling (handles USB mics at 44100 → 48000) ────────
            if (needResamp) {
                // Estimate output frames for this packet
                int outFrames = (int)std::ceil((double)frames * targetSR / nativeSR) + 2;
                resampOut.resize(outFrames * ch);
                int outCount = 0;

                double& pos2 = stream->resamplePos;
                std::vector<float>& prev = stream->resamplePrev;

                while (pos2 < (double)frames && outCount < outFrames) {
                    int   i0   = (int)pos2;
                    float frac = (float)(pos2 - i0);
                    int   i1   = std::min(i0 + 1, (int)frames - 1);

                    for (int c = 0; c < ch; ++c) {
                        float s0 = (i0 == 0) ? prev[c] : floatBuf[i0 * ch + c];
                        float s1 = floatBuf[i1 * ch + c];
                        resampOut[outCount * ch + c] = s0 + frac * (s1 - s0);
                    }
                    ++outCount;
                    pos2 += ratio;
                }
                // Save last frame as 'prev' for next packet
                for (int c = 0; c < ch; ++c)
                    prev[c] = floatBuf[(frames-1) * ch + c];
                pos2 -= (double)frames;
                if (pos2 < 0.0) pos2 = 0.0;

                // Peak metering on resampled data
                float pkL = 0, pkR = 0;
                for (int i = 0; i < outCount; ++i) {
                    float l = std::abs(resampOut[i*ch]);
                    float r = (ch>1) ? std::abs(resampOut[i*ch+1]) : l;
                    if (l > pkL) pkL = l;
                    if (r > pkR) pkR = r;
                }
                stream->peakL = std::max(pkL, stream->peakL.load() * 0.995f);
                stream->peakR = std::max(pkR, stream->peakR.load() * 0.995f);

                stream->ringBuffer->write(resampOut.data(), (size_t)outCount);
            } else {
                // No resampling needed — write directly
                float pkL = 0, pkR = 0;
                for (size_t i = 0; i < frames; ++i) {
                    float l = std::abs(floatBuf[i*ch]);
                    float r = (ch>1) ? std::abs(floatBuf[i*ch+1]) : l;
                    if (l > pkL) pkL = l;
                    if (r > pkR) pkR = r;
                }
                stream->peakL = std::max(pkL, stream->peakL.load() * 0.995f);
                stream->peakR = std::max(pkR, stream->peakR.load() * 0.995f);

                stream->ringBuffer->write(floatBuf.data(), frames);
            }

            stream->captureClient->ReleaseBuffer(frames);
        }
    }
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}

void WasapiManager::renderThread(WasapiStream* stream) {
    DWORD taskIdx = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);

    UINT32 bufferFrames = 0;
    stream->client->GetBufferSize(&bufferFrames);
    int ch = stream->format->nChannels;
    std::vector<float> readBuf(bufferFrames * ch);

    while (stream->running) {
        DWORD res = WaitForSingleObject(stream->eventHandle, 200);
        if (res != WAIT_OBJECT_0 || !stream->running) continue;

        UINT32 padding = 0;
        stream->client->GetCurrentPadding(&padding);
        UINT32 avail = bufferFrames - padding;
        if (avail == 0) continue;

        BYTE* data = nullptr;
        if (FAILED(stream->renderClient->GetBuffer(avail, &data))) continue;

        size_t got = stream->ringBuffer->read(readBuf.data(), avail);
        if (got == 0) {
            // Underrun: silence
            std::fill(readBuf.begin(), readBuf.begin() + avail * ch, 0.0f);
        }

        // Peak
        float pkL = 0, pkR = 0;
        for (size_t i = 0; i < avail; ++i) {
            pkL = std::max(pkL, std::abs(readBuf[i*ch]));
            pkR = std::max(pkR, std::abs(readBuf[i*ch + (ch>1?1:0)]));
        }
        stream->peakL = std::max(pkL, stream->peakL.load() * 0.99f);
        stream->peakR = std::max(pkR, stream->peakR.load() * 0.99f);

        // Convert float → INT16 (shared-mode render is almost always float, but handle it)
        auto* exfmt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(stream->format);
        bool outFloat = (stream->format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (stream->format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         IsEqualGUID(exfmt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        if (outFloat) {
            memcpy(data, readBuf.data(), avail * ch * sizeof(float));
        } else {
            auto* out = reinterpret_cast<INT16*>(data);
            for (size_t i = 0; i < avail * ch; ++i)
                out[i] = (INT16)(std::clamp(readBuf[i], -1.0f, 1.0f) * 32767.0f);
        }

        stream->renderClient->ReleaseBuffer(avail, 0);
    }
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}

// ──────────────────────────────────────────────────────────────────────────────
//  IAudioEndpointVolume helpers
// ──────────────────────────────────────────────────────────────────────────────
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

IAudioEndpointVolume* WasapiManager::openEndpointVolume(const std::wstring& deviceId) const {
    if (!m_enumerator) return nullptr;
    IMMDevice* dev = nullptr;
    if (FAILED(m_enumerator->GetDevice(deviceId.c_str(), &dev))) return nullptr;
    IAudioEndpointVolume* vol = nullptr;
    dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
    dev->Release();
    return vol;
}

float WasapiManager::getEndpointVolume(const std::wstring& id) const {
    auto* vol = openEndpointVolume(id);
    if (!vol) return 1.0f;
    float v = 1.0f;
    vol->GetMasterVolumeLevelScalar(&v);
    vol->Release();
    return v;
}

void WasapiManager::setEndpointVolume(const std::wstring& id, float scalar) {
    auto* vol = openEndpointVolume(id);
    if (!vol) return;
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    vol->SetMasterVolumeLevelScalar(scalar, nullptr);
    vol->Release();
}

bool WasapiManager::getEndpointMute(const std::wstring& id) const {
    auto* vol = openEndpointVolume(id);
    if (!vol) return false;
    BOOL m = FALSE;
    vol->GetMute(&m);
    vol->Release();
    return m != FALSE;
}

void WasapiManager::setEndpointMute(const std::wstring& id, bool mute) {
    auto* vol = openEndpointVolume(id);
    if (!vol) return;
    vol->SetMute(mute ? TRUE : FALSE, nullptr);
    vol->Release();
}

float WasapiManager::getEndpointVolumeDB(const std::wstring& id) const {
    auto* vol = openEndpointVolume(id);
    if (!vol) return 0.0f;
    float db = 0.0f;
    vol->GetMasterVolumeLevel(&db);
    vol->Release();
    return db;
}

void WasapiManager::setEndpointVolumeDB(const std::wstring& id, float db) {
    auto* vol = openEndpointVolume(id);
    if (!vol) return;
    vol->SetMasterVolumeLevel(db, nullptr);
    vol->Release();
}

void WasapiManager::getEndpointVolumeRange(const std::wstring& id,
                                            float& minDb, float& maxDb, float& stepDb) const {
    minDb = -65.25f; maxDb = 0.0f; stepDb = 0.03f;
    auto* vol = openEndpointVolume(id);
    if (!vol) return;
    vol->GetVolumeRange(&minDb, &maxDb, &stepDb);
    vol->Release();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Per-app session volume (IAudioSessionManager2)
// ──────────────────────────────────────────────────────────────────────────────
#include <audioclient.h>

std::vector<WasapiManager::AppSession>
WasapiManager::getAppSessions(const std::wstring& renderDeviceId) const {
    std::vector<AppSession> result;
    if (!m_enumerator) return result;

    IMMDevice* dev = nullptr;
    if (FAILED(m_enumerator->GetDevice(renderDeviceId.c_str(), &dev))) return result;

    IAudioSessionManager2* mgr = nullptr;
    dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr);
    dev->Release();
    if (!mgr) return result;

    IAudioSessionEnumerator* sesEnum = nullptr;
    if (FAILED(mgr->GetSessionEnumerator(&sesEnum))) { mgr->Release(); return result; }

    int count = 0;
    sesEnum->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(sesEnum->GetSession(i, &ctrl))) continue;

        IAudioSessionControl2* ctrl2 = nullptr;
        ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2);

        ISimpleAudioVolume* sav = nullptr;
        ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sav);

        IAudioMeterInformation* meter = nullptr;
        ctrl->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&meter);

        AppSession s;

        // Session display name
        LPWSTR dispName = nullptr;
        if (SUCCEEDED(ctrl->GetDisplayName(&dispName)) && dispName && *dispName) {
            s.name = dispName;
            CoTaskMemFree(dispName);
        }

        // Session identifier
        if (ctrl2) {
            LPWSTR sid = nullptr;
            if (SUCCEEDED(ctrl2->GetSessionIdentifier(&sid)) && sid) {
                s.id = sid;
                CoTaskMemFree(sid);
            }
            // Try process name if display name is empty
            if (s.name.empty()) {
                DWORD pid = 0;
                ctrl2->GetProcessId(&pid);
                if (pid) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hProc) {
                        wchar_t path[MAX_PATH] {};
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
                            std::wstring full = path;
                            auto pos = full.rfind(L'\\');
                            s.name = (pos != std::wstring::npos) ? full.substr(pos+1) : full;
                        }
                        CloseHandle(hProc);
                    }
                }
            }
            ctrl2->Release();
        }
        if (s.name.empty()) s.name = L"<Unknown>";

        if (sav) {
            sav->GetMasterVolume(&s.volume);
            BOOL m = FALSE; sav->GetMute(&m); s.muted = m != FALSE;
            sav->Release();
        }
        if (meter) {
            UINT32 ch = 0;
            meter->GetMeteringChannelCount(&ch);
            if (ch >= 1) {
                std::vector<float> peaks(ch);
                meter->GetChannelsPeakValues(ch, peaks.data());
                s.peakL = peaks[0];
                s.peakR = (ch >= 2) ? peaks[1] : peaks[0];
            }
            meter->Release();
        }

        result.push_back(s);
        ctrl->Release();
    }

    sesEnum->Release();
    mgr->Release();
    return result;
}

void WasapiManager::setAppSessionVolume(const std::wstring& renderDeviceId,
                                         const std::wstring& sessionId, float scalar) {
    if (!m_enumerator) return;
    IMMDevice* dev = nullptr;
    if (FAILED(m_enumerator->GetDevice(renderDeviceId.c_str(), &dev))) return;

    IAudioSessionManager2* mgr = nullptr;
    dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr);
    dev->Release();
    if (!mgr) return;

    IAudioSessionEnumerator* sesEnum = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&sesEnum))) {
        int count = 0; sesEnum->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* ctrl = nullptr;
            if (FAILED(sesEnum->GetSession(i, &ctrl))) continue;
            IAudioSessionControl2* ctrl2 = nullptr;
            ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2);
            if (ctrl2) {
                LPWSTR sid = nullptr;
                ctrl2->GetSessionIdentifier(&sid);
                if (sid && sessionId == sid) {
                    ISimpleAudioVolume* sav = nullptr;
                    ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sav);
                    if (sav) { sav->SetMasterVolume(scalar, nullptr); sav->Release(); }
                }
                if (sid) CoTaskMemFree(sid);
                ctrl2->Release();
            }
            ctrl->Release();
        }
        sesEnum->Release();
    }
    mgr->Release();
}

void WasapiManager::setAppSessionMute(const std::wstring& renderDeviceId,
                                       const std::wstring& sessionId, bool mute) {
    if (!m_enumerator) return;
    IMMDevice* dev = nullptr;
    if (FAILED(m_enumerator->GetDevice(renderDeviceId.c_str(), &dev))) return;

    IAudioSessionManager2* mgr = nullptr;
    dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr);
    dev->Release();
    if (!mgr) return;

    IAudioSessionEnumerator* sesEnum = nullptr;
    if (SUCCEEDED(mgr->GetSessionEnumerator(&sesEnum))) {
        int count = 0; sesEnum->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* ctrl = nullptr;
            if (FAILED(sesEnum->GetSession(i, &ctrl))) continue;
            IAudioSessionControl2* ctrl2 = nullptr;
            ctrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctrl2);
            if (ctrl2) {
                LPWSTR sid = nullptr;
                ctrl2->GetSessionIdentifier(&sid);
                if (sid && sessionId == sid) {
                    ISimpleAudioVolume* sav = nullptr;
                    ctrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sav);
                    if (sav) { sav->SetMute(mute ? TRUE : FALSE, nullptr); sav->Release(); }
                }
                if (sid) CoTaskMemFree(sid);
                ctrl2->Release();
            }
            ctrl->Release();
        }
        sesEnum->Release();
    }
    mgr->Release();
}
