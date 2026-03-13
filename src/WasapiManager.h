#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <unordered_map>

// ============================================================================
//  WASAPI device descriptor (discovered by enumeration)
// ============================================================================
struct WasapiDeviceInfo {
    std::wstring id;
    std::wstring friendlyName;
    bool         isCapture     { false }; // false = render
    bool         isLoopback    { false };
    bool         isDefault     { false };
    bool         isVirtualCable{ false };
    int          channels      { 2 };
    int          sampleRate    { 48000 };
};

// ============================================================================
//  Ring buffer for inter-thread audio transport
// ============================================================================
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacitySamples);
    size_t write(const float* data, size_t frames);
    size_t read (float* data,       size_t frames);
    size_t available() const;
    void   reset();
    int    channels { 2 };
private:
    std::vector<float> m_buf;
    std::atomic<size_t> m_writePos { 0 };
    std::atomic<size_t> m_readPos  { 0 };
};

// ============================================================================
//  A single active WASAPI stream (capture or render)
// ============================================================================
struct WasapiStream {
    WasapiDeviceInfo          info;
    IAudioClient*             client      { nullptr };
    IAudioCaptureClient*      captureClient { nullptr };
    IAudioRenderClient*       renderClient  { nullptr };
    WAVEFORMATEX*             format      { nullptr };
    HANDLE                    eventHandle { nullptr };
    std::thread               thread;
    std::atomic<bool>         running     { false };
    std::shared_ptr<AudioRingBuffer> ringBuffer;
    std::atomic<float>        peakL       { 0.0f };
    std::atomic<float>        peakR       { 0.0f };
    float                     gainLinear  { 1.0f };
    int                       nodeId      { -1 };

    // Resampler state (linear interpolation, for USB mics at 44100 → 48000)
    int    nativeSampleRate { 48000 };  // actual device rate
    int    targetSampleRate { 48000 };  // engine mix rate
    double resamplePos      { 0.0 };   // fractional read position
    std::vector<float> resamplePrev;   // last frame (for interpolation)
};

// ============================================================================
//  WasapiManager  –  owns all active WASAPI streams
// ============================================================================
class WasapiManager {
public:
    WasapiManager();
    ~WasapiManager();

    bool init();
    void shutdown();

    // Device enumeration
    std::vector<WasapiDeviceInfo> enumerateDevices();
    void refreshDeviceList();

    // Stream lifecycle
    bool openCaptureStream (const WasapiDeviceInfo& dev, int nodeId,
                            bool loopback = false,
                            int requestedChannels = 0,
                            int requestedSampleRate = 0);
    bool openRenderStream  (const WasapiDeviceInfo& dev, int nodeId,
                            int requestedChannels = 0);
    void closeStream       (int nodeId);
    void closeAllStreams    ();

    // Audio data access (called from AudioEngine mix thread)
    // Register a virtual ring-buffer stream for non-WASAPI sources (e.g. ASIO).
    // This allows the mix engine to read/write the node through the same API.
    void registerVirtualStream(int nodeId, int channels, int sampleRate = 48000);
    void unregisterVirtualStream(int nodeId);

    size_t readFromCapture (int nodeId, float* buf, size_t frames);
    // Read raw mono data (for mono-capable sources)
    size_t readFromCaptureRaw(int nodeId, float* buf, size_t frames, int expectCh);
    // Returns native channel count of the stream (1=mono, 2=stereo, 0=not found)
    int    streamChannels  (int nodeId) const;

    // Runtime format info filled when stream opens
    struct RtInfo { int sampleRate; int bitDepth; int channels; };
    RtInfo getRtInfo(int nodeId) const {
        std::lock_guard<std::mutex> lk(m_streamMutex);
        auto it = m_pendingRtInfo.find(nodeId);
        return (it != m_pendingRtInfo.end()) ? it->second : RtInfo{0,0,0};
    }
    size_t writeToRender   (int nodeId, const float* buf, size_t frames);
    // Drain excess captured data to prevent latency build-up.
    // Keeps at most `keepFrames` frames in the buffer.
    void   drainExcess     (int nodeId, size_t keepFrames);

    // Peak levels for metering
    float peakL(int nodeId) const;
    float peakR(int nodeId) const;

    const std::vector<WasapiDeviceInfo>& deviceList() const { return m_devices; }

    // Default device IDs
    std::wstring defaultPlaybackId()  const { return m_defaultPlaybackId; }
    std::wstring defaultCaptureId()   const { return m_defaultCaptureId; }

    // ── System endpoint volume (IAudioEndpointVolume) ───────────────────────
    // Get/set the Windows system volume for a device (0.0 – 1.0 scalar)
    float  getEndpointVolume (const std::wstring& deviceId) const;
    void   setEndpointVolume (const std::wstring& deviceId, float scalar);
    bool   getEndpointMute   (const std::wstring& deviceId) const;
    void   setEndpointMute   (const std::wstring& deviceId, bool mute);
    // dBFS version (Windows stores as dB internally)
    float  getEndpointVolumeDB(const std::wstring& deviceId) const;
    void   setEndpointVolumeDB(const std::wstring& deviceId, float db);
    // Range query
    void   getEndpointVolumeRange(const std::wstring& deviceId,
                                  float& minDb, float& maxDb, float& stepDb) const;

    // Session volume control (per-app, via IAudioSessionManager2)
    struct AppSession {
        std::wstring name;
        std::wstring id;
        float        volume  { 1.0f };
        bool         muted   { false };
        float        peakL   { 0.0f };
        float        peakR   { 0.0f };
    };
    std::vector<AppSession> getAppSessions(const std::wstring& renderDeviceId) const;
    void setAppSessionVolume(const std::wstring& renderDeviceId,
                             const std::wstring& sessionId, float scalar);
    void setAppSessionMute  (const std::wstring& renderDeviceId,
                             const std::wstring& sessionId, bool mute);

private:
    IAudioEndpointVolume* openEndpointVolume(const std::wstring& deviceId) const;
    void captureThread(WasapiStream* stream, bool loopback);
    void renderThread (WasapiStream* stream);
    static float pcm16ToFloat(INT16 s) { return s / 32768.0f; }
    static float pcm24ToFloat(const BYTE* p);
    static void  convertToFloat(const BYTE* src, float* dst, size_t frames,
                                int channels, WAVEFORMATEX* fmt);

    IMMDeviceEnumerator*             m_enumerator  { nullptr };
    std::vector<WasapiDeviceInfo>    m_devices;
    mutable std::mutex               m_devicesMutex;   // protects m_devices
    std::unordered_map<int, std::unique_ptr<WasapiStream>> m_streams;
    mutable std::mutex               m_streamMutex;
    std::unordered_map<int, RtInfo>  m_pendingRtInfo; // filled when stream opens
    std::wstring                     m_defaultPlaybackId;
    std::wstring                     m_defaultCaptureId;
};
