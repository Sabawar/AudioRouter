#pragma once
// ============================================================================
//  ASIO SDK type definitions (compatible with Steinberg ASIO SDK 2.3)
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>     // IUnknown
#include <objbase.h>    // CoCreateInstance, CLSIDFromString
#include <combaseapi.h> // CoInitializeEx

typedef long            ASIOBool;
typedef long            ASIOSampleType;
typedef long            ASIOError;
typedef double          ASIOSampleRate;
typedef long long       ASIOSamples;
typedef long long       ASIOTimeStamp;

#define ASIOFalse       0
#define ASIOTrue        1

// Sample types
#define ASIOSTInt16MSB      0
#define ASIOSTInt24MSB      1
#define ASIOSTInt32MSB      2
#define ASIOSTFloat32MSB    3
#define ASIOSTFloat64MSB    4
#define ASIOSTInt32MSB16    8
#define ASIOSTInt32MSB18    9
#define ASIOSTInt32MSB20    10
#define ASIOSTInt32MSB24    11
#define ASIOSTInt16LSB      16
#define ASIOSTInt24LSB      17
#define ASIOSTInt32LSB      18
#define ASIOSTFloat32LSB    19
#define ASIOSTFloat64LSB    20
#define ASIOSTInt32LSB16    24
#define ASIOSTInt32LSB18    25
#define ASIOSTInt32LSB20    26
#define ASIOSTInt32LSB24    27

// Error codes
#define ASE_OK              0
#define ASE_SUCCESS         0x3f4847a0
#define ASE_NotPresent      -1000
#define ASE_HWMalfunction   -999
#define ASE_InvalidParameter -998
#define ASE_InvalidMode     -997
#define ASE_SPNotAdvancing  -996
#define ASE_NoClock         -995
#define ASE_NoMemory        -994

struct ASIOClockSource {
    long  index;
    long  associatedChannel;
    long  associatedGroup;
    ASIOBool isCurrentSource;
    char  name[32];
};

struct ASIOChannelInfo {
    long      channel;
    ASIOBool  isInput;
    ASIOBool  isActive;
    long      channelGroup;
    ASIOSampleType type;
    char      name[32];
};

struct ASIOBufferInfo {
    ASIOBool  isInput;
    long      channelNum;
    void*     buffers[2];
};

struct ASIOTimeCode {
    double          speed;
    ASIOSamples     timeCodeSamples;
    unsigned long   flags;
    char            future[64];
};

struct AsioTimeInfo {
    double          speed;
    ASIOTimeStamp   systemTime;
    ASIOSamples     samplePosition;
    ASIOSampleRate  sampleRate;
    unsigned long   flags;
    char            reserved[12];
};

struct ASIOTime {
    long            reserved[4];
    AsioTimeInfo    timeInfo;
    ASIOTimeCode    timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

// ASIO message selectors
#define kAsioSelectorSupported          1
#define kAsioEngineVersion              2
#define kAsioResetRequest               3
#define kAsioBufferSizeChange           4
#define kAsioResyncRequest              5
#define kAsioLatenciesChanged           6
#define kAsioSupportsTimeInfo           7
#define kAsioSupportsTimeCode           8
#define kAsioMMCCommand                 9
#define kAsioSupportsInputMonitor       10

// IASIO COM interface GUID
// {00000000-0000-0000-C000-000000000046} is IUnknown
// IASIO — does NOT use standard COM QueryInterface/AddRef/Release dispatch.
// Steinberg's ASIO uses a custom COM-like vtable. We declare the 3 IUnknown
// methods so the compiler is happy; the real driver implements them.
struct IASIO {
    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG   STDMETHODCALLTYPE AddRef()  = 0;
    virtual ULONG   STDMETHODCALLTYPE Release() = 0;
    // IASIO
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void     getDriverName(char* name) = 0;
    virtual long     getDriverVersion() = 0;
    virtual void     getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};
