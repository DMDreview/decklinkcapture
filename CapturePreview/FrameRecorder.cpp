#include "stdafx.h"
#include "FrameRecorder.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <io.h>
#include <fcntl.h>

namespace
{
    static uint16_t readLE16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
    static uint32_t readBE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
    static uint32_t readLE32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    static void putLE16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

    static void setError(std::mutex& mutex, std::wstring& dst, const wchar_t* msg)
    {
        std::lock_guard<std::mutex> lock(mutex);
        dst = msg;
    }
}

FrameRecorder::FrameRecorder() = default;

FrameRecorder::~FrameRecorder()
{
    stop();
}

std::string FrameRecorder::narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), (int)value.size(), &result[0], n, nullptr, nullptr);
    return result;
}

std::string FrameRecorder::quote(const std::wstring& value)
{
    std::string s = narrow(value);
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

std::wstring FrameRecorder::quoteWide(const std::wstring& value)
{
    std::wstring out = L"\"";
    for (wchar_t c : value)
    {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

bool FrameRecorder::startFFmpeg(const RecordedFrame& frame, const std::string& inputPixFmt, const std::string& outputPixFmt)
{
    std::wstring outputPath;
    double fps;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        outputPath = m_outputPath;
        fps = m_fps;
    }

    std::ostringstream fpsText;
    fpsText << fps;
    const std::string fpsString = fpsText.str();
    const std::wstring fpsWide(fpsString.begin(), fpsString.end());
    const std::wstring inputWide(inputPixFmt.begin(), inputPixFmt.end());
    const std::wstring outputWide(outputPixFmt.begin(), outputPixFmt.end());

    std::wstring cmd = L"ffmpeg.exe -hide_banner -loglevel error -y -f rawvideo -pix_fmt " +
        inputWide +
        L" -s " + std::to_wstring(frame.width) + L"x" + std::to_wstring(frame.height) +
        L" -r " + fpsWide + L" -i - -an -c:v ffv1 -level 3 -coder 1 -context 1 -slicecrc 1 -pix_fmt " +
        outputWide;

    // The packed RGB8 transport mode has a fixed output colour interpretation:
    // BT.2020 non-constant-luminance matrix, SMPTE ST 2084 (PQ), video range.
    // Other recording modes retain their normal colour metadata.
    if (m_mode == RecordingMode::FFV1PackedRGB8ToYCbCr422P12)
    {
        cmd += L" -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv";
    }

    cmd += L" -f matroska " + quoteWide(outputPath);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0))
    {
        setError(m_stateMutex, m_lastError, L"CreatePipe failed while starting ffmpeg.exe.");
        return false;
    }
    if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0))
    {
        CloseHandle(stdinRead); CloseHandle(stdinWrite);
        setError(m_stateMutex, m_lastError, L"SetHandleInformation failed while starting ffmpeg.exe.");
        return false;
    }

    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE)
    {
        CloseHandle(stdinRead); CloseHandle(stdinWrite);
        setError(m_stateMutex, m_lastError, L"Unable to open NUL for ffmpeg.exe output.");
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> commandLine(cmd.begin(), cmd.end());
    commandLine.push_back(L'\\0');

    BOOL ok = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdinRead);
    CloseHandle(nul);

    if (!ok)
    {
        CloseHandle(stdinWrite);
        wchar_t msg[160];
        swprintf_s(msg, L"Unable to start ffmpeg.exe. CreateProcessW failed: error %lu", GetLastError());
        setError(m_stateMutex, m_lastError, msg);
        return false;
    }

    CloseHandle(pi.hThread);
    m_ffmpegProcess = pi.hProcess;

    int fd = _open_osfhandle((intptr_t)stdinWrite, _O_WRONLY | _O_BINARY);
    if (fd < 0)
    {
        CloseHandle(stdinWrite);
        TerminateProcess(m_ffmpegProcess, 1);
        WaitForSingleObject(m_ffmpegProcess, 2000);
        CloseHandle(m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
        setError(m_stateMutex, m_lastError, L"Unable to create FFV1 input stream.");
        return false;
    }

    m_pipe = _fdopen(fd, "wb");
    if (!m_pipe)
    {
        _close(fd);
        TerminateProcess(m_ffmpegProcess, 1);
        WaitForSingleObject(m_ffmpegProcess, 2000);
        CloseHandle(m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
        setError(m_stateMutex, m_lastError, L"Unable to open FFV1 input stream.");
        return false;
    }

    return true;
}

bool FrameRecorder::start(RecordingMode mode, const std::wstring& outputPath, double fps, BMDPixelFormat expectedPixelFormat)
{
    stop();

    if (outputPath.empty())
    {
        setError(m_stateMutex, m_lastError, L"Output file name is empty.");
        return false;
    }
    if (fps <= 0.0)
    {
        setError(m_stateMutex, m_lastError, L"Invalid frame rate.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_lastError.clear();
        m_outputPath = outputPath;
        m_mode = mode;
        m_fps = fps;
        m_expectedPixelFormat = expectedPixelFormat;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_stopRequested = false;
    }

    m_framesWritten = 0;
    m_recording = true;
    m_worker = std::thread(&FrameRecorder::workerProc, this);
    return true;
}

void FrameRecorder::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_recording && !m_worker.joinable())
            return;
        m_stopRequested = true;
    }
    m_queueCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    m_recording = false;
}

