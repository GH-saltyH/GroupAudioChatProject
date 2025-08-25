// =============================
//      server.cpp 
//      Date : 2025-08-25 (Refactored)
//      Author : Dev.seunhak
// =============================
#include "../core/core.h"
#include <atomic>
#include <csignal>
#include <memory>
#include <algorithm>

// -------------------------------------------
// 전역 상태
// -------------------------------------------

// 서버 실행 상태 (Ctrl+C 등으로 false 가 되면 종료 된다)
static std::atomic<bool> gRunning{ true };

// -------------------------------------------
// 클라이언트 엔트리
//  1. 각 클라이언트 별 송신 전용 큐 / 스레드를 보유
//  2. 느린 클리이언트가 있어도 다른 클라이언트로의 송신은 지연되지 않는다
// -------------------------------------------
struct ClientInfo
{
    SOCKET sock = INVALID_SOCKET;
    // 송신 전용 큐
    std::mutex qMutex;
    std::condition_variable qCV;
    // 공유 포인터로 패킷을 보관하여 불필요한 복사를 줄인다
    std::queue<std::shared_ptr<std::vector<char>>> q;
    // 송신 스레드
    std::thread sendThread;
    // 활성 상태
    std::atomic<bool> active{ true };
    // 백프레셔 카운터 (무한 메모리 증가 방지용) - 단순 프레임 수 제한
    size_t queuedFrames = 0;
};

static std::vector<std::shared_ptr<ClientInfo>> gClients;
// -------------------------------------------
// 멀티스레드에서 동시에 gClients 벡터를 접근할 수 있으므로
// mutex로 보호한다
// -------------------------------------------
static std::mutex gClientMutex;

// ---------------------------
// 믹싱 큐
// ---------------------------
struct MixFrame
{
    std::vector<char> data;                 // 16bit stereo PCM
};
static std::mutex gMixMutex;
static std::vector<MixFrame> gMixFrames;

// -------------------------------------------
// 소켓 옵션 보조 함수
//  1. Nagle 비활성화 (지연 최소화)
//  2. 송수신 버퍼 크기 조정 (선택)
// -------------------------------------------
static void TuneSocket(SOCKET s)
{
    // 1. Nagle 비활성화 (지연 최소화)
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));

    // 2. 송수신 버퍼 약간 축소하여 지연 완화 (환경에 맞게 조정해야 함)
    int sz = 32 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
}

// -------------------------------------------
// RemoveClient
//  1. gClients 컨테이너에서 제거하고 소켓 정리
//  2. sendThread 가 깔끔히 종료되도록 active=false + notify
// -------------------------------------------
static void RemoveClient(const std::shared_ptr<ClientInfo>& cli)
{
    // 이미 종료된 경우 중복 방지
    if (!cli || !cli->active.exchange(false))
        return;
    
    // 1. 활성 플래그 내리고 대기 깨우기
    //cli->active = false;
    {
        std::lock_guard<std::mutex> lock(cli->qMutex);
        while (!cli->q.empty()) cli->q.pop();
        cli->queuedFrames = 0;
    }
    cli->qCV.notify_all();

    // 2. 소켓 정리
    if (cli->sock != INVALID_SOCKET)
    {
        shutdown(cli->sock, SD_BOTH);
        closesocket(cli->sock);
        cli->sock = INVALID_SOCKET;
    }

    // 3. sendThread 조인
    if (cli->sendThread.joinable())
        cli->sendThread.join();

    // 4. gClients 에서 제거
    {
        std::lock_guard<std::mutex> glock(gClientMutex);

        gClients.erase(std::remove(gClients.begin(), gClients.end(), cli), gClients.end());

        /*auto it = std::find(gClients.begin(), gClients.end(), cli);
        if (it != gClients.end())
            gClients.erase(it);*/
    }

    std::cout << "[서버] 클라이언트 제거 완료 (잔여 " << gClients.size() << "명)" << std::endl;
}

