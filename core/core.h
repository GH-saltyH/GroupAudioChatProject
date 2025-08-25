#pragma once

#include <WinSock2.h>						// 기본 소켓 함수 (send, recv, socket 등)
#include <WS2tcpip.h>						// 확장 소켓 기능 (inet_pton 등)
#include <Windows.h>						// win32 API (멀티미디어 캡처, 이벤트 등)
#include <process.h>							// 멀티스레드 (_beginthreadex 등)
#include <atomic>								// 원자적 연산기능 (thread-safe counter)
#include <thread>								// C++11 스레드
#include <mutex>								// 뮤텍스 (스레드 락)
#include <condition_variable>			// 조건 변수 (스레드 동기화)
#include <queue>								// 오디오 송신 큐
#include <vector>
#include <list>
#include <iostream>
#include <string>

// ──────────────────────────────
// 서버 접속 설정
// ──────────────────────────────
#define SERVER_IP "220.116.162.64"				// 서버의 IP 주소 (여기 변경해야 접속 주소 바뀜) 
#define PORT 9797											// 이 서버 규약에서 쓰려는 포트
#define AUDIO_BUFFER_SIZE 3840				// 20ms 단위 버퍼 크기 (48kHz, 16bit, Stereo)

// -------------------------------------------
// 상수 설정
// -------------------------------------------

// 너무 느린 클라이언트 보호 : 큐에 쌓일 수 있는 최대 프레임 수 
// (20ms 프레임 기준 50개면 약 1초 분량)
#define MAX_QUEUE_FRAMES 50

// ──────────────────────────────
// 링킹할 라이브러리 (클라이언트 및 서버 공통)
// ──────────────────────────────
// ws2_32.lib : 윈속 API (소켓)
// winmm.lib  : 오디오 캡처/재생 (waveIn, waveOut)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ──────────────────────────────
// 전역 버퍼 관리
// ──────────────────────────────
// 오디오 재생 시 WAVEHDR 구조체가 필요하며
// waveOutUnprepareHeader 호출 전까지 해제 불가.
// 따라서 안전한 해제를 위해 리스트로 추적.
static std::mutex gBufMutex;
static std::list<WAVEHDR*> gAllocatedBufs;

// ──────────────────────────────
// 안전한 send()
// - TCP는 한번의 send()가 전체 데이터를 보장하지 않음
// - 따라서 루프 돌며 전체 len 만큼 보낼 때까지 반복
// ──────────────────────────────
static bool sendAll(SOCKET s, const char* data, int len)
{
	int sent = 0;
	while (sent < len)
	{
		int n = send(s, data + sent, len - sent, 0);

		// 에러 또는 연결 종료
		if (n <= 0)
			return false;

		// 누적 송신 길이를 갱신한다
		sent += n;
	}
	return true;
}

// ──────────────────────────────
// 안전한 recv()
// - TCP는 한번의 recv()가 원하는 길이만큼 읽지 못할 수 있음
// - 따라서 len 바이트를 다 받을 때까지 반복
// ──────────────────────────────
static bool recvAll(SOCKET s, char* data, int len)
{
	int recvd = 0;
	while (recvd < len)
	{
		int n = recv(s, data + recvd, len - recvd, 0);

		// 에러 또는 연결 종료
		if (n <= 0)
			return false;

		// 누적 수신 길이를 갱신한다
		recvd += n;
	}
	return true;
}


// ──────────────────────────────
// 길이-프리픽스 전송
// 1. 먼저 4바이트 길이 정보(uint32_t)를 네트워크 바이트 오더로 변환해 전송
//    htonl = host to network long
// 2. 그 다음 실제 데이터 payload를 전송
//=> 프레임 경계 보장을 위해 반드시 필요
// ──────────────────────────────
static bool sendFrame(SOCKET s, const char* data, uint32_t len)
{
	// Host byte order --> Netword byte order
	uint32_t nlen = htonl(len);
	if (!sendAll(s, (const char*)&nlen, sizeof(nlen)))
		return false;

	return sendAll(s, data, (int)len);
}

// ──────────────────────────────
// 길이-프리픽스 수신
// 1. 먼저 4바이트 길이 정보 수신 (네트워크 바이트 오더)
//    ntohl = network to host long 이다
// 2. 해당 길이만큼 버퍼를 resize 후 실제 데이터 수신
// 3. 너무 큰 패킷(len > 16MB)은 방어적으로 차단
// 
//  *** 간단히 말해 들어온 패킷이 완전한지 검증하는 절차라고 할 수 있다
// ──────────────────────────────
static bool recvFrame(SOCKET s, std::vector<char>& out)
{
	uint32_t nlen = 0;
	if (!recvAll(s, (char*)&nlen, sizeof(nlen)))
		return false;

	// Network byte order --> Host byte order
	uint32_t len = ntohl(nlen);

	// 너무 큿 패킷은 방어적으로 차단하기 (최대 16MB 제약)
	if (len == 0 || len > 1u << 24)
		return false;

	out.resize(len);
	return recvAll(s, out.data(), (int)len);
}