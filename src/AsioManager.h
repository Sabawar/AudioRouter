#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <combaseapi.h>
#include "asio.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

// ============================================================================
//  ASIO device descriptor
// ============================================================================
struct AsioDeviceInfo {
    std::string  name;
    std::string  clsid;
    CLSID        guid {};
    int          maxInputs  { 0 };
    int          maxOutputs { 0 };
    int          sampleRate { 48000 };
    int          bufferSize { 512 };
    bool         useMinBuffer { true };
    std::wstring dllPath;        // full path to the ASIO DLL
    bool         is32bit { false }; // driver from WOW6432 key (32-bit)
};

// ============================================================================
//  Callback typedefs
// ============================================================================
using AsioBufferCallback = std::function<void(
    long bufIndex,
    const std::vector<float>& inputs,  // interleaved
    std::vector<float>& outputs,       // interleaved, fill this
    int numFrames,
    int numIn,
    int numOut
)>;

// ============================================================================
//  AsioManager  –  owns one ASIO driver at a time
// ============================================================================
class AsioManager {
public:
    AsioManager();
    ~AsioManager();

    // Enumerate all installed ASIO drivers from registry
    static std::vector<AsioDeviceInfo> enumerateDrivers();

    // Load / unload a driver
    bool loadDriver(const AsioDeviceInfo& dev, HWND ownerWindow);
    void unloadDriver();
    bool isLoaded() const { return m_asio != nullptr; }

    // Get info after load
    int  numInputChannels()  const { return m_numIn;  }
    int  numOutputChannels() const { return m_numOut; }
    int  bufferSize()        const { return m_bufSize; }
    int  bufferSizeMin()     const { return m_bufSizeMin; }
    int  bufferSizePref()    const { return m_bufSizePref; }
    double sampleRate()      const { return m_sampleRate; }
    std::string driverName() const { return m_driverName; }

    // Open control panel (driver UI)
    void showControlPanel();

    // Re-query driver for current SR/buffer without reloading.
    // Returns true if values changed since last load.
    bool queryLiveSettings();
    const AsioDeviceInfo& currentDevice() const { return m_current; }

    // Create buffers and start streaming
    bool createBuffersAndStart(int numInputs, int numOutputs,
                               AsioBufferCallback callback);
    void stopAndDisposeBuffers();

    bool isRunning() const { return m_running; }

    // Input/output peak levels
    float inputPeak(int ch)  const;
    float outputPeak(int ch) const;

    // Node IDs in the routing graph for this ASIO device
    int inputNodeId  { -1 };
    int outputNodeId { -1 };

private:
    // ASIO callback shim (ASIO uses global/static callbacks)
    static void  onBufferSwitch(long index, ASIOBool direct);
    static void  onSampleRateChanged(ASIOSampleRate rate);
    static long  onAsioMessage(long sel, long val, void* msg, double* opt);
    static ASIOTime* onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool direct);

    void processBuffers(long bufIndex);

    static AsioManager*     s_instance;   // singleton for callbacks

    IASIO*                  m_asio       { nullptr };
    HMODULE                 m_driverDll  { nullptr };
    std::vector<ASIOBufferInfo> m_bufInfos;
    ASIOCallbacks           m_callbacks {};
    ASIOChannelInfo         m_chanInfo[64] {};

    int    m_numIn     { 0 };
    int    m_numOut    { 0 };
    int    m_bufSize   { 512 };
    int    m_bufSizeMin  { 512 };
    int    m_bufSizePref { 512 };
    double m_sampleRate{ 48000.0 };
    bool   m_running   { false };
    std::string m_driverName;
    AsioDeviceInfo m_current;

    AsioBufferCallback m_userCallback;
    std::vector<float> m_inputBuf;
    std::vector<float> m_outputBuf;
    std::vector<float> m_inPeaks;
    std::vector<float> m_outPeaks;
};