// -------------------------------------------
// ClientSendThread
//  1. 클라이언트별로 독립된 송신 루프
//  2. 큐에서 패킷을 꺼내 길이-프리ㅂ픽스로 안전한 송신
//  3. 실패 시 클라이언트 제거
// -------------------------------------------
static void ClientSendThread(std::shared_ptr<ClientInfo> cli)
{
    while (cli->active)
    {
        std::shared_ptr<std::vector<char>> packet;

        // 1. 큐에서 패킷 대기
        {
            std::unique_lock<std::mutex> lock(cli->qMutex);
            cli->qCV.wait(lock, [&] { return !cli->q.empty() || !cli->active; });
            if (!cli->active)
                break;

            packet = cli->q.front();
            cli->q.pop();
            if (cli->queuedFrames > 0)
                cli->queuedFrames--;
        }

        // 2. 안전 패킷 송신
        if (!sendFrame(cli->sock, packet->data(), (uint32_t)packet->size()))
        {
            std::cerr << "[서버] 클라이언트 송신 실패" << std::endl;
            cli->active = false;
            break;
        }
    }

    // 루프 탈출 시 클라이언트 제거 --> 수정 -> RecvThread 에서만 최종적으로 호출
    //RemoveClient(cli);
}

// -------------------------------------------
// ClientRecvThread
//  1. 클라이언트가 보낸 오디오 프레임을 수신
//  2. BroadcastAudio 로 전체에게 송신
// -------------------------------------------
static void ClientRecvThread(std::shared_ptr<ClientInfo> cli)
{
    std::vector<char> frame;
    while (gRunning && cli->active)
    {
        if (!recvFrame(cli->sock, frame))
        {
            std::cout << "[서버] 클라이언트 연결 종료" << std::endl;
            break;
        }

        // 믹스 프레임 수신
        MixFrame mf;
        mf.data = frame;
        {
            std::lock_guard<std::mutex> lock(gMixMutex);
            gMixFrames.push_back(std::move(mf));
        }
        
        //// 수신 프레임을 전체에게 브로드 캐스트
        //BroadcastAudio(cli->sock, frame.data(), (int)frame.size());
    }

    // 수신 종료 시 제거
    RemoveClient(cli);
}

