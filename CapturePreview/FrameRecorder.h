#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include "DeckLinkAPI_h.h"

struct RecordedFrame
{
    BMDPixelFormat pixelFormat = bmdFormatUnspecified;
    int width = 0;
    int height = 0;
    int rowBytes = 0;
    std::vector<uint8_t> bytes;
};

enum class RecordingMode
{
    NativeRaw = 0,
    FFV1Lossless = 1,
    FFV1PackedRGB8ToYCbCr422P12 = 2
};

class FrameRecorder
{
public:
    FrameRecorder();
    ~FrameRecorder();

    bool start(RecordingMode mode, const std::wstring& outputPath,
               double fps, BMDPixelFormat expectedPixelFormat = bmdFormatUnspecified);
    void stop();
    bool isRecording() const { return m_recording; }

    void pushFrame(IDeckLinkVideoInputFrame* frame);
    std::wstring lastError() const;
    uint64_t framesWritten() const { return m_framesWritten; }

private:
    void workerProc();
    bool convertFrame(const RecordedFrame& frame, std::vector<uint8_t>& payload, std::string& inputPixFmt, std::string& outputPixFmt) const;
    bool startFFmpeg(const RecordedFrame& frame, const std::string& inputPixFmt, const std::string& outputPixFmt);
    static std::wstring quoteWide(const std::wstring& value);
    static std::string quote(const std::wstring& value);
    static std::string narrow(const std::wstring& value);

    static bool ConvertUYVYToYUV422P(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool ConvertV210ToYUV422P10LE(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool ConvertR210ToGBRP10LE(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool ConvertR10XToGBRP10LE(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool ConvertR12BToGBRP12LE(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool ConvertR12LToGBRP12LE(const RecordedFrame& frame, std::vector<uint8_t>& out);
    static bool DecodePackedRGB8TransportToYCbCr422P12(const RecordedFrame& frame, std::vector<uint8_t>& out);

    mutable std::mutex m_stateMutex;
    std::wstring m_lastError;
    std::wstring m_outputPath;
    RecordingMode m_mode = RecordingMode::NativeRaw;
    double m_fps = 30.0;
    BMDPixelFormat m_expectedPixelFormat = bmdFormatUnspecified;

    FILE* m_pipe = nullptr;
    HANDLE m_ffmpegProcess = nullptr;
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<RecordedFrame> m_queue;
    bool m_stopRequested = false;
    std::atomic<bool> m_recording{ false };
    std::atomic<uint64_t> m_framesWritten{ 0 };
    size_t m_maxQueue = 12;
};