std::wstring FrameRecorder::lastError() const
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastError;
}

void FrameRecorder::pushFrame(IDeckLinkVideoInputFrame* frame)
{
    if (!frame || !m_recording)
        return;

    BMDPixelFormat pf = frame->GetPixelFormat();
    if (m_expectedPixelFormat != bmdFormatUnspecified && pf != m_expectedPixelFormat)
        return;

    // DeckLink SDK 16.0 exposes the actual frame memory through
    // IDeckLinkVideoBuffer. Do not call GetBytes() on IDeckLinkVideoFrame.
    CComQIPtr<IDeckLinkVideoBuffer> videoBuffer(frame);
    if (!videoBuffer)
        return;

    const HRESULT startHr = videoBuffer->StartAccess(bmdBufferAccessRead);
    if (startHr != S_OK)
    {
        wchar_t msg[128];
        swprintf_s(msg, L"IDeckLinkVideoBuffer::StartAccess failed: HRESULT=0x%08lX", (unsigned long)startHr);
        setError(m_stateMutex, m_lastError, msg);
        return;
    }

    void* bytes = nullptr;
    const HRESULT getHr = videoBuffer->GetBytes(&bytes);
    if (getHr != S_OK || !bytes)
    {
        wchar_t msg[128];
        swprintf_s(msg, L"IDeckLinkVideoBuffer::GetBytes failed: HRESULT=0x%08lX", (unsigned long)getHr);
        setError(m_stateMutex, m_lastError, msg);
        videoBuffer->EndAccess(bmdBufferAccessRead);
        return;
    }

    const int width = frame->GetWidth();
    const int height = frame->GetHeight();
    const int rowBytes = frame->GetRowBytes();
    if (width <= 0 || height <= 0 || rowBytes <= 0)
    {
        videoBuffer->EndAccess(bmdBufferAccessRead);
        setError(m_stateMutex, m_lastError, L"Invalid captured frame dimensions or row bytes.");
        return;
    }

    RecordedFrame copy;
    copy.pixelFormat = pf;
    copy.width = width;
    copy.height = height;
    copy.rowBytes = rowBytes;
    copy.bytes.resize(size_t(rowBytes) * size_t(height));
    memcpy(copy.bytes.data(), bytes, copy.bytes.size());
    const HRESULT endHr = videoBuffer->EndAccess(bmdBufferAccessRead);
    if (endHr != S_OK)
    {
        wchar_t msg[128];
        swprintf_s(msg, L"IDeckLinkVideoBuffer::EndAccess failed: HRESULT=0x%08lX", (unsigned long)endHr);
        setError(m_stateMutex, m_lastError, msg);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stopRequested)
            return;
        if (m_queue.size() >= m_maxQueue)
        {
            // Never block the DeckLink callback. Drop the oldest queued frame.
            m_queue.pop_front();
        }
        m_queue.emplace_back(std::move(copy));
    }
    m_queueCv.notify_one();
}

