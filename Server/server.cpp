// =============================
//      server.cpp 
//      Date : 2025-08-23
//      Author : Dev.seunhak
// =============================
#include "../core/core.h"
#include <atomic>
#include <csignal>

// -------------------------------------------
// 전역 상태
// -------------------------------------------

// -------------------------------------------
// 멀티스레드에서 동시에 gClients 벡터를 접근할 수 있으므로
// mutex로 보호한다
// -------------------------------------------
static std::mutex gClientMutex;
static std::vector<SOCKET> gClients;

// 서버 실행 상태 (Ctrl+C 등으로 false 가 되면 종료 된다)
static std::atomic<bool> gRunning{ true };

// -------------------------------------------
// RemoveClient
// 특정 클라이언트 소켓을 벡터에서 제거하고 close한다
// -------------------------------------------
static void RemoveClient(SOCKET s)
{
    std::lock_guard<std::mutex> lock(gClientMutex);
    auto it = std::find(gClients.begin(), gClients.end(), s);
    if (it != gClients.end())
    {
        closesocket(*it);
        gClients.erase(it);
    }
}

// -------------------------------------------
// BroadcastAudio
//  - 한 클라이언트에서 수신한 오디오 프레임을
//    모든 클라이언트에게 전송한다.
//  - sendFrame은 (길이 + 실제 바이트 데이터) 형식으로 송신.
//    TCP는 스트림 기반이라 경계가 없으므로,
//    길이값을 앞에 붙여야 올바른 Frame을 복원 가능.
// -------------------------------------------
static void BroadcastAudio(SOCKET sender, const char* buf, int len)
{
    std::vector<SOCKET> bad;

    {
        std::lock_guard<std::mutex> lock(gClientMutex);
        for (SOCKET c : gClients)
        {
            // 송신 실패 시, 해당 소켓은 이후 제거 대상에 추가시킨다
            if (!sendFrame(c, buf, (uint32_t)len))
                bad.push_back(c);
        }
    }

    // 실패한 소켓을 실제 제거 시키는 처리를 한다
    for (SOCKET b : bad)
    {
        std::cerr << "[서버] 전송 실패 클라이언트 제거" << std::endl;
        RemoveClient(b);
    }
}

// -------------------------------------------
// ClientThread
//  - 클라이언트별 스레드
//  - recvFrame()은 (길이 + 데이터) 구조를 읽어
//    완전한 오디오 프레임 단위로 반환한다.
//  - 받은 프레임은 BroadcastAudio()를 통해 전체 클라이언트에게 송신.
// -------------------------------------------
static void ClientThread(SOCKET s)
{
    std::vector<char> frame;
    while (gRunning)
    {
        // recvFrame: TCP 스트림에서 
        // [4바이트 길이][데이터] 구조로된 데이터를 해석한다
        if (!recvFrame(s, frame))
            break;

        BroadcastAudio(s, frame.data(), (int)frame.size());
    }

    std::cout << "[서버] 클라이언트 연결 종료" << std::endl;
    RemoveClient(s);
}

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
    std::cout << "//    * Date" << std::endl << "//        [2025-08-23]" << std::endl;
    std::cout << "// ───────────────────────────────" << std::endl << std::endl;

    // 1. Winsock 초기화
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "[서버] WSAStartup 실패" << std::endl;
        return 1;
    }

    // 2. Ctrl+C 처리 핸들러 등록
    std::signal(SIGINT, SignalHandler);

    // 3. 리슨 소켓 생성
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET)
    {
        std::cerr << "[서버] socket 실패" << std::endl;
        WSACleanup();
        return 1;
    }

    // (옵션) 빠른 재시작을 위한 SO_REUSEADDR  = allow local address reuse 설정
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // 4. Bind : 서버 주소와 포트를 지정한다
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[서버] bind 실패: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    // 5. Listen 상태 진입 ( 동시 접속 허용 처리)
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[서버] listen 실패: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    std::cout << "[오디오 서버] 포트 " << PORT << " 수신 대기" << std::endl;

    // 6. 메인 루프: 새로운 클라이언트 accept
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

        {
            std::lock_guard<std::mutex> lock(gClientMutex);
            gClients.push_back(s);
        }

        // 클라이언트 별로 전용 스레드를 생성하여 처리한다
        std::thread(ClientThread, s).detach();
        std::cout << "[서버] 클라이언트 접속" << std::endl;
    }

    // 7. 종료 처리: 모든 클라이언트 소켓 닫기
    {
        std::lock_guard<std::mutex> lock(gClientMutex);
        for (SOCKET s : gClients)
        {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        gClients.clear();
    }

    closesocket(listenSock);
    WSACleanup();
    std::cout << "[서버] 정상 종료" << std::endl;
    return 0;
}
