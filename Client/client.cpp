// =============================
//      client.cpp 
//      Date : 2025-08-25 (Refactored)
//      Author : Dev.seunhak
// =============================
#include "../core/core.h"
#include <csignal>
#include <mmreg.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BITS_PER_SAMPLE 16

// ───────────────────────────────
// 글로벌 상태 변수
// ───────────────────────────────
static std::atomic<bool> gRunning{ true };                  // 전체 실행 여부
static SOCKET gSock = INVALID_SOCKET;                   // 서버와의 TCP 소켓
static HWAVEIN gWaveIn = nullptr;                             // 캡처 장치의 핸들러
static HWAVEOUT gWaveOut = nullptr;                       // 재생 장치의 핸들러

// ───────────────────────────────
// 송신 큐 (캡처 → 네트워크 송신 파이프라인)
// ───────────────────────────────
static std::mutex gSendMutex;
static std::condition_variable gSendCV;
static std::queue<std::shared_ptr<std::vector<char>>> gSendQueue;
static std::atomic<size_t> gSendQueueFrames{ 0 };
        //static std::queue<std::vector<char>> gSendQueue;
        // 백프레셔 카운터
        //static size_t gSendQueuedFrames = 0;

// ───────────────────────────────
// 재생 큐 (수신 → 오디오 출력 파이프라인)
//   - RecvThread 는 네트워크에서 받은 프레임을 빠르게 push
//   - 별도의 PlaybackThread 가 waveOutWrite 를 수행
//   - waveOutWrite 가 내부적으로 대기해도 RecvThread 가 막히지 않음
// ───────────────────────────────
static std::mutex gPlayMutex;
static std::condition_variable gPlayCV;
static std::queue<std::shared_ptr<std::vector<char>>> gPlayQueue;
static std::atomic<size_t> gPlayQueuedFrames{ 0 };
            //static std::queue<std::vector<char>> gPlayQueue;
            //static size_t gPlayQueuedFrames = 0; // 백프레셔 카운트

// ───────────────────────────────
// 시그널 처리
// ───────────────────────────────
static void SignalHandler(int) { gRunning = false; }

// ───────────────────────────────
// 소켓 옵션
// ───────────────────────────────
static void TuneSocket(SOCKET s)
{
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
    int sz = 32 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
}