void FrameRecorder::workerProc()
{
    std::wstring outputPath;
    RecordingMode mode;
    double fps;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        outputPath = m_outputPath;
        mode = m_mode;
        fps = m_fps;
    }

    FILE* pipe = nullptr;
    bool ffmpegStarted = false;
    std::string command;

    while (true)
    {
        RecordedFrame frame;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
            // Stop means stop accepting/encoding new frames immediately. Do not
            // drain the backlog: FFV1 can be much slower than real time and
            // draining the queue here made Stop Recording appear to hang.
            if (m_stopRequested)
            {
                m_queue.clear();
                break;
            }
            frame = std::move(m_queue.front());
            m_queue.pop_front();
        }

        if (mode == RecordingMode::NativeRaw)
        {
            // Native raw mode opens one exact-buffer stream. The first frame establishes the file.
            if (!pipe)
            {
                if (_wfopen_s(&pipe, outputPath.c_str(), L"wb") != 0 || !pipe)
                {
                    setError(m_stateMutex, m_lastError, L"Unable to open native raw output file.");
                    break;
                }
            }
            fwrite(frame.bytes.data(), 1, frame.bytes.size(), pipe);
            ++m_framesWritten;
            continue;
        }

        if (!ffmpegStarted)
        {
            std::string inputPixFmt, outputPixFmt;
            std::vector<uint8_t> dummy;
            if (!convertFrame(frame, dummy, inputPixFmt, outputPixFmt))
            {
                setError(m_stateMutex, m_lastError, L"Unsupported DeckLink pixel format for FFV1 recording.");
                break;
            }

            if (!startFFmpeg(frame, inputPixFmt, outputPixFmt))
                break;

            pipe = m_pipe;
            ffmpegStarted = true;
        }

        std::vector<uint8_t> payload;
        std::string inputPixFmt, outputPixFmt;
        if (!convertFrame(frame, payload, inputPixFmt, outputPixFmt))
        {
            setError(m_stateMutex, m_lastError, L"Failed to convert captured frame to FFV1 input format.");
            break;
        }

        if (fwrite(payload.data(), 1, payload.size(), pipe) != payload.size())
        {
            setError(m_stateMutex, m_lastError, L"FFV1 encoder pipe closed unexpectedly. Check ffmpeg.exe and its error output.");
            break;
        }
        ++m_framesWritten;
    }

    if (pipe)
    {
        fclose(pipe);
        m_pipe = nullptr;
    }
    if (m_ffmpegProcess)
    {
        DWORD waitResult = WaitForSingleObject(m_ffmpegProcess, 10000);
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateProcess(m_ffmpegProcess, 1);
            WaitForSingleObject(m_ffmpegProcess, 2000);
            setError(m_stateMutex, m_lastError, L"ffmpeg.exe did not terminate after closing the input stream.");
        }
        CloseHandle(m_ffmpegProcess);
        m_ffmpegProcess = nullptr;
    }

    m_recording = false;
}

bool FrameRecorder::convertFrame(const RecordedFrame& frame, std::vector<uint8_t>& payload, std::string& inputPixFmt, std::string& outputPixFmt) const
{
    switch (frame.pixelFormat)
    {
        case bmdFormat8BitYUV:
            if (!ConvertUYVYToYUV422P(frame, payload)) return false;
            inputPixFmt = "yuv422p"; outputPixFmt = "yuv422p"; return true;
        case bmdFormat10BitYUV:
            if (!ConvertV210ToYUV422P10LE(frame, payload)) return false;
            inputPixFmt = "yuv422p10le"; outputPixFmt = "yuv422p10le"; return true;
        case bmdFormat8BitARGB:
            payload = frame.bytes; inputPixFmt = "argb"; outputPixFmt = "argb"; return true;
        case bmdFormat8BitBGRA:
            if (m_mode == RecordingMode::FFV1PackedRGB8ToYCbCr422P12)
            {
                if (!DecodePackedRGB8TransportToYCbCr422P12(frame, payload)) return false;
                inputPixFmt = "yuv422p12le"; outputPixFmt = "yuv422p12le"; return true;
            }
            payload = frame.bytes; inputPixFmt = "bgra"; outputPixFmt = "bgra"; return true;
        case bmdFormat10BitRGB:
            if (!ConvertR210ToGBRP10LE(frame, payload)) return false;
            inputPixFmt = "gbrp10le"; outputPixFmt = "gbrp10le"; return true;
        case bmdFormat12BitRGB:
            if (!ConvertR12BToGBRP12LE(frame, payload)) return false;
            inputPixFmt = "gbrp12le"; outputPixFmt = "gbrp12le"; return true;
        case bmdFormat12BitRGBLE:
            if (!ConvertR12LToGBRP12LE(frame, payload)) return false;
            inputPixFmt = "gbrp12le"; outputPixFmt = "gbrp12le"; return true;
        case bmdFormat10BitRGBXLE:
            if (!ConvertR10XToGBRP10LE(frame, payload)) return false;
            inputPixFmt = "gbrp10le"; outputPixFmt = "gbrp10le"; return true;
        case bmdFormat10BitRGBX:
            if (!ConvertR210ToGBRP10LE(frame, payload)) return false;
            inputPixFmt = "gbrp10le"; outputPixFmt = "gbrp10le"; return true;
        default:
            return false;
    }
}