// -------------------------------------------
// MixerThread
//  1. 클라이언트가 보낸 오디오를 믹싱
// -------------------------------------------
static void MixerThread()
{
    const int FRAME_SIZE = AUDIO_BUFFER_SIZE;   // 20ms PCM
    const int NUM_CHANNELS = 2;

    while (gRunning)
    {
        std::vector<MixFrame> framesToMix;

        {
            std::lock_guard<std::mutex> lock(gMixMutex);
            if (gMixFrames.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            framesToMix.swap(gMixFrames);
        }

        // mix
        std::vector<char> mixed(FRAME_SIZE, 0);
        for (auto& f : framesToMix)
        {
            for (size_t i = 0; i < FRAME_SIZE; i += 2)
            {
                short* dst = (short*)&mixed[i];
                short* src = (short*)&f.data[i];
                int s = *dst + *src;

                if (s > 32767)
                    s = 32767;

                if (s < -32768)
                    s = -32768;

                *dst = (short)s;
            }
        }

        // 모든 클라이언트에 push
        std::lock_guard<std::mutex> glock(gClientMutex);
        for (auto& cli : gClients)
        {
            if (!cli->active)
                continue;

            std::lock_guard<std::mutex> lock(cli->qMutex);
            while (cli->queuedFrames >= MAX_QUEUE_FRAMES && !cli->q.empty())
            {
                cli->q.pop();
                cli->queuedFrames--;
            }

            cli->q.push(std::make_shared<std::vector<char>>(mixed));
            cli->queuedFrames++;
            cli->qCV.notify_one();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// -------------------------------------------
// BroadcastAudio
//  1. 하나의 프레임을 모든 클라이언트의 송신 큐에 push
//  2. 느린 클라이언트는 큐가 가득 차면 가장 오래된 프레임을 drop
//     (실시간 음성 특성상 오래된 프레임을 보낼 이유가 없다)
// -------------------------------------------
//static void BroadcastAudio(SOCKET sender, const char* buf, int len)
//{
//    // 1. 패킷 공유 포인터 (복사 최소화)
//    auto packet = std::make_shared<std::vector<char>>(buf, buf + len);
//
//    // 2. 모든 클라이언트 큐에 push
//    std::lock_guard<std::mutex> glock(gClientMutex);
//    for (auto& cli : gClients)
//    {
//        if (!cli->active)
//			continue;
//
//        std::lock_guard<std::mutex> lock(cli->qMutex);
//
//        // 백프레셔 : 큐가 가득 차면 가장 오래된 프레임 drop
//        while (cli->queuedFrames >= MAX_QUEUE_FRAMES && !cli->q.empty())
//        {
//            cli->q.pop();
//            cli->queuedFrames--;
//        }
//
//        cli->q.push(packet);
//        cli->queuedFrames++;
//        cli->qCV.notify_one();
//	}
//}

// -------------------------------------------
// SignalHandler
//  - Ctrl+C(SIGINT) 발생 시 gRunning = false 로 설정하여
//    메인 루프가 빠져나오게 한다.
// -------------------------------------------
static void SignalHandler(int)
{
    gRunning = false;
    std::cerr << "\n[서버] 종료 시그널 수신, 서버 종료 중..." << std::endl;
}

// -------------------------------------------
// main()
//  1. Winsock 초기화
//  2. 소켓 생성, 바인드(bind), 리슨(listen)
//  3. 메인 루프에서 accept 로 클라이언트 접속 대기
//  3. 클라이언트가 접속하면 ClientThread 실행
//  4. 종료 시 모든 소켓 정리
// -------------------------------------------
int main()
{
    std::cout << "// ───────────────────────────────" << std::endl;
    std::cout << "// 비압축 Wave 형식의 오디오 송수신 프로그램 [ 서버 ]" << std::endl;
    std::cout << "//    * 형식 *PCM, 2ch, 48000kHz, 16bit" << std::endl;
    std::cout << "//    * 현재서버 주소" << std::endl << "//        [" << SERVER_IP << "]" << std::endl;
    std::cout << "//    * Author" << std::endl << "//        [Dev.Shhyun@gmail.com]" << std::endl;
    std::cout << "//    * Date" << std::endl << "//        [2025-08-25]" << std::endl;
    std::cout << "// ───────────────────────────────" << std::endl << std::endl;

    // 1. Winsock 초기화
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "[서버] WSAStartup 실패" << std::endl;
        return 1;
    }

    // 2 Ctrl+C 처리 핸들러 등록
    std::signal(SIGINT, SignalHandler);

    // 3. 리슨 소켓 생성
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
    {
        std::cerr << "[서버] socket 실패" << std::endl;
        WSACleanup();
        return 1;
    }

    // (옵션) 빠른 재시작을 위한 SO_REUSEADDR = allow local address reuse 설정
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // 4. Bind : 서버 주소와 포트를 지정한다
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[서버] bind 실패: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
	}

    // 5. Listen 상태 진입 (동시 접속 허용 처리
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[서버] listen 실패: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    std::cout << "[오디오 서버] 포트" << PORT << " 수신 대기" << std::endl;

    // ** 믹서 스레드 등록
    std::thread mixer(MixerThread);

    // 6. 메인 루프 : 새로운 클라이언트 accept
    while (gRunning)
    {
        SOCKET s = accept(listenSock, nullptr, nullptr);
        if (s == INVALID_SOCKET)
        {
            if (!gRunning)
                break;
            std::cerr << "[서버] accept 실패: " << WSAGetLastError() << std::endl;
            continue;
        }

        // 소켓 튜닝 (지연 감소)
        TuneSocket(s);

        // ClientInfo 생성 및 등록
        auto cli = std::make_shared<ClientInfo>();
        cli->sock = s;
        {
            std::lock_guard<std::mutex> glock(gClientMutex);
            gClients.push_back(cli);
        }

        // 송신 스레드 시작
        cli->sendThread = std::thread(ClientSendThread, cli);

        // 수신 스레드는 detach (자체적으로 RemoveClient 처리)
        std::thread(ClientRecvThread, cli).detach();

        std::cout << "[서버] 클라이언트 접속 (총 " << (int)gClients.size() << " 명)" << std::endl;    
    }

    // 7. 종료 처리: 모든 클라이언트 소켓/스레드 닫기
  //  {
  //      std::vector<std::shared_ptr<ClientInfo>> snapshot;
  //      {
  //          std::lock_guard<std::mutex> glock(gClientMutex);
  //          snapshot = gClients;        // 복사하여 락을 짧게 유지
  //      }
  //      for(auto& cli : snapshot)
  //      {
  //          RemoveClient(cli);
		//}
  //  }

    mixer.join();
    closesocket(listenSock);
    WSACleanup();
    std::cout << "[서버] 정상 종료" << std::endl;
    return 0;
}