bool InitCapture() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = CHANNELS;
    wf.nSamplesPerSec = SAMPLE_RATE;
    wf.wBitsPerSample = BITS_PER_SAMPLE;
    wf.nBlockAlign = (wf.nChannels * wf.wBitsPerSample) / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    if (waveInOpen(&gWaveIn, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return false;

    waveInStart(gWaveIn);
    return true;
}
void CaptureAudio(char* buffer, size_t size) {
    if (!gWaveIn) InitCapture();
    static WAVEHDR header{};
    header.lpData = buffer;
    header.dwBufferLength = (DWORD)size;
    header.dwFlags = 0;
    header.dwLoops = 0;
    waveInPrepareHeader(gWaveIn, &header, sizeof(header));
    waveInAddBuffer(gWaveIn, &header, sizeof(header));

    // 블로킹 대기 (간단 구현)
    while (!(header.dwFlags & WHDR_DONE)) Sleep(1);

    waveInUnprepareHeader(gWaveIn, &header, sizeof(header));
}

bool InitPlayback() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = CHANNELS;
    wf.nSamplesPerSec = SAMPLE_RATE;
    wf.wBitsPerSample = BITS_PER_SAMPLE;
    wf.nBlockAlign = (wf.nChannels * wf.wBitsPerSample) / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

    if (waveOutOpen(&gWaveOut, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return false;

    return true;
}

// ───────────────────────────────
// PlayAudio (멀티버퍼 + 백그라운드 재생)
// ───────────────────────────────
void PlayAudio(std::shared_ptr<std::vector<char>> frame)
{
    if (!gWaveOut) InitPlayback();

    WAVEHDR* hdr = new WAVEHDR();
    ZeroMemory(hdr, sizeof(WAVEHDR));
    hdr->lpData = (LPSTR)frame->data();
    hdr->dwBufferLength = (DWORD)frame->size();
    hdr->dwFlags = 0;
    hdr->dwLoops = 0;

    waveOutPrepareHeader(gWaveOut, hdr, sizeof(WAVEHDR));
    waveOutWrite(gWaveOut, hdr, sizeof(WAVEHDR));

    // 재생 완료 후 메모리 해제
    std::thread([hdr]()
        {
            while (!(hdr->dwFlags & WHDR_DONE)) Sleep(2);
            waveOutUnprepareHeader(gWaveOut, hdr, sizeof(WAVEHDR));
            delete hdr;
        }).detach();
}

// ───────────────────────────────
// 캡처 스레드 -> SendQueue
// ───────────────────────────────
void CaptureThread()
{
    while (gRunning)
    {
        std::vector<char> frame(AUDIO_BUFFER_SIZE);
        CaptureAudio(frame.data(), frame.size());       // 사용자 캡처 함수

        auto packet = std::make_shared<std::vector<char>>(std::move(frame));
        {
            std::lock_guard<std::mutex> lock(gSendMutex);
            while (gSendQueueFrames >= MAX_QUEUE_FRAMES && !gSendQueue.empty())
            {
                gSendQueue.pop();
                gSendQueueFrames--;
            }
            gSendQueue.push(packet);
            gSendQueueFrames++;
        }
        gSendCV.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ───────────────────────────────
// SendThread
// ───────────────────────────────
void SendThread()
{
    while (gRunning)
    {
        std::shared_ptr<std::vector<char>> packet;
        {
            std::unique_lock<std::mutex > lock(gSendMutex);
            gSendCV.wait(lock, [] { return !gSendQueue.empty() || !gRunning; });
            if (!gRunning)
                break;

            // 최신 프레임만 전송
            while (gSendQueue.size() > 1)
            {
                gSendQueue.pop();
                gSendQueueFrames--;
            }
            packet = gSendQueue.front();
            gSendQueue.pop();
            gSendQueueFrames--;
        }

        if (!sendFrame(gSock, packet->data(), (uint32_t)packet->size()))
        {
            std::cerr << "[클라이언트] 송신 실패" << std::endl;
            gRunning = false;
            break;
        }
    }
}

// ───────────────────────────────
// RecvThread
// ───────────────────────────────
void RecvThread()
{
    while (gRunning)
    {
        std::vector<char> buf(AUDIO_BUFFER_SIZE);
        if (!recvFrame(gSock, buf)) { gRunning = false; break; }

        auto packet = std::make_shared<std::vector<char>>(std::move(buf));
        {
            std::lock_guard<std::mutex> lock(gPlayMutex);
            while (gPlayQueuedFrames >= MAX_QUEUE_FRAMES && !gPlayQueue.empty())
            {
                gPlayQueue.pop();
                gPlayQueuedFrames--;
            }
            gPlayQueue.push(packet);
            gPlayQueuedFrames++;
        }
        gPlayCV.notify_one();
    }
 }

// ───────────────────────────────
// PlaybackThread
// ───────────────────────────────
void PlaybackThread()
{
    while (gRunning)
    {
        std::shared_ptr<std::vector<char>> frame;
        {
            std::unique_lock<std::mutex> lock(gPlayMutex);
            gPlayCV.wait(lock, [] { return !gPlayQueue.empty() || !gRunning; });
            if (!gRunning) break;

            // 최신 프레임만 재생
            while (gPlayQueue.size() > 1)
            {
                gPlayQueue.pop();
                gPlayQueuedFrames--;
            }

            frame = gPlayQueue.front();
            gPlayQueue.pop();
            gPlayQueuedFrames--;
        }

        PlayAudio(frame); // 재생
    }
}

// ───────────────────────────────
// main
// ───────────────────────────────
int main()
{
    std::cout << "// ───────────────────────────────" << std::endl;
    std::cout << "// 비압축 Wave 형식의 오디오 송수신 프로그램 [ 클라이언트 ]" << std::endl;
    std::cout << "//    * 형식 *PCM, 2ch, 48000kHz, 16bit" << std::endl;
    std::cout << "//    * 현재서버 주소" << std::endl << "//        [" << SERVER_IP << "]" << std::endl;
    std::cout << "//    * Author" << std::endl << "//        [Dev.Shhyun@gmail.com]" << std::endl;
    std::cout << "//    * Date" << std::endl << "//        [2025-08-25]" << std::endl;
    std::cout << "// ───────────────────────────────" << std::endl << std::endl;

    std::signal(SIGINT, SignalHandler);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    gSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(gSock, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        std::cerr << "[클라이언트] 서버 연결 실패\n";
        return -1;
    }

    std::cout << "[system] 서버 연결 성공" << std::endl;

    TuneSocket(gSock);

    std::thread tCapture(CaptureThread);
    std::thread tSend(SendThread);
    std::thread tRecv(RecvThread);
    std::thread tPlay(PlaybackThread);

    // 5. 안전 종료 ( 엔터 입력 대기 )
        std::cout << "[system] 음성 채팅 클라이언트 실행 중. 엔터 입력 시 종료" << std::endl;
        std::string dummy;
        std::getline(std::cin, dummy);

    tCapture.join();
    tSend.join();
    tRecv.join();
    tPlay.join();

    closesocket(gSock);
    WSACleanup();
}

//// ───────────────────────────────
//// 클라이언트 동작 모드
////   - Normal : 실제 오디오 입출력 사용
////   - Test   : 무음 송신 + 무출력 (다중 실행 시 장치 충돌 방지)
//// ───────────────────────────────
//enum class ClientMode { 
//    Normal, 
//    Test 
//};
//static ClientMode gMode = ClientMode::Normal;
//
//// ───────────────────────────────
//// 상수 설정 (백프레셔)
////  - 20ms 프레임 기준 50개이면 약 1초 분량
//// ───────────────────────────────
//#define MAX_SEND_QUEUE_FRAMES  50
//#define MAX_PLAY_QUEUE_FRAMES  50
//
//// ───────────────────────────────
//// [함수] RegisterBuffer
//// 캡처 장치에 WAVEHDR 버퍼를 등록한다.
//// 1. 헤더 준비 : waveInPrepareHeader: 헤더 준비 (드라이버가 접근 가능하게)
//// 2. 버퍼 등록 : waveInAddBuffer: 캡처 시 실제 데이터가 채워질 공간 등록
//// 3. 관리 리스트에 추가 :gAllocatedBufs -> pushback
//// ───────────────────────────────
//static void RegisterBuffer(HWAVEIN hIn, WAVEHDR* hdr)
//{
//    // 1. 헤더 준비 (드라이버가 접근 가능하게)
//    {
//        MMRESULT r1 = waveInPrepareHeader(hIn, hdr, sizeof(WAVEHDR));
//        if (r1 != MMSYSERR_NOERROR)
//        {
//            std::cerr << "waveInPrepareHeader 실패: " << r1 << std::endl;
//            return;
//        }
//    }
//
//    // 2. 버퍼 등록 (캡처 시 데이터가 채워질 공간)
//    {
//        MMRESULT r2 = waveInAddBuffer(hIn, hdr, sizeof(WAVEHDR));
//        if (r2 != MMSYSERR_NOERROR)
//        {
//            std::cerr << "waveInAddBuffer 실패: " << r2 << std::endl;
//            return;
//        }
//    }
//
//    // 3. 관리 리스트에 추가 (StopCapture 시 안전하게 해제하려면)
//    {
//        std::lock_guard<std::mutex> lock(gBufMutex);
//        gAllocatedBufs.push_back(hdr);
//    }
//}
//
//// ───────────────────────────────
//// [함수] StopCapture
//// 캡처 장치를 안전하게 종료한다.
//// 1. 캡처 중단 및 초기화 : waveInStop / waveInReset: 입력 중단 및 버퍼 반환
//// 2. 등록된 버퍼 해제 : 등록된 모든 버퍼(WAVEHDR) 해제
//// 3. 장치 닫기 : waveInClose,  장치 핸들 닫기
//// ───────────────────────────────
//static void StopCapture(HWAVEIN hIn)
//{
//    if (!hIn)
//        return;
//
//    // 1. 캡처 중단 및 초기화
//    {
//        waveInStop(hIn);
//        waveInReset(hIn);
//    }
//
//    // 2. 중요 :: 등록된 버퍼 해제 
//    {
//        std::lock_guard<std::mutex> lock(gBufMutex);
//        for (auto hdr : gAllocatedBufs) {
//            waveInUnprepareHeader(hIn, hdr, sizeof(WAVEHDR));
//            delete[] hdr->lpData;
//            delete hdr;
//        }
//        gAllocatedBufs.clear();
//    }
//
//    // 3. 장치 닫기
//    waveInClose(hIn);
//    std::cout << "캡처 장치 안전 종료 완료" << std::endl;
//}
//
//// ───────────────────────────────
//// [콜백] waveInProc
//// 오디오 입력 콜백 (마이크에서 일정량 캡처 완료 시 호출)
//// 1. 메시지 체크 : param1 → 캡처된 데이터가 담긴 WAVEHDR 포인터
//// 2. TCP 송신 큐에 패킷 복사 :  dwBytesRecorded 크기만큼 TCP 송신 큐에 push
//// 3. 버퍼 재등록 : waveInAddBuffer로 재등록 (순환 버퍼)
//// ───────────────────────────────
//static void CALLBACK waveInProc(HWAVEIN hwi, UINT msg, DWORD_PTR inst, DWORD_PTR param1, DWORD_PTR /*param2*/)
//{
//    // 1. WIM_DATA 메시지 및 실행 상태 확인
//    if (msg != WIM_DATA || !gRunning || gMode == ClientMode::Test)
//        return;
//
//    // 2.  TCP 송신 큐에 복사
//    {
//        WAVEHDR* hdr = (WAVEHDR*)param1;
//        if (hdr->dwBytesRecorded > 0)
//        {
//            // 리펙터링으로 블록 std::vector<char> packet(hdr->lpData, hdr->lpData + hdr->dwBytesRecorded);
//            /*gSendQueue.push(std::move(packet));
//            gSendCV.notify_one();*/
//            std::lock_guard<std::mutex> lock(gSendMutex);
//
//            // 리펙터링으로 추가
//            // 백 프레셔 :: 큐가 가득 차면 가장 오래된 프레임 drop
//            while (gSendQueuedFrames >= MAX_SEND_QUEUE_FRAMES && !gSendQueue.empty())
//            {
//                gSendQueue.pop();
//                gSendQueuedFrames--;               
//            }
//
//            // 리펙터링 추가
//            gSendQueue.emplace(hdr->lpData, hdr->lpData + hdr->dwBytesRecorded);
//            gSendQueuedFrames++;
//            gSendCV.notify_one();
//        }
//
//        // 3. 버퍼를 캡처 장치에 재등록 (순환 구조)
//        waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
//    }
//}
//
//// ───────────────────────────────
//// [스레드] SendThread
//// 큐에서 꺼낸 오디오 패킷을 서버로 전송한다.
//// 1. 큐에서 패킷 대기 : - sendFrame: TCP 길이 프리픽스
//// 2. 서버로 전송 : sendFrame 전송
//// 3. 실패 시 종료 : gRunning = false → 전체 종료
//// ───────────────────────────────
//static void SendThread()
//{
//    // ─────────────────
//    // Test 모드 : 무음 패킷 주기적으로 송신
//    // ─────────────────
//    if (gMode == ClientMode::Test)
//    {
//        std::vector<char> silence(AUDIO_BUFFER_SIZE, 0);
//        while (gRunning)
//        {
//            if (!sendFrame(gSock, silence.data(), (uint32_t)silence.size()))
//            {
//                std::cerr << "서버 송신 실패 (test 모드)" << std::endl;
//                gRunning = false;
//                break;
//            }
//            std::this_thread::sleep_for(std::chrono::milliseconds(20));
//        }
//        return;
//    }
//
//    // ─────────────────
//    // Normalt 모드 : 큐에서 패킷을 꺼내 전송
//    // ─────────────────
//    while (gRunning)
//    {
//        // 1. 큐에서 패킷 대기
//        std::unique_lock<std::mutex> lock(gSendMutex);
//        gSendCV.wait(lock, [] { return !gSendQueue.empty() || !gRunning; });
//        if (!gRunning)
//            break;
//
//        // 2. 패킷을 꺼낸다
//        auto packet = std::move(gSendQueue.front());
//        gSendQueue.pop();
//        
//        // 리펙터링으로 추가
//        // 백프레셔 카운트 감소 
//        if (gSendQueuedFrames > 0)
//            gSendQueuedFrames--;
//
//        lock.unlock();
//
//        // 3. 서버로 전송한다
//        if (!sendFrame(gSock, packet.data(), (uint32_t)packet.size()))
//        {
//            std::cerr << "서버 송신 실패" << std::endl;
//            gRunning = false;
//            break;
//        }
//    }
//}
//
//// ───────────────────────────────
//// [스레드] RecvThread
//// 서버로부터 오디오 프레임을 수신하고    재생한다.
//// 1. 서버에서 프레임 수신 : recvFrame: TCP에서 [길이][데이터] 형태로 안전 수신
//// 2. 재생용 버퍼 준비 : 수신 후 동적 버퍼에 복사하여 waveOutWrite 호출
//// 3. 재생 및 재생 후 안전 해제 : 재생이 끝나면 스레드에서 polling 하여 메모리 해제
//// ───────────────────────────────
//static void RecvThread()
//{
//    while (gRunning)
//    {
//        // 1. 서버에서 프레임 수신
//        std::vector<char> buf;
//        if (!recvFrame(gSock, buf))
//        {
//            std::cerr << "서버 연결 끊김" << std::endl;
//            gRunning = false;
//            break;
//        }
//
//        // test 모드인 경우 재생하지 않고 discard 
//        if (gMode == ClientMode::Test)
//            continue;
//        
//        // ───────────────────────────────
//        // 중요 :: Recv → Play 를 분리
//        //  - 재생 큐에 push 후 PlaybackThread 가 waveOutWrite 수행
//        //  - 백프레셔 : 가득 차면 가장 오래된 프레임 drop
//        // ───────────────────────────────
//        {
//            std::lock_guard<std::mutex> lock(gPlayMutex);
//
//            while (gPlayQueuedFrames >= MAX_PLAY_QUEUE_FRAMES && !gPlayQueue.empty())
//            {
//                gPlayQueue.pop();
//                gPlayQueuedFrames--;
//            }
//
//            gPlayQueue.emplace(std::move(buf));
//            gPlayQueuedFrames++;
//            gPlayCV.notify_one();
//        }
//    }
//}
//
//// Refactoring 으로 추가 (recv → play 분리)
//// ───────────────────────────────
//// [스레드] PlaybackThread
////  - 재생 큐에서 프레임을 꺼내 waveOutWrite 수행
////  - waveOutWrite 가 내부적으로 대기해도 RecvThread 는 영향을 받지 않는다
//// ───────────────────────────────
//static void PlaybackThread()
//{
//    if (gMode == ClientMode::Test)
//        return;             // Test 모드인 경우 재생하지 않음
//
//    while (gRunning)
//    {
//        std::vector<char> buf;
//
//        // 1. 큐에서 프레임 대기
//        {
//            std::unique_lock<std::mutex> lock(gPlayMutex);
//            gPlayCV.wait(lock, [] { return !gPlayQueue.empty() || !gRunning; });
//            if (!gRunning)
//                break;
//            
//            // 오래된 프레임은 모두 제거, 마지막 프레임만 재생
//            while (gPlayQueue.size() > 1)
//            {
//                gPlayQueue.pop();
//                gPlayQueuedFrames--;
//            }
//
//            buf = std::move(gPlayQueue.front());
//            gPlayQueue.pop();
//            if (gPlayQueuedFrames > 0)
//                gPlayQueuedFrames--;
//        }
//
//        // 2. 재생용 동적 버퍼를 준비한다
//        WAVEHDR* hdr = new WAVEHDR();
//        ZeroMemory(hdr, sizeof(WAVEHDR));
//        hdr->lpData = new char[buf.size()];
//        memcpy(hdr->lpData, buf.data(), buf.size());
//        hdr->dwBufferLength = (DWORD)buf.size();
//
//        waveOutPrepareHeader(gWaveOut, hdr, sizeof(WAVEHDR));
//        waveOutWrite(gWaveOut, hdr, sizeof(WAVEHDR));
//
//        // 3. 중요 : 힙 제거 안전 코드 : 재생이 끝날 때까지 대기 후 메모리 해제
//        std::thread([hdr]
//            {
//                while (!(hdr->dwFlags & WHDR_DONE)) Sleep(2);
//                waveOutUnprepareHeader(gWaveOut, hdr, sizeof(WAVEHDR));
//                delete[] hdr->lpData;
//                delete hdr;
//            }).detach();
//    }
//}
//
//// ───────────────────────────────
//// [함수] StartCapture
//// 마이크 캡처 장치를 초기화하고 버퍼 등록 후 시작
//// 1. WAVEFORMATEX 설정
//// 2. 캡처 장치 열기
//// 3. 초기 버퍼 8개 등록
//// 4. 캡처 시작
//// ───────────────────────────────
//static bool StartCapture()
//{
//    if(gMode == ClientMode::Test)
//		return true;
//
//    // 1. WAVEFORMATEX 설정 (PCM, 48kHz, 16bit, 스테리오 타입)
//    WAVEFORMATEX fmt{};
//    {
//        fmt.wFormatTag = WAVE_FORMAT_PCM;
//        fmt.nChannels = 2;
//        fmt.nSamplesPerSec = 48000;
//        fmt.wBitsPerSample = 16;
//        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
//        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
//    }
//
//    // 2. 캡처 장치 열기
//    {
//        MMRESULT r = waveInOpen(&gWaveIn, WAVE_MAPPER, &fmt, (DWORD_PTR)waveInProc, 0, CALLBACK_FUNCTION);
//        if (r != MMSYSERR_NOERROR) 
//        {
//            std::cerr << "waveInOpen 실패" << std::endl;
//            return false;
//        }
//    }
//
//    // 3. 초기 버퍼 8개 등록 (순환 버퍼)
//    for (int i = 0; i < 8; i++)
//    {
//        WAVEHDR* hdr = new WAVEHDR();
//        ZeroMemory(hdr, sizeof(WAVEHDR));
//        hdr->lpData = new char[AUDIO_BUFFER_SIZE];
//        hdr->dwBufferLength = AUDIO_BUFFER_SIZE;
//
//        // 캡처 장치에 등록하고 관리 리스트에 추가
//        RegisterBuffer(gWaveIn, hdr);
//    }
//
//    waveInStart(gWaveIn);
//    return true;
//}
//
//// ───────────────────────────────
//// [함수] InitPlayback
//// 재생 장치 초기화
//// 1. WAVEFORMATEX 설정
//// 2. 장치 열기
//// ───────────────────────────────
//static bool InitPlayback()
//{
//    if (gMode == ClientMode::Test)
//        return true;    // 장치 열지 않음
//
//    // 1. WAVEFORMATEX 설정 (PCM, 48kHz, 16bit, 스테레오)
//    WAVEFORMATEX fmt{};
//    {
//        fmt.wFormatTag = WAVE_FORMAT_PCM;
//        fmt.nChannels = 2;
//        fmt.nSamplesPerSec = 48000;
//        fmt.wBitsPerSample = 16;
//        fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
//        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
//    }
//
//    // 2. 장치 열기
//    {
//        MMRESULT r = waveOutOpen(&gWaveOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
//        if (r != MMSYSERR_NOERROR)
//        {
//            std::cerr << "waveOutOpen 실패" << std::endl;
//            return false;
//        }
//    }
//
//    return true;
//}
//
//// ───────────────────────────────
//// [메인 함수]
//// 1. Winsock 초기화
//// 2. 서버 접속
//// 3. 재생 장치 / 캡처 장치 초기화
//// 4. 송신/수신 스레드 실행
//// 5. 엔터 입력 → 안전 종료
//// ───────────────────────────────
//int main(int argc, char* argv[])
//{
//    std::cout << "// ───────────────────────────────" << std::endl;
//    std::cout << "// 비압축 Wave 형식의 오디오 송수신 프로그램 [ 클라이언트 ]" << std::endl;
//    std::cout << "//    * 형식 *PCM, 2ch, 48000kHz, 16bit" << std::endl;
//    std::cout << "//    * 현재서버 주소" << std::endl << "//        [" << SERVER_IP << "]" << std::endl;
//    std::cout << "//    * Author" << std::endl << "//        [Dev.Shhyun@gmail.com]" << std::endl;
//    std::cout << "//    * Date" << std::endl << "//        [2025-08-25]" << std::endl;
//    std::cout << "// ───────────────────────────────" << std::endl << std::endl;
//
//    // 실행 인자 확인 → test 모드
//    if (argc > 1 && std::string(argv[1]) == "test")
//    {
//        gMode = ClientMode::Test;
//        std::cout << "[system] Test 모드 활성화 : 무음 송신 / 무출력" << std::endl;
//    }
//
//    // 1. Winsock 초기화
//    {
//        WSADATA wsa;
//        WSAStartup(MAKEWORD(2, 2), &wsa);
//    }
//
//    // 2. 서버 접속
//    {
//        gSock = socket(AF_INET, SOCK_STREAM, 0);
//        sockaddr_in serv{};
//        serv.sin_family = AF_INET;
//        serv.sin_port = htons(PORT);
//        inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);
//
//
//        if (connect(gSock, (sockaddr*)&serv, sizeof(serv)) == SOCKET_ERROR)
//        {
//            std::cerr << "[system] 서버 연결 실패" << std::endl;
//            return -1;
//        }
//
//        // ───────────────────────────────
//        // 지연 최적화 (선택) : Nagle 비활성화
//        // ───────────────────────────────
//        int flag = 1;
//        setsockopt(gSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
//
//        std::cout << "[system] 서버 연결 성공" << std::endl;
//    }
//
//    // 3. 재생/캡처 장치 초기화
//    {
//        if (!InitPlayback())
//            return -1;
//        if (!StartCapture())
//            return -1;
//    }
//
//    // 4. 송수신 스레드 실행
//    {
//        std::thread tSend(SendThread);
//        std::thread tRecv(RecvThread);
//
//        // 리펙터링으로 추가
//        std::thread tPlay;
//        if(gMode != ClientMode::Test)
//            tPlay = std::thread(PlaybackThread);
//
//        // 5. 안전 종료 ( 엔터 입력 대기 )
//        std::cout << "[system] 음성 채팅 클라이언트 실행 중. 엔터 입력 시 종료" << std::endl;
//        std::string dummy;
//        std::getline(std::cin, dummy);
//
//        gRunning = false;
//        gSendCV.notify_all();
//        // 리펙터링으로 추가
//        gPlayCV.notify_all();
//
//        StopCapture(gWaveIn);
//        if (gWaveOut)
//            waveOutClose(gWaveOut);
//
//        closesocket(gSock);
//
//        tSend.join();
//        tRecv.join();
//
//        // 리펙터링으로 추가
//        if (tPlay.joinable())
//            tPlay.join();
//
//        // 리펙터링으로 추가
//        // 큐 비우기 (종료 시 청소)
//        {
//            std::lock_guard<std::mutex> lock1(gSendMutex);
//            while (!gSendQueue.empty()) gSendQueue.pop();
//            gSendQueuedFrames = 0;
//        }
//        {
//            std::lock_guard<std::mutex> lock2(gPlayMutex);
//            while (!gPlayQueue.empty()) gPlayQueue.pop();
//            gPlayQueuedFrames = 0;
//        }
//    }
//
//    WSACleanup();
//    return 0;
//}