bool FrameRecorder::ConvertUYVYToYUV422P(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    if (f.width & 1) return false;
    const size_t plane = size_t(f.width) * size_t(f.height);
    out.resize(plane + plane / 2 + plane / 2);
    uint8_t* Y = out.data(); uint8_t* U = Y + plane; uint8_t* V = U + plane / 2;
    for (int y = 0; y < f.height; ++y)
    {
        const uint8_t* s = f.bytes.data() + size_t(y) * f.rowBytes;
        for (int x = 0; x < f.width; x += 2)
        {
            size_t o = size_t(y) * f.width + x;
            size_t c = size_t(y) * (f.width / 2) + x / 2;
            U[c] = s[0]; Y[o] = s[1]; V[c] = s[2]; Y[o + 1] = s[3]; s += 4;
        }
    }
    return true;
}

bool FrameRecorder::ConvertV210ToYUV422P10LE(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    if (f.width & 1) return false;
    const size_t yPlane = size_t(f.width) * f.height;
    const size_t cPlane = yPlane / 2;
    out.resize((yPlane + cPlane + cPlane) * 2);
    uint16_t* Y = reinterpret_cast<uint16_t*>(out.data());
    uint16_t* U = Y + yPlane;
    uint16_t* V = U + cPlane;
    for (int y = 0; y < f.height; ++y)
    {
        const uint8_t* row = f.bytes.data() + size_t(y) * f.rowBytes;
        const size_t groups = (size_t(f.width) + 5) / 6;
        for (size_t g = 0; g < groups; ++g)
        {
            const uint8_t* p = row + g * 16;
            const uint32_t w0 = readLE32(p + 0);
            const uint32_t w1 = readLE32(p + 4);
            const uint32_t w2 = readLE32(p + 8);
            const uint32_t w3 = readLE32(p + 12);
            const size_t x = g * 6;
            const size_t rem = size_t(f.width) - x;
            if (rem >= 1) U[size_t(y)*(f.width/2)+x/2] = uint16_t(w0 & 0x3ff);
            if (rem >= 1) Y[size_t(y)*f.width+x] = uint16_t((w0 >> 10) & 0x3ff);
            if (rem >= 2) V[size_t(y)*(f.width/2)+x/2] = uint16_t((w0 >> 20) & 0x3ff);
            if (rem >= 2) Y[size_t(y)*f.width+x+1] = uint16_t(w1 & 0x3ff);
            if (rem >= 3) U[size_t(y)*(f.width/2)+x/2+1] = uint16_t((w1 >> 10) & 0x3ff);
            if (rem >= 4) Y[size_t(y)*f.width+x+2] = uint16_t((w1 >> 20) & 0x3ff);
            if (rem >= 4) V[size_t(y)*(f.width/2)+x/2+1] = uint16_t(w2 & 0x3ff);
            if (rem >= 5) Y[size_t(y)*f.width+x+3] = uint16_t((w2 >> 10) & 0x3ff);
            if (rem >= 6) U[size_t(y)*(f.width/2)+x/2+2] = uint16_t((w2 >> 20) & 0x3ff);
            if (rem >= 6) Y[size_t(y)*f.width+x+4] = uint16_t(w3 & 0x3ff);
            if (rem >= 6) V[size_t(y)*(f.width/2)+x/2+2] = uint16_t((w3 >> 10) & 0x3ff);
            if (rem >= 6) Y[size_t(y)*f.width+x+5] = uint16_t((w3 >> 20) & 0x3ff);
        }
    }
    return true;
}

bool FrameRecorder::ConvertR210ToGBRP10LE(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    const size_t plane = size_t(f.width) * f.height;
    out.resize(plane * 3 * 2);
    uint16_t* G = reinterpret_cast<uint16_t*>(out.data());
    uint16_t* B = G + plane;
    uint16_t* R = B + plane;
    for (int y=0;y<f.height;++y)
    {
        const uint8_t* row=f.bytes.data()+size_t(y)*f.rowBytes;
        for(int x=0;x<f.width;++x)
        {
            uint32_t w=readBE32(row+size_t(x)*4);
            B[size_t(y)*f.width+x]=(uint16_t)((w>>22)&0x3ff);
            G[size_t(y)*f.width+x]=(uint16_t)((w>>12)&0x3ff);
            R[size_t(y)*f.width+x]=(uint16_t)((w>>2)&0x3ff);
        }
    }
    return true;
}

bool FrameRecorder::ConvertR10XToGBRP10LE(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    const size_t plane=size_t(f.width)*f.height;
    out.resize(plane*3*2);
    uint16_t* G=reinterpret_cast<uint16_t*>(out.data()); uint16_t* B=G+plane; uint16_t* R=B+plane;
    for(int y=0;y<f.height;++y){ const uint8_t* row=f.bytes.data()+size_t(y)*f.rowBytes; for(int x=0;x<f.width;++x){uint32_t w=readLE32(row+size_t(x)*4); R[size_t(y)*f.width+x]=(w>>22)&0x3ff; G[size_t(y)*f.width+x]=(w>>12)&0x3ff; B[size_t(y)*f.width+x]=(w>>2)&0x3ff;}}
    return true;
}

// R12B/R12L are SMPTE 268M DPX C4 packed. The implementation below decodes the 12-bit components
// using the byte layouts specified by the DeckLink SDK. Output is planar G/B/R 12-bit little-endian.
static void decodeR12BlockToPlanes(const uint8_t* p, bool bigEndianWords, uint16_t* G, uint16_t* B, uint16_t* R, size_t base)
{
    uint8_t q[36];
    for (int w = 0; w < 9; ++w)
    {
        if (bigEndianWords)
        {
            q[w*4+0] = p[w*4+3];
            q[w*4+1] = p[w*4+2];
            q[w*4+2] = p[w*4+1];
            q[w*4+3] = p[w*4+0];
        }
        else
        {
            q[w*4+0] = p[w*4+0];
            q[w*4+1] = p[w*4+1];
            q[w*4+2] = p[w*4+2];
            q[w*4+3] = p[w*4+3];
        }
    }

    auto r0 = uint16_t(q[0] | ((q[1] & 0x0F) << 8));
    auto g0 = uint16_t((q[1] >> 4) | (q[2] << 4));
    auto b0 = uint16_t(q[3] | ((q[4] & 0x0F) << 8));
    auto r1 = uint16_t((q[4] >> 4) | (q[5] << 4));
    auto g1 = uint16_t(q[6] | ((q[7] & 0x0F) << 8));
    auto b1 = uint16_t((q[7] >> 4) | (q[8] << 4));
    auto r2 = uint16_t(q[9] | ((q[10] & 0x0F) << 8));
    auto g2 = uint16_t((q[10] >> 4) | (q[11] << 4));
    auto b2 = uint16_t(q[12] | ((q[13] & 0x0F) << 8));
    auto r3 = uint16_t((q[13] >> 4) | (q[14] << 4));
    auto g3 = uint16_t(q[15] | ((q[16] & 0x0F) << 8));
    auto b3 = uint16_t((q[16] >> 4) | (q[17] << 4));
    auto r4 = uint16_t(q[18] | ((q[19] & 0x0F) << 8));
    auto g4 = uint16_t((q[19] >> 4) | (q[20] << 4));
    auto b4 = uint16_t(q[21] | ((q[22] & 0x0F) << 8));
    auto r5 = uint16_t((q[22] >> 4) | (q[23] << 4));
    auto g5 = uint16_t(q[24] | ((q[25] & 0x0F) << 8));
    auto b5 = uint16_t((q[25] >> 4) | (q[26] << 4));
    auto r6 = uint16_t(q[27] | ((q[28] & 0x0F) << 8));
    auto g6 = uint16_t((q[28] >> 4) | (q[29] << 4));
    auto b6 = uint16_t(q[30] | ((q[31] & 0x0F) << 8));
    auto r7 = uint16_t((q[31] >> 4) | (q[32] << 4));
    auto g7 = uint16_t(q[33] | ((q[34] & 0x0F) << 8));
    auto b7 = uint16_t((q[34] >> 4) | (q[35] << 4));

    const uint16_t rr[8] = {r0,r1,r2,r3,r4,r5,r6,r7};
    const uint16_t gg[8] = {g0,g1,g2,g3,g4,g5,g6,g7};
    const uint16_t bb[8] = {b0,b1,b2,b3,b4,b5,b6,b7};
    for (int i=0;i<8;++i) { R[base+i]=rr[i]; G[base+i]=gg[i]; B[base+i]=bb[i]; }
}

bool FrameRecorder::ConvertR12LToGBRP12LE(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    const size_t plane=size_t(f.width)*f.height;
    out.resize(plane*3*2);
    uint16_t* G=reinterpret_cast<uint16_t*>(out.data());
    uint16_t* B=G+plane;
    uint16_t* R=B+plane;
    const int blocks=(f.width+7)/8;
    for(int y=0;y<f.height;++y)
    {
        const uint8_t* row=f.bytes.data()+size_t(y)*f.rowBytes;
        for(int block=0;block<blocks;++block)
        {
            const int x=block*8;
            const int count=std::min(8,f.width-x);
            if (count < 8) return false; // DeckLink row format is block based; don't guess partial C4 blocks.
            decodeR12BlockToPlanes(row+size_t(block)*36,false,G,B,R,size_t(y)*f.width+x);
        }
    }
    return true;
}

bool FrameRecorder::ConvertR12BToGBRP12LE(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    const size_t plane=size_t(f.width)*f.height;
    out.resize(plane*3*2);
    uint16_t* G=reinterpret_cast<uint16_t*>(out.data());
    uint16_t* B=G+plane;
    uint16_t* R=B+plane;
    const int blocks=(f.width+7)/8;
    for(int y=0;y<f.height;++y)
    {
        const uint8_t* row=f.bytes.data()+size_t(y)*f.rowBytes;
        for(int block=0;block<blocks;++block)
        {
            const int x=block*8;
            const int count=std::min(8,f.width-x);
            if (count < 8) return false;
            decodeR12BlockToPlanes(row+size_t(block)*36,true,G,B,R,size_t(y)*f.width+x);
        }
    }
    return true;
}

// The 8-bit BGRA frame is not ordinary RGB for this mode. It is a packed transport
// carrying 12-bit Y and 12-bit Cb/Cr: B low nibble = low 4 bits, G = high 8 bits
// of Y; B high nibble = low 4 bits and R = high 8 bits of Cb/Cr.
// No RGB->YCbCr matrix conversion is performed here. The resulting samples are
// written as planar yuv422p12le for FFV1.
bool FrameRecorder::DecodePackedRGB8TransportToYCbCr422P12(const RecordedFrame& f, std::vector<uint8_t>& out)
{
    if (f.width & 1) return false;
    const size_t yPlane=size_t(f.width)*f.height; const size_t cPlane=yPlane/2;
    out.resize((yPlane+cPlane+cPlane)*2);
    uint16_t* Y=(uint16_t*)out.data(); uint16_t* Cb=Y+yPlane; uint16_t* Cr=Cb+cPlane;
    for(int y=0;y<f.height;++y){ const uint8_t* s=f.bytes.data()+size_t(y)*f.rowBytes; for(int x=0;x<f.width;x+=2){
        const uint8_t* p0=s+size_t(x)*4; const uint8_t* p1=p0+4; size_t yo=size_t(y)*f.width+x; size_t co=size_t(y)*(f.width/2)+x/2;
        Y[yo]=uint16_t((uint16_t(p0[1])<<4)|(p0[0]&0x0F));
        Cb[co]=uint16_t((uint16_t(p0[2])<<4)|(p0[0]>>4));
        Y[yo+1]=uint16_t((uint16_t(p1[1])<<4)|(p1[0]&0x0F));
        Cr[co]=uint16_t((uint16_t(p1[2])<<4)|(p1[0]>>4));
    }}
    return true;
}
